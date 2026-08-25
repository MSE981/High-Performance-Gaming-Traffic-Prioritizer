#include "App.hpp"
#include "EventCallbacks.hpp"
#include "WatchdogServices.hpp"
#include "DataPlane.hpp"
#include "GUI/Dashboard.hpp"
// POSIX C headers, visible only in this translation unit.
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

// Registered data-plane event subscriber: commits batched telemetry. The frame/
// packet/tx hooks are reserved so future clients can subscribe without touching
// the forwarding pipeline.
class TelemetryObserver final : public Events::PacketObserver {
public:
    void on_frame(std::span<const uint8_t>) override {}
    void on_packet(const Net::ParsedPacket&, Net::Priority) override {}
    void on_batch(const Telemetry::BatchStats& stats, int core_id) override {
        Telemetry::instance().commit_batch(stats, core_id);
    }
    void on_tx_result(Traffic::TxResult, size_t) override {}
};

// RX thread uses poll(2) on the raw socket plus stop_efd (-1 timeout). Egress uses DataPlane::TxFrameOutput.
// L2/ARP snapshot and the kernel WAN address are owned by DataPlane::ForwardingPlane.

// Packet routing context (internal to data plane)
struct RouteContext {
    int tx_fd;
    Traffic::Shaper* shaper;
};
using RouteFunc = void (*)(const RouteContext&, std::span<uint8_t>, size_t, int);

// Data-plane route handlers

void fast_path_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                        size_t prio_idx, int core_id) {
    DataPlane::TxFrameOutput::send_best_effort(ctx.tx_fd, pkt, core_id, prio_idx);
}

void shaper_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                     size_t /*prio_idx*/, int /*core_id*/) {
    if (ctx.shaper) ctx.shaper->enqueue_normal(pkt);
}

// PacketConsumer
// Internal data-plane class: assembled callback pipeline, zero runtime
// branching. Hidden from all App clients.

class PacketConsumer {
public:
    int rx_fd;
    int tx_fd;
    int core_id;
    Telemetry::BatchStats          stats;
    Logic::HeuristicProcessor      processor;
    RouteContext                   ctx;
    Logic::NatEngine*              nat_engine;
    Logic::DhcpEngine*             dhcp_engine;
    Events::CallbackRegistry&      callbacks;
    DataPlane::ForwardingPlane&       plane;

    std::array<std::array<RouteFunc, 2>, 2> routes;

    using PipelineStep = bool (*)(PacketConsumer&, Net::ParsedPacket&);

    // Ordered pipeline stages; each step returns true if it handled the packet.
    struct PacketPipeline {
        std::array<PipelineStep, 12> steps{};
    };
    PacketPipeline pipeline;

    PacketConsumer(int rx_fd_, const PacketWorkerConfig& cfg,
                   DataPlane::ForwardingPlane& plane_)
        : rx_fd(rx_fd_), tx_fd(cfg.tx_fd), core_id(cfg.core_id),
          ctx{cfg.tx_fd, cfg.route_shaper},
          nat_engine(cfg.nat_engine),
          dhcp_engine(cfg.dhcp_engine),
          callbacks(*cfg.callbacks),
          plane(plane_) {

        routes = {{
            { fast_path_handler, shaper_handler }, // acceleration: High fast, Normal throttled
            { fast_path_handler, fast_path_handler } // bridge: everything unthrottled
        }};

        // Pipeline steps are fixed at construction (no per-packet branch to select a path).
        if (core_id == 2) {
            // Core 2 WAN→LAN: DNAT, then L2 rewrite and QoS routing.
            pipeline.steps = {{
                step_dhcp_interceptor,
                step_nat_downstream,
                step_eth_rewrite_wan_to_lan,
                step_qos_routing,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
            }};
        } else {
            // Core 3 LAN→WAN: SNAT before sending upstream.
            pipeline.steps = {{
                step_dhcp_interceptor,
                step_nat_downstream,
                step_nat_upstream, step_eth_rewrite_lan_to_wan,
                step_qos_routing,
                nullptr, nullptr, nullptr, nullptr, nullptr
            }};
        }
    }

    // Pipeline steps

    static bool step_dhcp_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_dhcp.load(std::memory_order_relaxed)) return false;
        if (self.core_id == 3 && pkt.is_valid_ipv4() && pkt.l4_protocol == 17) {
            auto udp = pkt.udp();
            if (udp && (ntohs(udp->dest) == 67 || ntohs(udp->dest) == 68)) {
                if (self.dhcp_engine) self.dhcp_engine->intercept_request(pkt);
                return true;
            }
        }
        return false;
    }

    static bool step_nat_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        if (self.nat_engine) self.nat_engine->process_inbound(pkt);
        return false;
    }

    static bool step_nat_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        if (self.nat_engine) self.nat_engine->process_outbound(pkt);
        return false;
    }

    static bool ipv4_needs_eth_rewrite_for_forward(Net::IPv4Header* ip) noexcept {
        const uint32_t d = ntohl(ip->daddr.raw());
        if (d == 0xFFFFFFFFu) return false;
        if ((d >> 24) >= 224u) return false;
        return true;
    }

    static bool step_eth_rewrite_lan_to_wan(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        const auto& s = self.plane.snapshot();
        if (!s.ready || !pkt.eth || !pkt.ipv4) return false;
        if (!ipv4_needs_eth_rewrite_for_forward(pkt.ipv4)) return false;

        const Net::IPv4Net dst = pkt.ipv4->daddr;
        bool on_link = false;
        if (s.wan_cfg_valid)
            on_link = Utils::Network::ipv4_in_subnet(
                dst, static_cast<int>(s.wan_prefix_len), Net::IPv4Net{s.wan_ip_nbo});

        std::memcpy(pkt.eth->src, s.wan_hw.data(), 6);

        if (on_link) {
            uint8_t nh[6]{};
            if (!self.plane.resolve_mac_onlink(s, dst.raw(), nh)) return true;
            std::memcpy(pkt.eth->dest, nh, 6);
            return false;
        }

        if (!s.default_gw_ip_configured) return true;
        if (!s.gw_hw_valid) return true;
        std::memcpy(pkt.eth->dest, s.gw_hw.data(), 6);
        return false;
    }

    static bool step_eth_rewrite_wan_to_lan(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        const auto& s = self.plane.snapshot();
        if (!s.ready || !pkt.eth || !pkt.ipv4) return false;
        if (!ipv4_needs_eth_rewrite_for_forward(pkt.ipv4)) return false;
        const uint32_t key = pkt.ipv4->daddr.raw();
        for (uint32_t i = 0; i < s.arp_count; ++i) {
            if (s.arp[i].ip_nbo == key) {
                std::memcpy(pkt.eth->src, s.lan_hw.data(), 6);
                std::memcpy(pkt.eth->dest, s.arp[i].mac.data(), 6);
                return false;
            }
        }
        return false;
    }

    static bool step_qos_routing(PacketConsumer& self, Net::ParsedPacket& pkt) {
        auto prio       = self.processor.process(pkt);
        const size_t pi = static_cast<size_t>(prio);
        self.stats.pkts++;
        self.stats.bytes += pkt.raw_span.size();
        self.stats.prio_pkts[pi]++;
        self.stats.prio_bytes[pi] += pkt.raw_span.size();
        self.callbacks.dispatch_packet(pkt, prio);
        size_t mode = Telemetry::instance().effective_bridge_mode.load(std::memory_order_acquire) ? 1U : 0U;
        self.routes[mode][pi](self.ctx, pkt.raw_span, pi, self.core_id);
        return true;
    }

    // Packet entry point
    void on_packet_event(Net::ParsedPacket& pkt) {
        for (auto* step : pipeline.steps)
            if (step && step(*this, pkt)) break;
        // Batch-commit telemetry every 32 packets (& 31 avoids division)
        if ((stats.pkts & 31) == 0) {
            callbacks.dispatch_batch(stats, core_id);
            stats.reset();
        }
    }
};

// Fixed-size copy of one Ethernet frame for transfer from the RX thread to the
// packet processing thread (single producer, single consumer ring).
struct RxFrameCopy {
    std::array<uint8_t, 2048> data{};
    uint16_t                len = 0;
};

// Blocks for period_ms or until the shared watchdog stop eventfd has a token.
// Returns 0 on timeout, 1 on stop, -1 on poll failure.
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

} // anonymous namespace

// App method definitions

std::expected<void, std::string> App::sync_lan_subnet_and_dhcp_gateway() {
    auto parse_kern = [](const std::string& s) -> std::optional<Net::IPv4Net> {
        if (s.empty()) return std::nullopt;
        auto e = Config::parse_ip_str(s);
        if (!e) return std::nullopt;
        return *e;
    };

    if (!Config::global_state.enable_dhcp.load(std::memory_order_relaxed)) {
        if (Config::APPLY_ROUTER_IP_TO_LAN && Config::LAN_PREFIX_LEN > 0 && Config::LAN_PREFIX_LEN <= 32) {
            if (Config::parse_ip_str(Config::ROUTER_IP)) {
                if (!Utils::Network::set_iface_ipv4_and_prefix(
                        Config::iface_lan(), Config::ROUTER_IP, Config::LAN_PREFIX_LEN))
                    std::println(stderr, "[App] set_iface ROUTER_IP on {} failed: {}",
                        Config::iface_lan(), std::strerror(errno));
            }
        }
        auto re = Config::parse_ip_str(Config::ROUTER_IP);
        if (!re) return std::unexpected(std::string("ROUTER_IP: ") + re.error());
        const std::string k = Utils::Network::get_local_ip(Config::iface_lan());
        effective_lan_gateway_ = parse_kern(k).value_or(*re);
        if (dhcp_engine_) dhcp_engine_->set_router_ip(effective_lan_gateway_);
        return {};
    }

    auto pstart_e = Config::parse_ip_str(Config::DHCP_POOL_START);
    auto pend_e   = Config::parse_ip_str(Config::DHCP_POOL_END);
    if (!pstart_e) return std::unexpected(std::string("DHCP_POOL_START: ") + pstart_e.error());
    if (!pend_e) return std::unexpected(std::string("DHCP_POOL_END: ") + pend_e.error());
    const Net::IPv4Net pool_start = *pstart_e;
    const Net::IPv4Net pool_end   = *pend_e;
    if (ntohl(pool_start.raw()) > ntohl(pool_end.raw()))
        return std::unexpected(std::string("DHCP pool start is after pool end"));

    const int prefix = Utils::Network::infer_prefix_covering_pool(pool_start, pool_end);
    if (prefix > 32)
        return std::unexpected(std::string("invalid DHCP pool prefix derivation"));

    if (Config::LAN_PREFIX_LEN > 0 && Config::LAN_PREFIX_LEN <= 32
        && Config::LAN_PREFIX_LEN != prefix)
        std::println(stderr,
            "[App] LAN_PREFIX_LEN={} differs from pool-derived prefix {}; using derived for subnet and ioctl.",
            Config::LAN_PREFIX_LEN, prefix);

    auto router_e = Config::parse_ip_str(Config::ROUTER_IP);
    if (!router_e) return std::unexpected(std::string("ROUTER_IP: ") + router_e.error());

    if (Config::APPLY_ROUTER_IP_TO_LAN) {
        if (!Utils::Network::set_iface_ipv4_and_prefix(
                Config::iface_lan(), Config::ROUTER_IP, prefix)) {
            std::println(stderr, "[App] set_iface ROUTER_IP on {} failed: {}",
                Config::iface_lan(), std::strerror(errno));
        }
    }

    auto in_pool_subnet = [&](Net::IPv4Net ip) {
        return Utils::Network::ipv4_in_subnet(ip, prefix, pool_start);
    };

    std::string       kern_s = Utils::Network::get_local_ip(Config::iface_lan());
    std::optional<Net::IPv4Net> kern = parse_kern(kern_s);

    bool ok = kern.has_value() && in_pool_subnet(*kern);
    if (!ok) {
        if (in_pool_subnet(*router_e)) {
            if (!Utils::Network::set_iface_ipv4_and_prefix(
                    Config::iface_lan(), Config::ROUTER_IP, prefix))
                return std::unexpected(std::string("set_iface ROUTER_IP: ") + std::strerror(errno));
            kern_s = Utils::Network::get_local_ip(Config::iface_lan());
            kern   = parse_kern(kern_s);
            ok     = kern.has_value() && in_pool_subnet(*kern);
        } else {
            return std::unexpected(
                std::string("ROUTER_IP is not in the DHCP pool subnet (derived /") + std::to_string(prefix)
                + "); fix ROUTER_IP or DHCP_POOL_*");
        }
    }

    if (!ok || !kern.has_value())
        return std::unexpected(
            std::string("IFACE_LAN has no IPv4 in the DHCP pool subnet; set APPLY_ROUTER_IP_TO_LAN=true "
                        "with ROUTER_IP inside the pool subnet, or assign manually."));

    effective_lan_gateway_ = *kern;
    if (dhcp_engine_) dhcp_engine_->set_router_ip(effective_lan_gateway_);
    std::println("[App] DHCP gateway {} (kernel LAN, pool subnet /{})",
        Config::ip_to_str(effective_lan_gateway_), prefix);
    return {};
}

void App::refresh_dhcp_router_from_kernel() noexcept {
    if (!dhcp_engine_ || !Config::global_state.enable_dhcp.load(std::memory_order_relaxed))
        return;
    const std::string s = Utils::Network::get_local_ip(Config::iface_lan());
    if (s.empty()) return;
    auto e = Config::parse_ip_str(s);
    if (!e) return;
    if (*e != dhcp_engine_->router_ip_snapshot()) {
        dhcp_engine_->set_router_ip(*e);
    }
}

App::App() {
    telemetry_observer_ = std::make_unique<TelemetryObserver>();

    // Service flags are loaded from config/config.txt before App is constructed;
    // do not override them here.
    nat_engine_     = std::make_unique<Logic::NatEngine>();
    dhcp_engine_    = std::make_unique<Logic::DhcpEngine>(
        Config::ROUTER_IP,
        Logic::DhcpPoolConfig{
            Net::parse_ipv4(Config::DHCP_POOL_START.c_str()),
            Net::parse_ipv4(Config::DHCP_POOL_END.c_str()),
            Config::DHCP_LEASE_DURATION});
}

App::~App() {
    // Stop data-plane workers (Cores 2/3) before control service threads so
    // they do not read stale shaper state after workers exit.
    running_workers.store(false, std::memory_order_relaxed);
    wake_proc_threads_for_shutdown();
    if (worker_downstream.joinable()) worker_downstream.join();
    if (worker_upstream.joinable())   worker_upstream.join();
    close_worker_poll_fds();
    running_watchdog.store(false, std::memory_order_relaxed);
    wake_watchdog_for_shutdown();
    for (auto& t : service_threads) if (t.joinable()) t.join();
    if (control_event_thread.joinable()) control_event_thread.join();
    close_watchdog_stop_efd();
    close_dhcp_cfg_efd();
}

std::expected<void, std::string> App::open_worker_poll_fds_for_start() {
    close_worker_poll_fds();
    for (auto& w : worker_poll_) {
        w.frame_efd = ::eventfd(0, EFD_CLOEXEC);
        // Semaphore mode: RX and proc threads both poll+read the same stop_efd; a single
        // non-semaphore write(1) wakes both but only one read drains - the other blocks
        // forever on read(), so App::stop() hangs on worker join.
        w.stop_efd  = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        if (w.frame_efd < 0 || w.stop_efd < 0) {
            int e = errno;
            close_worker_poll_fds();
            return std::unexpected(
                std::string("eventfd for worker poll sync failed: ") + std::strerror(e));
        }
    }
    return {};
}

void App::close_worker_poll_fds() {
    for (auto& w : worker_poll_) {
        if (w.frame_efd >= 0) {
            ::close(w.frame_efd);
            w.frame_efd = -1;
        }
        if (w.stop_efd >= 0) {
            ::close(w.stop_efd);
            w.stop_efd = -1;
        }
    }
}

void App::wake_proc_threads_for_shutdown() {
    for (auto& w : worker_poll_) {
        if (w.stop_efd >= 0)
            // Two readers per worker (RX thread + proc thread); EFD_SEMAPHORE needs one
            // increment per successful read.
            (void)::eventfd_write(w.stop_efd, 2);
    }
}

void App::wake_watchdog_for_shutdown() {
    if (watchdog_stop_efd_ >= 0)
        (void)::eventfd_write(watchdog_stop_efd_, WATCHDOG_SERVICE_COUNT + 1);
}

void App::close_watchdog_stop_efd() {
    if (watchdog_stop_efd_ >= 0) {
        ::close(watchdog_stop_efd_);
        watchdog_stop_efd_ = -1;
    }
}

void App::close_dhcp_cfg_efd() {
    if (dhcp_cfg_efd_ >= 0) {
        ::close(dhcp_cfg_efd_);
        dhcp_cfg_efd_ = -1;
    }
}

void App::applyDhcpConfig() {
    if (!dhcp_engine_) return;
    auto pool_s = Config::parse_ip_str(Config::DHCP_POOL_START);
    auto pool_e = Config::parse_ip_str(Config::DHCP_POOL_END);
    if (!pool_s || !pool_e) {
        std::println(stderr, "[DHCP] Invalid pool bounds: {} / {}",
            pool_s ? "" : pool_s.error(), pool_e ? "" : pool_e.error());
        return;
    }
    if (auto dr = dhcp_engine_->reconfigure({*pool_s, *pool_e, Config::DHCP_LEASE_DURATION}); !dr) {
        std::println(stderr, "[DHCP] reconfigure failed: {}", dr.error());
        return;
    }
    if (auto s = sync_lan_subnet_and_dhcp_gateway(); !s)
        std::println(stderr, "[DHCP] LAN subnet sync after pool change: {}", s.error());
    if (auto sr = Config::save_config(); !sr)
        std::println(stderr, "[Config] save after DHCP change: {}", sr.error());
}

void App::stop() {
    if (shutdown_sequence_started_.exchange(true, std::memory_order_acq_rel)) return;
    // Stop data-plane workers (Cores 2/3) before control service threads so
    // they do not read stale shaper state after workers exit.
    running_workers.store(false, std::memory_order_relaxed);
    wake_proc_threads_for_shutdown();
    if (worker_downstream.joinable()) worker_downstream.join();
    if (worker_upstream.joinable()) worker_upstream.join();
    close_worker_poll_fds();
    running_watchdog.store(false, std::memory_order_release);
    wake_watchdog_for_shutdown();
    for (auto& t : service_threads) if (t.joinable()) t.join();
    if (control_event_thread.joinable()) control_event_thread.join();
    close_watchdog_stop_efd();
    {
        std::lock_guard lock(shutdown_mutex_);
        shutdown_complete_.store(true, std::memory_order_release);
    }
    shutdown_cv_.notify_all();
}

std::expected<void, std::string> App::init() {
    Utils::Network::disable_hardware_offloads(Config::iface_wan());
    Utils::Network::disable_hardware_offloads(Config::iface_lan());
    iface_wan = std::make_unique<Engine::RawSocketManager>(Config::iface_wan());
    iface_lan = std::make_unique<Engine::RawSocketManager>(Config::iface_lan());
    if (auto r = iface_wan->init(); !r) return r;
    if (auto r = iface_lan->init(); !r) return r;

    if (auto s = sync_lan_subnet_and_dhcp_gateway(); !s) return s;

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

    HPGTP::System::Optimizer::lock_cpu_frequency();

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

    if (auto pr = open_worker_poll_fds_for_start(); !pr) {
        std::println(stderr, "[App] {}", pr.error());
        return;
    }

    if (watchdog_stop_efd_ < 0) {
        watchdog_stop_efd_ = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        if (watchdog_stop_efd_ < 0) {
            std::println(stderr, "[Fatal] watchdog stop eventfd: {}",
                std::strerror(errno));
            std::exit(1);
        }
    }
    if (dhcp_cfg_efd_ < 0) {
        dhcp_cfg_efd_ = ::eventfd(0, EFD_CLOEXEC);
        if (dhcp_cfg_efd_ < 0) {
            std::println(stderr, "[Fatal] DHCP config eventfd: {}",
                std::strerror(errno));
            std::exit(1);
        }
    }
    running_watchdog.store(true, std::memory_order_relaxed);
    service_threads[0] = std::thread(&App::watchdog_telemetry_loop, this);
    service_threads[1] = std::thread(&App::watchdog_l2_refresh_loop, this);
    service_threads[2] = std::thread(&App::watchdog_wan_tracker_loop, this);
    service_threads[3] = std::thread(&App::watchdog_dhcp_worker_loop, this);
    service_threads[4] = std::thread(&App::watchdog_qos_loop, this);
    service_threads[5] = std::thread(&App::watchdog_nat_ticker_loop, this);
    control_event_thread = std::thread(&App::control_event_loop, this);

    forwarding_plane_.refresh_l2();
    if (!forwarding_plane_.snapshot().ready)
        std::println(stderr,
            "[App] Warning: Ethernet MACs for L3 forwarding not ready (interfaces or default route / ARP).");

    running_workers.store(true, std::memory_order_relaxed);

    worker_downstream = std::thread(
        [this, ps = &worker_poll_[0]](std::unique_ptr<Engine::RawSocketManager> iface,
                                       PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(iface_wan),
        PacketWorkerConfig{ fd_lan, 2, shaper_dl_.get(), nat_engine_.get(),
                            dhcp_engine_.get(), &dl_events_ });

    worker_upstream = std::thread(
        [this, ps = &worker_poll_[1]](std::unique_ptr<Engine::RawSocketManager> iface,
                                       PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(iface_lan),
        PacketWorkerConfig{ fd_wan, 3, shaper_ul_.get(), nat_engine_.get(),
                            dhcp_engine_.get(), &ul_events_ });

    std::println("[App] Data plane and control plane started.");
}

void App::wait_for_shutdown() {
    std::unique_lock lock(shutdown_mutex_);
    shutdown_cv_.wait(lock, [this]() {
        return shutdown_complete_.load(std::memory_order_acquire);
    });
    std::println("\n[System] GUI shutdown requested, core services terminated gracefully.");
}

// Worker event loop (Core 2 / Core 3)
// RX thread: blocking poll(2) on AF_PACKET and stop_efd; copy each frame into an
// SPSC ring. Processing thread: parse and run PacketConsumer::on_packet_event;
// waits on frame_efd/stop_efd with no periodic timeout.

void App::worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr,
                             PacketWorkerConfig cfg,
                             WorkerPollSync& poll_sync) {
    HPGTP::System::Optimizer::set_current_thread_affinity_control(); // container: control cores 0-1
    std::println("[App] Core {} pipeline mounted and ready.", cfg.core_id);

    const int rx_fd_saved = rx_mgr->get_fd();
    PacketConsumer consumer(rx_fd_saved, cfg, forwarding_plane_);
    Net::SpscRingBuffer<RxFrameCopy, 1024> frame_q{};

    std::thread rx_thread(
        [this, mgr = std::move(rx_mgr), &frame_q, &poll_sync, core = cfg.core_id]() mutable {
            HPGTP::System::Optimizer::set_current_thread_affinity(core);
            HPGTP::System::Optimizer::set_realtime_priority();
            const int sock_fd = mgr->get_fd();
            while (this->running_workers.load(std::memory_order_relaxed)) {
                struct pollfd pfds_rx[2]{};
                pfds_rx[0] = { sock_fd, POLLIN, 0 };
                pfds_rx[1] = { poll_sync.stop_efd, POLLIN, 0 };
                int prx = ::poll(pfds_rx, 2, -1);
                if (prx < 0) {
                    if (errno == EINTR) continue;
                    const int e = errno;
                    mgr->notify_rx_poll_fatal(e, 2);
                    std::println(stderr, "[App] Core {} RX poll failed: {}",
                        core, std::strerror(e));
                    break;
                }
                if ((pfds_rx[1].revents & POLLIN) != 0) {
                    uint64_t v;
                    (void)::eventfd_read(poll_sync.stop_efd, &v);
                    break;
                }
                if ((pfds_rx[0].revents & POLLIN) == 0) continue;

                std::span<uint8_t> raw;
                while (this->running_workers.load(std::memory_order_relaxed)
                       && mgr->peek_rx_frame(raw)) {
                    RxFrameCopy copy{};
                    const size_t n =
                        raw.size() < copy.data.size() ? raw.size() : copy.data.size();
                    copy.len = static_cast<uint16_t>(n);
                    std::memcpy(copy.data.data(), raw.data(), n);

                    if (frame_q.push(copy)) {
                        if (poll_sync.frame_efd >= 0)
                            (void)::eventfd_write(poll_sync.frame_efd, 1);
                        mgr->finish_rx_frame();
                    } else {
                        Telemetry::instance().core_metrics[core].dropped[0].fetch_add(
                            1, std::memory_order_relaxed);
                        mgr->finish_rx_frame();
                    }
                }
            }
        });

    std::thread proc_thread([this, &consumer, &frame_q, &poll_sync, cfg]() {
        HPGTP::System::Optimizer::set_current_thread_affinity(cfg.core_id);
        HPGTP::System::Optimizer::set_realtime_priority();
        while (this->running_workers.load(std::memory_order_relaxed)) {
            RxFrameCopy copy{};
            while (frame_q.pop(copy)) {
                consumer.callbacks.dispatch_frame(
                    std::span<const uint8_t>(copy.data.data(), copy.len));
                auto pkt = Net::ParsedPacket::parse(
                    std::span<uint8_t>(copy.data.data(), copy.len));
                consumer.on_packet_event(pkt);
            }

            if (cfg.route_shaper) cfg.route_shaper->process_queue(cfg.tx_fd);

            if (poll_sync.frame_efd < 0 || poll_sync.stop_efd < 0) break;

            struct pollfd pfds[2]{};
            pfds[0] = { poll_sync.frame_efd, POLLIN, 0 };
            pfds[1] = { poll_sync.stop_efd,  POLLIN, 0 };
            int pr = ::poll(pfds, 2, -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if ((pfds[1].revents & POLLIN) != 0) {
                uint64_t v;
                (void)::eventfd_read(poll_sync.stop_efd, &v);
                break;
            }
            if ((pfds[0].revents & POLLIN) != 0) {
                uint64_t v;
                (void)::eventfd_read(poll_sync.frame_efd, &v);
            }
        }
    });

    proc_thread.join();
    rx_thread.join();
}

// Watchdog service threads (control cores 0-1). Each task owns its own
// thread and blocks on the shared stop eventfd with a period timeout.

void App::watchdog_telemetry_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    auto& tel = Telemetry::instance();
    Core::TelemetryCollector telemetry(tel);
    uint64_t tick5 = 0;
    uint8_t prev_pe = 0;
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        telemetry.tick1Hz();
        if (++tick5 % 5 == 0) telemetry.tick5s();
        const uint8_t pe = tel.raw_socket_poll_errors.load(std::memory_order_relaxed);
        if (pe != prev_pe && pe != 0) {
            std::println(stderr, "[App] Packet RX poll failure (telemetry mask {})", pe);
            prev_pe = pe;
        }
    }
}

void App::watchdog_l2_refresh_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    Core::L2ForwardRefresher l2_refresher([this]() { forwarding_plane_.refresh_l2(); });
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 5000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        l2_refresher.tick5s();
    }
}

void App::watchdog_wan_tracker_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    Core::NatWanTracker wan_tracker(*nat_engine_,
        [this]() { return Utils::Network::get_local_ip(Config::iface_wan()); });
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 5000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        wan_tracker.tick5s();
    }
}

void App::watchdog_dhcp_worker_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    Core::DhcpWorker dhcp_worker(*dhcp_engine_, lan_fd_,
        [this]() { refresh_dhcp_router_from_kernel(); });
    uint64_t tick5 = 0;
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        dhcp_worker.tick1Hz();
        if (++tick5 % 5 == 0) dhcp_worker.tick5s();
    }
}

void App::watchdog_qos_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    auto& tel = Telemetry::instance();
    Core::QosController qos(tel, shaper_dl_.get(), shaper_ul_.get(),
                            base_dl_mbps, base_ul_mbps);
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        qos.tick1Hz();
    }
}

void App::watchdog_nat_ticker_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    Core::EngineTicker ticker(*nat_engine_);
    while (running_watchdog.load(std::memory_order_acquire)) {
        const int r = wait_watchdog_period(watchdog_stop_efd_, 1000);
        if (r != 0) break;
        if (!running_watchdog.load(std::memory_order_acquire)) break;
        ticker.tick1Hz();
    }
}

// Event-driven control thread: GUI writes dhcp_cfg_efd_; this thread applies
// the DHCP config. Also handles the shared shutdown token.
void App::control_event_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity_control();
    while (running_watchdog.load(std::memory_order_acquire)) {
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
            if (running_watchdog.load(std::memory_order_acquire))
                applyDhcpConfig();
        }
    }
}

void App::request_dhcp_config_apply() {
    if (dhcp_cfg_efd_ >= 0)
        (void)::eventfd_write(dhcp_cfg_efd_, 1);
}

} // namespace HPGTP
