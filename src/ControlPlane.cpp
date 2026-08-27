#include "ControlPlane.hpp"
#include "Config.hpp"
#include "Network_util.hpp"
#include "SystemOptimizer_util.hpp"
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <print>

namespace HPGTP::Control {

namespace {
int wait_watchdog_period(int stop_efd, int period_ms) {
    struct pollfd pfd{stop_efd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, period_ms);
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if ((pfd.revents & POLLIN) != 0) {
        uint64_t v{};
        (void)::eventfd_read(stop_efd, &v);
        return 1;
    }
    return 0;
}
} // anonymous

ControlPlane::ControlPlane(Telemetry& tel, Logic::NatEngine& nat, Logic::DhcpEngine& dhcp,
                         int lan_fd, Traffic::Shaper* dl, Traffic::Shaper* ul,
                         double base_dl, double base_ul,
                         std::function<void()> dhcp_applied)
    : tel_(tel), nat_(nat), dhcp_(dhcp), lan_fd_(lan_fd),
      dl_(dl), ul_(ul), base_dl_(base_dl), base_ul_(base_ul),
      dhcp_applied_(std::move(dhcp_applied)) {}

void ControlPlane::start(ForwardState_util::ForwardingState_util& plane) {
    plane_ = &plane;
    if (watchdog_stop_efd_ < 0) {
        watchdog_stop_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        if (watchdog_stop_efd_ < 0) {
            std::println(stderr, "[Fatal] watchdog stop eventfd: {}", std::strerror(errno));
            std::exit(1);
        }
    }
    if (dhcp_cfg_efd_ < 0) {
        dhcp_cfg_efd_ = ::eventfd(0, EFD_CLOEXEC);
        if (dhcp_cfg_efd_ < 0) {
            std::println(stderr, "[Fatal] DHCP config eventfd: {}", std::strerror(errno));
            std::exit(1);
        }
    }
    running_.store(true, std::memory_order_relaxed);
    service_threads_[0] = std::thread(&ControlPlane::watchdog_telemetry_loop, this);
    service_threads_[1] = std::thread(&ControlPlane::watchdog_l2_refresh_loop, this);
    service_threads_[2] = std::thread(&ControlPlane::watchdog_wan_tracker_loop, this);
    service_threads_[3] = std::thread(&ControlPlane::watchdog_dhcp_worker_loop, this);
    service_threads_[4] = std::thread(&ControlPlane::watchdog_qos_loop, this);
    service_threads_[5] = std::thread(&ControlPlane::watchdog_nat_ticker_loop, this);
    control_event_thread_ = std::thread(&ControlPlane::control_event_loop, this);
}

void ControlPlane::stop() {
    running_.store(false, std::memory_order_relaxed);
    wake_watchdog();
    for (auto& t : service_threads_) if (t.joinable()) t.join();
    if (control_event_thread_.joinable()) control_event_thread_.join();
    close_watchdog_stop_efd();
    close_dhcp_cfg_efd();
}

ControlPlane::~ControlPlane() {
    stop();
}

void ControlPlane::request_dhcp_config_apply() {
    if (dhcp_cfg_efd_ >= 0)
        (void)::eventfd_write(dhcp_cfg_efd_, 1);
}

void ControlPlane::wake_watchdog() {
    if (watchdog_stop_efd_ >= 0)
        (void)::eventfd_write(watchdog_stop_efd_, WATCHDOG_SERVICE_COUNT + 1);
}

void ControlPlane::close_watchdog_stop_efd() {
    if (watchdog_stop_efd_ >= 0) {
        ::close(watchdog_stop_efd_);
        watchdog_stop_efd_ = -1;
    }
}

void ControlPlane::close_dhcp_cfg_efd() {
    if (dhcp_cfg_efd_ >= 0) {
        ::close(dhcp_cfg_efd_);
        dhcp_cfg_efd_ = -1;
    }
}

void ControlPlane::applyDhcpConfig() {
    auto pool_s = Config::parse_ip_str(Config::DHCP_POOL_START);
    auto pool_e = Config::parse_ip_str(Config::DHCP_POOL_END);
    if (!pool_s || !pool_e) {
        std::println(stderr, "[DHCP] Invalid pool bounds: {} / {}",
            pool_s ? "" : pool_s.error(), pool_e ? "" : pool_e.error());
        return;
    }
    if (auto dr = dhcp_.reconfigure({*pool_s, *pool_e, Config::DHCP_LEASE_DURATION}); !dr) {
        std::println(stderr, "[DHCP] reconfigure failed: {}", dr.error());
        return;
    }
    if (dhcp_applied_) dhcp_applied_();
}

void ControlPlane::watchdog_telemetry_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    auto& tel = Telemetry::instance();
    ParameterServices::TelemetryCollector telemetry(tel);
    uint64_t tick5 = 0;
    uint8_t prev_pe = 0;
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        telemetry.tick1Hz();
        if (++tick5 % 5 == 0) telemetry.tick5s();
        const uint8_t pe = tel.raw_socket_poll_errors.load(std::memory_order_relaxed);
        if (pe != prev_pe && pe != 0) {
            std::println(stderr, "[App] Packet RX poll failure (telemetry mask {})", pe);
            prev_pe = pe;
        }
    }
}

void ControlPlane::watchdog_l2_refresh_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    ParameterServices::L2ForwardRefresher l2_refresher([this]() { plane_->refresh_l2(); });
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 5000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        l2_refresher.tick5s();
    }
}

void ControlPlane::watchdog_wan_tracker_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    ParameterServices::NatWanTracker wan_tracker(nat_,
        [this]() { return Utils_util::Network_util::get_local_ip(Config::iface_wan()); });
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 5000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        wan_tracker.tick5s();
    }
}

void ControlPlane::watchdog_dhcp_worker_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    ParameterServices::DhcpWorker dhcp_worker(dhcp_, lan_fd_,
        [this]() {
            const std::string s = Utils_util::Network_util::get_local_ip(Config::iface_lan());
            if (s.empty()) return;
            auto e = Config::parse_ip_str(s);
            if (e && *e != dhcp_.router_ip_snapshot()) dhcp_.set_router_ip(*e);
        });
    uint64_t tick5 = 0;
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        dhcp_worker.tick1Hz();
        if (++tick5 % 5 == 0) dhcp_worker.tick5s();
    }
}

void ControlPlane::watchdog_qos_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    auto& tel = Telemetry::instance();
    ParameterServices::QosController qos(tel, dl_, ul_,
                            base_dl_, base_ul_);
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        qos.tick1Hz();
    }
}

void ControlPlane::watchdog_nat_ticker_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    ParameterServices::EngineTicker ticker(nat_);
    while (running_.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_.load(std::memory_order_acquire)) break;
        ticker.tick1Hz();
    }
}

// Event-driven control thread: GUI writes dhcp_cfg_efd_; this thread applies
// the DHCP config. Also handles the shared shutdown token.
void ControlPlane::control_event_loop() {
    HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
    while (running_.load(std::memory_order_acquire)) {
        struct pollfd pfds[2]{};
        pfds[0] = { dhcp_cfg_efd_, POLLIN, 0 };
        pfds[1] = { watchdog_stop_efd_, POLLIN, 0 };
        const int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((pfds[1].revents & POLLIN) != 0) {
            uint64_t v{};
            (void)::eventfd_read(watchdog_stop_efd_, &v);
            break;
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            uint64_t v{};
            (void)::eventfd_read(dhcp_cfg_efd_, &v);
            if (running_.load(std::memory_order_acquire))
                applyDhcpConfig();
        }
    }
}

} // namespace HPGTP::Control
