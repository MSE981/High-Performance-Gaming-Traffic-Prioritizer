#include "App.hpp"
#include "EventCallbacks_util.hpp"
#include "ParameterServices.hpp"
#include "TxFrameOutput_util.hpp"
#include "Config.hpp"
#include "Headers_util.hpp"
#include "Processor.hpp"
#include "SystemOptimizer_util.hpp"
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
#include <memory>
#include <span>
#include <thread>
#include <netinet/in.h>
#include <optional>

namespace HPGTP {

namespace {

// Registered data-plane event subscriber
class TelemetryObserver final : public Utils::Events::PacketObserver {
public:
    void on_frame(std::span<const uint8_t>) override {}
    void on_packet(const Utils::Net::ParsedPacket&, Utils::Net::Priority) override {}
    void on_batch(const Telemetry::BatchStats& stats, int core_id) override {
        Telemetry::instance().commit_batch(stats, core_id);
    }
    void on_tx_result(Engine::Scheduler::TxResult, size_t) override {}
};


} // namespace

// App method definitions


App::App() {
    telemetry_observer_ = std::make_unique<TelemetryObserver>();
    netcfg_ = std::make_unique<NetConfig::NetworkConfig>();

    // Service flags are loaded from config/config.txt before App is constructed;
    nat_engine_     = std::make_unique<Engine::Nat::NatEngine>();
    dhcp_engine_    = std::make_unique<Engine::Dhcp::DhcpEngine>(
        Config::ROUTER_IP,
        Engine::Dhcp::DhcpPoolConfig{
            Utils::Net::parse_ipv4(Config::DHCP_POOL_START.c_str()),
            Utils::Net::parse_ipv4(Config::DHCP_POOL_END.c_str()),
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
    Utils::Network::Network_util::disable_hardware_offloads(Config::iface_wan());
    Utils::Network::Network_util::disable_hardware_offloads(Config::iface_lan());
    iface_wan = std::make_unique<Utils::RawSocket::RawSocketManager>(Config::iface_wan());
    iface_lan = std::make_unique<Utils::RawSocket::RawSocketManager>(Config::iface_lan());
    if (auto r = iface_wan->init(); !r) return r;
    if (auto r = iface_lan->init(); !r) return r;

    if (auto s = netcfg_->sync(*dhcp_engine_); !s) return s;

    if (Config::global_state.enable_nat.load(std::memory_order_relaxed)) {
        auto w = forwarding_plane_.resolve_nat_wan_ip();
        if (!w) return std::unexpected(w.error());
        nat_engine_->set_wan_ip(*w);
        std::println("[App] NAT WAN address: {}", Config::ip_to_str(*w));
    } else {
        nat_engine_->set_wan_ip(Utils::Net::IPv4Net{});
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

    HPGTP::Utils::System::lock_cpu_frequency();

    int fd_wan = iface_wan->get_fd();
    int fd_lan = iface_lan->get_fd();
    lan_fd_ = fd_lan;

    base_dl_mbps = tel.qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
    base_ul_mbps = tel.qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
    tel.effective_qos_global_dl_mbps.store(base_dl_mbps, std::memory_order_release);
    tel.effective_qos_global_ul_mbps.store(base_ul_mbps, std::memory_order_release);
    shaper_dl_ = std::make_unique<Engine::Scheduler::Shaper>(Utils::Units::Mbps{base_dl_mbps});
    shaper_ul_ = std::make_unique<Engine::Scheduler::Shaper>(Utils::Units::Mbps{base_ul_mbps});

    dl_events_.register_observer(telemetry_observer_.get());
    ul_events_.register_observer(telemetry_observer_.get());

    shaper_dl_->set_tx_result_callback(
        Engine::Scheduler::TxResultCallback([this](Engine::Scheduler::TxResult result, size_t bytes) {
            dl_events_.dispatch_tx_result(result, bytes);
            if (result == Engine::Scheduler::TxResult::Fatal)
                std::println(stderr, "[App] downstream shaper TX callback: fatal after {} bytes", bytes);
        }));
    shaper_ul_->set_tx_result_callback(
        Engine::Scheduler::TxResultCallback([this](Engine::Scheduler::TxResult result, size_t bytes) {
            ul_events_.dispatch_tx_result(result, bytes);
            if (result == Engine::Scheduler::TxResult::Fatal)
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

    forward_engine_ = std::make_unique<Engine::Forward::Forward_Engine>();
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

namespace HPGTP::Engine::Forward {

namespace {
struct RouteContext {
    int tx_fd;
    Engine::Scheduler::Shaper* shaper;
};
using RouteFunc = void (*)(const RouteContext&, std::span<uint8_t>, size_t, int);

// Data-plane route handlers

void fast_path_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                        size_t prio_idx, int core_id) {
    Utils::TxFrame::TxFrameOutput_util::send_best_effort(ctx.tx_fd, pkt, core_id, prio_idx);
}

void shaper_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                     size_t /*prio_idx*/, int /*core_id*/) {
    if (ctx.shaper) ctx.shaper->enqueue_normal(pkt);
}

// PacketConsumer=Internal data-plane class: assembled callback pipeline, zero runtime

class PacketConsumer {
public:
    int rx_fd;
    int tx_fd;
    int core_id;
    Telemetry::BatchStats          stats;
    Logic::HeuristicProcessor      processor;
    RouteContext                   ctx;
    Engine::Nat::NatEngine*              nat_engine;
    Engine::Dhcp::DhcpEngine*             dhcp_engine;
    Utils::Events::CallbackRegistry&      callbacks;
    Utils::ForwardState::ForwardingState_util&       plane;

    std::array<std::array<RouteFunc, 2>, 2> routes;

    using PipelineStep = bool (*)(PacketConsumer&, Utils::Net::ParsedPacket&);

    // Ordered pipeline stages; each step returns true if it handled the packet.
    struct PacketPipeline {
        std::array<PipelineStep, 12> steps{};
    };
    PacketPipeline pipeline;

    PacketConsumer(int rx_fd_, const PacketWorkerConfig& cfg,
                   Utils::ForwardState::ForwardingState_util& plane_)
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

        // Pipeline steps are fixed at construction.
        if (core_id == 2) {
            // Core 2 WAN to LAN: DNAT, then L2 rewrite and QoS routing.
            pipeline.steps = {{
                step_dhcp_interceptor,
                step_nat_downstream,
                step_eth_rewrite_wan_to_lan,
                step_qos_routing,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
            }};
        } else {
            // Core 3 LAN to WAN: SNAT before sending upstream.
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

    static bool step_dhcp_interceptor(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
        if (self.core_id == 3 && pkt.is_valid_ipv4() && pkt.l4_protocol == 17) {
            auto udp = pkt.udp();
            if (udp && (ntohs(udp->dest) == 67 || ntohs(udp->dest) == 68)) {
                if (self.dhcp_engine) self.dhcp_engine->intercept_request(pkt);
                return true;
            }
        }
        return false;
    }

    static bool step_nat_downstream(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        if (self.nat_engine) self.nat_engine->process_inbound(pkt);
        return false;
    }

    static bool step_nat_upstream(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        if (self.nat_engine) self.nat_engine->process_outbound(pkt);
        return false;
    }

    static bool ipv4_needs_eth_rewrite_for_forward(Utils::Net::IPv4Header* ip) noexcept {
        const uint32_t d = ntohl(ip->daddr.raw());
        if (d == 0xFFFFFFFFu) return false;
        if ((d >> 24) >= 224u) return false;
        return true;
    }

    static bool step_eth_rewrite_lan_to_wan(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_nat.load(std::memory_order_relaxed)) return false;
        const auto& s = self.plane.snapshot();
        if (!s.ready || !pkt.eth || !pkt.ipv4) return false;
        if (!ipv4_needs_eth_rewrite_for_forward(pkt.ipv4)) return false;

        const Utils::Net::IPv4Net dst = pkt.ipv4->daddr;
        bool on_link = false;
        if (s.wan_cfg_valid)
            on_link = Utils::Network::Network_util::ipv4_in_subnet(
                dst, static_cast<int>(s.wan_prefix_len), Utils::Net::IPv4Net{s.wan_ip_nbo});

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

    static bool step_eth_rewrite_wan_to_lan(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
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

    static bool step_qos_routing(PacketConsumer& self, Utils::Net::ParsedPacket& pkt) {
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
    void on_packet_event(Utils::Net::ParsedPacket& pkt) {
        for (auto* step : pipeline.steps)
            if (step && step(*this, pkt)) break;
        // Batch-commit telemetry every 32 packets
        if ((stats.pkts & 31) == 0) {
            callbacks.dispatch_batch(stats, core_id);
            stats.reset();
        }
    }
};

// Fixed-size copy of one Ethernet frame
struct RxFrameCopy {
    std::array<uint8_t, 2048> data{};
    uint16_t                len = 0;
};

} // namespace

std::expected<void, std::string> Forward_Engine::open_poll_fds() {
    close_poll_fds();
    for (auto& w : poll_sync_) {
        w.frame_efd = ::eventfd(0, EFD_CLOEXEC);
        // Semaphore mode: RX and proc threads both poll+read the same stop_efd; a single non-semaphore write(1) wakes both but only one read drains - the other blocks
        w.stop_efd  = ::eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        if (w.frame_efd < 0 || w.stop_efd < 0) {
            int e = errno;
            close_poll_fds();
            return std::unexpected(
                std::string("eventfd for worker poll sync failed: ") + std::strerror(e));
        }
    }
    return {};
}

void Forward_Engine::close_poll_fds() {
    for (auto& w : poll_sync_) {
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

void Forward_Engine::wake_workers() {
    for (auto& w : poll_sync_) {
        if (w.stop_efd >= 0)
            // Two readers per worker (RX  + proc ); EFD_SEMAPHORE needs one increment per successful read.
            (void)::eventfd_write(w.stop_efd, 2);
    }
}

std::expected<void, std::string> Forward_Engine::start(
    std::unique_ptr<Utils::RawSocket::RawSocketManager> wan,
    std::unique_ptr<Utils::RawSocket::RawSocketManager> lan,
    int fd_wan, int fd_lan,
    Engine::Scheduler::Shaper* shaper_dl, Engine::Scheduler::Shaper* shaper_ul,
    Engine::Nat::NatEngine* nat_engine, Engine::Dhcp::DhcpEngine* dhcp_engine,
    Utils::Events::CallbackRegistry* dl_events,
    Utils::Events::CallbackRegistry* ul_events,
    Utils::ForwardState::ForwardingState_util& plane) {
    wan_ = std::move(wan);
    lan_ = std::move(lan);
    fd_wan_ = fd_wan;
    fd_lan_ = fd_lan;
    shaper_dl_ = shaper_dl;
    shaper_ul_ = shaper_ul;
    nat_engine_ = nat_engine;
    dhcp_engine_ = dhcp_engine;
    dl_events_ = dl_events;
    ul_events_ = ul_events;
    plane_ = &plane;

    if (auto pr = open_poll_fds(); !pr) return pr;

    running_.store(true, std::memory_order_relaxed);
    worker_downstream_ = std::thread(
        [this, ps = &poll_sync_[0]](std::unique_ptr<Utils::RawSocket::RawSocketManager> iface,
                                    PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(wan_),
        PacketWorkerConfig{ fd_lan_, 2, shaper_dl_, nat_engine_, dhcp_engine_, dl_events_ });
    worker_upstream_ = std::thread(
        [this, ps = &poll_sync_[1]](std::unique_ptr<Utils::RawSocket::RawSocketManager> iface,
                                    PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(lan_),
        PacketWorkerConfig{ fd_wan_, 3, shaper_ul_, nat_engine_, dhcp_engine_, ul_events_ });
    return {};
}

void Forward_Engine::stop() {
    running_.store(false, std::memory_order_relaxed);
    wake_workers();
    if (worker_downstream_.joinable()) worker_downstream_.join();
    if (worker_upstream_.joinable())   worker_upstream_.join();
    close_poll_fds();
}

Forward_Engine::~Forward_Engine() {
    stop();
}

void Forward_Engine::worker_event_loop(std::unique_ptr<Utils::RawSocket::RawSocketManager> rx_mgr,
                             PacketWorkerConfig cfg,
                             WorkerPollSync& poll_sync) {
    HPGTP::Utils::System::set_current_thread_affinity_control(); // container: control cores 0-1
    std::println("[App] Core {} pipeline mounted and ready.", cfg.core_id);

    const int rx_fd_saved = rx_mgr->get_fd();
    PacketConsumer consumer(rx_fd_saved, cfg, *plane_);
    Utils::Net::SpscRingBuffer<RxFrameCopy, 1024> frame_q{};

    std::thread rx_thread(
        [this, mgr = std::move(rx_mgr), &frame_q, &poll_sync, core = cfg.core_id]() mutable {
            HPGTP::Utils::System::set_current_thread_affinity(core);
            HPGTP::Utils::System::set_realtime_priority();
            const int sock_fd = mgr->get_fd();
            while (this->running_.load(std::memory_order_relaxed)) {
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
                while (this->running_.load(std::memory_order_relaxed)
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
        HPGTP::Utils::System::set_current_thread_affinity(cfg.core_id);
        HPGTP::Utils::System::set_realtime_priority();
        while (this->running_.load(std::memory_order_relaxed)) {
            RxFrameCopy copy{};
            while (frame_q.pop(copy)) {
                consumer.callbacks.dispatch_frame(
                    std::span<const uint8_t>(copy.data.data(), copy.len));
                auto pkt = Utils::Net::ParsedPacket::parse(
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

} // namespace HPGTP::Engine::Forward
