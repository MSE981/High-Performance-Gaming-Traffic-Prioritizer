#include "App.hpp"
#include "EventCallbacks_util.hpp"
#include "ParameterServices.hpp"
#include "TxFrameOutput_util.hpp"
#include "GUI/Dashboard.hpp"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <dirent.h>
#include <sys/eventfd.h>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <print>
#include <cerrno>
#include <string_view>
#include <mutex>
#include <string>
#include <array>
#include <atomic>
#include <netinet/in.h>
#include <optional>

namespace HPGTP {

namespace {

// Registered data-plane event subscriber
class TelemetryObserver final : public Events_util::PacketObserver {
public:
    void on_frame(std::span<const uint8_t>) override {}
    void on_packet(const Net::ParsedPacket&, Net::Priority) override {}
    void on_batch(const Telemetry::BatchStats& stats, int core_id) override {
        Telemetry::instance().commit_batch(stats, core_id);
    }
    void on_tx_result(Traffic::TxResult, size_t) override {}
};

// RX thread uses poll(2) on the raw socket plus stop_efd. Egress uses TxFrame_util::TxFrameOutput_util.
// L2/ARP snapshot and the kernel WAN address are owned by ForwardState_util::ForwardingState_util.


} // anonymous namespace

// App method definitions


App::App() {
    telemetry_observer_ = std::make_unique<TelemetryObserver>();
    netcfg_ = std::make_unique<NetConfig::NetworkConfig>();

    // Service flags are loaded from config/config.txt before App is constructed;
    nat_engine_     = std::make_unique<Logic::NatEngine>();
    dhcp_engine_    = std::make_unique<Logic::DhcpEngine>(
        Config::ROUTER_IP,
        Logic::DhcpPoolConfig{
            Net::parse_ipv4(Config::DHCP_POOL_START.c_str()),
            Net::parse_ipv4(Config::DHCP_POOL_END.c_str()),
            Config::DHCP_LEASE_DURATION});
}

App::~App() {
    // Stop data-plane workers (Cores 2/3) before control service threads so they do not read stale shaper state after workers exit.
    if (forward_engine_) forward_engine_->stop();
    if (control_plane_) control_plane_->stop();
}



void App::stop() {
    if (!lifecycle_.begin_stop()) return;
    // Stop data-plane workers before control service threads so they do not read stale shaper state.
    if (forward_engine_) forward_engine_->stop();
    if (control_plane_) control_plane_->stop();
    lifecycle_.mark_complete();
}

std::expected<void, std::string> App::init() {
    Utils_util::Network_util::disable_hardware_offloads(Config::iface_wan());
    Utils_util::Network_util::disable_hardware_offloads(Config::iface_lan());
    iface_wan = std::make_unique<Engine::RawSocketManager>(Config::iface_wan());
    iface_lan = std::make_unique<Engine::RawSocketManager>(Config::iface_lan());
    if (auto r = iface_wan->init(); !r) return r;
    if (auto r = iface_lan->init(); !r) return r;

    if (auto s = netcfg_->sync(*dhcp_engine_); !s) return s;

    if (Config::global_state.enable_nat.load(std::memory_order_relaxed)) {
        auto w = forwarding_plane_.resolve_nat_wan_ip();
        if (!w) return std::unexpected(w.error());
        nat_engine_->set_wan_ip(*w);
        std::println("[App] NAT WAN address: {}", Config::ip_to_str(*w));
    } else {
        nat_engine_->set_wan_ip(Net::IPv4Net{});
    }
    return {};
}

void App::start() {
    std::println("=== High-performance gaming traffic prioritizer (software router) ===");
    auto& tel = Telemetry::instance();
    const bool accel = Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed);
    tel.acceleration_pending.store(accel, std::memory_order_relaxed);
    tel.mode_config_dirty.store(false, std::memory_order_relaxed);
    tel.effective_acceleration.store(accel, std::memory_order_release);
    tel.effective_bridge_mode.store(!accel, std::memory_order_release);

    HPGTP::System::Optimizer_util::lock_cpu_frequency();

    int fd_wan = iface_wan->get_fd();
    int fd_lan = iface_lan->get_fd();
    lan_fd_ = fd_lan;

    base_dl_mbps = tel.qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
    base_ul_mbps = tel.qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
    tel.effective_qos_global_dl_mbps.store(base_dl_mbps, std::memory_order_release);
    tel.effective_qos_global_ul_mbps.store(base_ul_mbps, std::memory_order_release);
    shaper_dl_ = std::make_unique<Traffic::Shaper>(Traffic::Mbps{base_dl_mbps});
    shaper_ul_ = std::make_unique<Traffic::Shaper>(Traffic::Mbps{base_ul_mbps});

    dl_events_.register_observer(telemetry_observer_.get());
    ul_events_.register_observer(telemetry_observer_.get());

    shaper_dl_->set_tx_result_callback(
        Traffic::TxResultCallback([this](Traffic::TxResult result, size_t bytes) {
            dl_events_.dispatch_tx_result(result, bytes);
            if (result == Traffic::TxResult::Fatal)
                std::println(stderr, "[App] downstream shaper TX callback: fatal after {} bytes", bytes);
        }));
    shaper_ul_->set_tx_result_callback(
        Traffic::TxResultCallback([this](Traffic::TxResult result, size_t bytes) {
            ul_events_.dispatch_tx_result(result, bytes);
            if (result == Traffic::TxResult::Fatal)
                std::println(stderr, "[App] upstream shaper TX callback: fatal after {} bytes", bytes);
        }));

    control_plane_ = std::make_unique<Control::ControlPlane>(
        tel, *nat_engine_, *dhcp_engine_, lan_fd_, shaper_dl_.get(), shaper_ul_.get(),
        base_dl_mbps, base_ul_mbps, [this]() {
            if (auto s = netcfg_->sync(*dhcp_engine_); !s)
                std::println(stderr, "[DHCP] LAN subnet sync after pool change: {}", s.error());
            if (auto sr = Config::save_config(); !sr)
                std::println(stderr, "[Config] save after DHCP change: {}", sr.error());
        });
    control_plane_->start(forwarding_plane_);

    forwarding_plane_.refresh_l2();
    if (!forwarding_plane_.snapshot().ready)
        std::println(stderr,
            "[App] Warning: Ethernet MACs for L3 forwarding not ready (interfaces or default route / ARP).");

    forward_engine_ = std::make_unique<ForwardEngine::Forward_Engine>();
    if (auto pr = forward_engine_->start(std::move(iface_wan), std::move(iface_lan),
            fd_wan, fd_lan, shaper_dl_.get(), shaper_ul_.get(),
            nat_engine_.get(), dhcp_engine_.get(), &dl_events_, &ul_events_,
            forwarding_plane_); !pr) {
        std::println(stderr, "[App] {}", pr.error());
        return;
    }

    std::println("[App] Data plane and control plane started.");
}

void App::wait_for_shutdown() {
    lifecycle_.wait();
    std::println("\n[System] GUI shutdown requested, core services terminated gracefully.");
}


void App::request_dhcp_config_apply() {
    if (control_plane_)
        control_plane_->request_dhcp_config_apply();
}

} // namespace HPGTP
