#include "App.hpp"
#include "DataPlane.hpp"
// POSIX C headers — visible only in this translation unit, hidden from all
// clients that include App.hpp.
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/timerfd.h>
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
#include <iostream>
#include <cerrno>
#include <string_view>
#include <mutex>
#include <string>

namespace HPGTP {

namespace {

// RX thread uses poll(2) on the raw socket plus stop_efd (-1 timeout). Egress uses DataPlane::TxFrameOutput.

// ─── Packet routing context (internal to data plane) ─────────────────────────
struct RouteContext {
    int tx_fd;
    std::shared_ptr<Traffic::Shaper> shaper;
};
using RouteFunc = void (*)(const RouteContext&, std::span<uint8_t>, size_t, int);

// ─── Data-plane route handlers ───────────────────────────────────────────────

void fast_path_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                        size_t prio_idx, int core_id) {
    DataPlane::TxFrameOutput::send_best_effort(ctx.tx_fd, pkt, core_id, prio_idx);
}

void shaper_handler(const RouteContext& ctx, std::span<uint8_t> pkt,
                     size_t /*prio_idx*/, int /*core_id*/) {
    if (ctx.shaper) ctx.shaper->enqueue_normal(pkt);
}

// ─── PacketConsumer ──────────────────────────────────────────────────────────
// Internal data-plane class: assembled callback pipeline, zero runtime
// branching.  Hidden from all App clients.

class PacketConsumer {
public:
    int rx_fd;
    int tx_fd;
    int core_id;
    Telemetry::BatchStats          stats;
    Logic::HeuristicProcessor      processor;
    RouteContext                   ctx;
    std::shared_ptr<Logic::NatEngine>      nat_engine;
    std::shared_ptr<Logic::DnsEngine>      dns_engine;
    std::shared_ptr<QoSConfig>             qos_config;
    std::shared_ptr<QoSConfig>             device_shaper;
    std::shared_ptr<Logic::DhcpEngine>     dhcp_engine;
    std::shared_ptr<Logic::FirewallEngine> firewall_engine;
    Net::IPv4Net gateway_ip{};

    std::array<std::array<RouteFunc, 3>, 2> routes;

    using PipelineStep = bool (*)(PacketConsumer&, Net::ParsedPacket&);

    // Ordered pipeline stages; each step returns true if it handled the packet.
    struct PacketPipeline {
        std::array<PipelineStep, 10> steps{};
    };
    PacketPipeline pipeline;

    PacketConsumer(int rx_fd_, const PacketWorkerConfig& cfg)
        : rx_fd(rx_fd_), tx_fd(cfg.tx_fd), core_id(cfg.core_id),
          ctx{cfg.tx_fd, cfg.route_shaper},
          nat_engine(cfg.nat_engine), dns_engine(cfg.dns_engine),
          qos_config(cfg.qos_config), device_shaper(cfg.device_shaper),
          dhcp_engine(cfg.dhcp_engine),
          firewall_engine(cfg.firewall_engine), gateway_ip(cfg.gateway_ip) {

        routes = {{
            { fast_path_handler, fast_path_handler, shaper_handler }, // acceleration
            { fast_path_handler, fast_path_handler, fast_path_handler } // bridge
        }};

        // Pipeline steps are fixed at construction (no per-packet branch to select a path).
        if (core_id == 2) {
            // Core 2 WAN→LAN: DNAT first, then device block on real LAN IP
            pipeline.steps = {{
                step_dhcp_interceptor, step_dns_interceptor,
                step_firewall_inbound, step_nat_downstream,
                step_block_device_downstream, step_device_shaper_downstream,
                step_ip_shaper_downstream, step_qos_routing,
                nullptr, nullptr
            }};
        } else {
            // Core 3 LAN→WAN: block and SNAT before sending upstream
            pipeline.steps = {{
                step_dhcp_interceptor, step_dns_interceptor,
                step_local_delivery_blocker, step_block_device_upstream,
                step_firewall_track_outbound, step_nat_downstream,
                step_nat_upstream, step_device_shaper_upstream,
                step_ip_shaper_upstream, step_qos_routing
            }};
        }
    }

    // ── Pipeline steps ────────────────────────────────────────────────────────

    static bool step_local_delivery_blocker(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4()) return false;
        if (self.core_id == 3) {
            Net::IPv4Net d = pkt.ipv4->daddr;
            if (d == self.gateway_ip              ||
                d == Net::IPv4Net{0xFAFFFFEF}     ||  // 239.255.255.250 SSDP multicast (NBO on LE)
                d == Net::IPv4Net{0xFFFFFFFF})        // broadcast
                return true;
        }
        return false;
    }

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

    static bool step_dns_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_dns_cache.load(std::memory_order_relaxed)) return false;
        if (self.core_id == 3) {
            if (self.dns_engine && self.dns_engine->process_query(pkt, self.rx_fd))
                return true;
        } else if (self.core_id == 2) {
            if (self.dns_engine) self.dns_engine->intercept_response(pkt);
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

    static bool step_ip_shaper_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.qos_config) return false;
        size_t ai     = self.qos_config->active_idx.load(std::memory_order_acquire);
        auto shaper   = self.qos_config->buffers[ai].find(pkt.ipv4->daddr);
        if (shaper) {
            RouteContext ip_ctx{self.tx_fd, shaper};
            shaper_handler(ip_ctx, pkt.raw_span, 2, self.core_id);
            return true;
        }
        return false;
    }

    static bool step_ip_shaper_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.qos_config) return false;
        size_t ai   = self.qos_config->active_idx.load(std::memory_order_acquire);
        auto shaper = self.qos_config->buffers[ai].find(pkt.ipv4->saddr);
        if (shaper) {
            RouteContext ip_ctx{self.tx_fd, shaper};
            shaper_handler(ip_ctx, pkt.raw_span, 2, self.core_id);
            return true;
        }
        return false;
    }

    static bool step_firewall_inbound(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_firewall.load(std::memory_order_relaxed)) return false;
        if (!self.firewall_engine) return false;
        return !self.firewall_engine->check_inbound(pkt); // true = drop
    }

    static bool step_firewall_track_outbound(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!Config::global_state.enable_firewall.load(std::memory_order_relaxed)) return false;
        if (self.firewall_engine) self.firewall_engine->track_outbound(pkt);
        return false;
    }

    static bool step_block_device_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.firewall_engine) return false;
        return self.firewall_engine->is_blocked_ip(pkt.ipv4->daddr);
    }

    static bool step_block_device_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.firewall_engine) return false;
        return self.firewall_engine->is_blocked_ip(pkt.ipv4->saddr);
    }

    static bool step_device_shaper_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.device_shaper) return false;
        size_t ai   = self.device_shaper->active_idx.load(std::memory_order_acquire);
        auto shaper = self.device_shaper->buffers[ai].find(pkt.ipv4->daddr);
        if (shaper) {
            RouteContext c{self.tx_fd, shaper};
            shaper_handler(c, pkt.raw_span, 2, self.core_id);
            return true;
        }
        return false;
    }

    static bool step_device_shaper_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
        if (!pkt.is_valid_ipv4() || !self.device_shaper) return false;
        size_t ai   = self.device_shaper->active_idx.load(std::memory_order_acquire);
        auto shaper = self.device_shaper->buffers[ai].find(pkt.ipv4->saddr);
        if (shaper) {
            RouteContext c{self.tx_fd, shaper};
            shaper_handler(c, pkt.raw_span, 2, self.core_id);
            return true;
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
        size_t mode = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
        self.routes[mode][pi](self.ctx, pkt.raw_span, pi, self.core_id);
        return true;
    }

    // ── Packet entry point ────────────────────────────────────────────────────
    void on_packet_event(Net::ParsedPacket& pkt) {
        for (auto* step : pipeline.steps)
            if (step && step(*this, pkt)) break;
        // Batch-commit telemetry every 32 packets (& 31 avoids division)
        if ((stats.pkts & 31) == 0) {
            Telemetry::instance().commit_batch(stats, core_id);
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

} // anonymous namespace

// ─── App method definitions ───────────────────────────────────────────────────

App::App() {
    shutdown_future = shutdown_promise.get_future();

    // Service flags are loaded from config/config.txt before App is constructed;
    // do not override them here.
    nat_engine      = std::make_shared<Logic::NatEngine>();
    dns_engine      = std::make_shared<Logic::DnsEngine>();
    dhcp_engine     = std::make_shared<Logic::DhcpEngine>(
        Config::ROUTER_IP,
        Logic::DhcpPoolConfig{
            Net::parse_ipv4(Config::DHCP_POOL_START.c_str()),
            Net::parse_ipv4(Config::DHCP_POOL_END.c_str()),
            Config::DHCP_LEASE_DURATION});
    firewall_engine = std::make_shared<Logic::FirewallEngine>();
    if (Config::global_state.enable_upnp.load(std::memory_order_relaxed))
        upnp_engine = std::make_shared<Logic::UpnpEngine>(nat_engine, Config::ROUTER_IP);
    qos_config       = std::make_shared<QoSConfig>();
    device_shaper_dl = std::make_shared<QoSConfig>();
    device_shaper_ul = std::make_shared<QoSConfig>();
    qos_config->update(Config::IP_LIMIT_TABLE, Config::IP_LIMIT_COUNT);
    nat_engine->set_wan_ip(Config::parse_ip_str(Config::ROUTER_IP));  // IPv4Net ✓
}

App::~App() {
    // Stop data-plane workers (Cores 2/3) before watchdog (Core 1) so
    // the watchdog does not read stale shaper state after workers exit.
    running_workers.store(false, std::memory_order_relaxed);
    wake_proc_threads_for_shutdown();
    if (worker_downstream.joinable()) worker_downstream.join();
    if (worker_upstream.joinable())   worker_upstream.join();
    close_worker_poll_fds();
    running_watchdog.store(false, std::memory_order_relaxed);
    wake_watchdog_for_shutdown();
    if (watchdog.joinable()) watchdog.join();
    close_watchdog_stop_efd();
}

std::expected<void, std::string> App::open_worker_poll_fds_for_start() {
    close_worker_poll_fds();
    for (auto& w : worker_poll_) {
        w.frame_efd = ::eventfd(0, EFD_CLOEXEC);
        w.stop_efd  = ::eventfd(0, EFD_CLOEXEC);
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
            (void)::eventfd_write(w.stop_efd, 1);
    }
}

void App::wake_watchdog_for_shutdown() {
    if (watchdog_stop_efd_ >= 0)
        (void)::eventfd_write(watchdog_stop_efd_, 1);
}

void App::close_watchdog_stop_efd() {
    if (watchdog_stop_efd_ >= 0) {
        ::close(watchdog_stop_efd_);
        watchdog_stop_efd_ = -1;
    }
}

void App::stop() {
    if (shutdown_sequence_started_.exchange(true, std::memory_order_acq_rel)) return;
    // Stop data-plane workers (Cores 2/3) before watchdog (Core 1) so
    // the watchdog does not read stale shaper state after workers exit.
    running_workers.store(false, std::memory_order_relaxed);
    wake_proc_threads_for_shutdown();
    if (worker_downstream.joinable()) worker_downstream.join();
    if (worker_upstream.joinable()) worker_upstream.join();
    close_worker_poll_fds();
    running_watchdog.store(false, std::memory_order_release);
    wake_watchdog_for_shutdown();
    if (watchdog.joinable()) watchdog.join();
    close_watchdog_stop_efd();
    std::call_once(shutdown_notify_once_, [this]() { shutdown_promise.set_value(); });
}

std::expected<void, std::string> App::init() {
    Utils::Network::disable_hardware_offloads(Config::IFACE_WAN);
    Utils::Network::disable_hardware_offloads(Config::IFACE_LAN);
    iface_wan = std::make_unique<Engine::RawSocketManager>(Config::IFACE_WAN);
    iface_lan = std::make_unique<Engine::RawSocketManager>(Config::IFACE_LAN);
    if (auto r = iface_wan->init(); !r) return r;
    if (auto r = iface_lan->init(); !r) return r;
    return {};
}

void App::start() {
    std::println("=== High-performance gaming traffic prioritizer (software router) ===");
    Telemetry::instance().bridge_mode.store(
        !Config::ENABLE_ACCELERATION.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    HPGTP::System::Optimizer::lock_cpu_frequency();

    int fd_wan = iface_wan->get_fd();
    int fd_lan = iface_lan->get_fd();
    lan_fd_ = fd_lan;

    constexpr double dl = 500.0;
    constexpr double ul = 50.0;
    base_dl_mbps = dl;
    base_ul_mbps = ul;
    global_shaper_dl = std::make_shared<Traffic::Shaper>(Traffic::Mbps{base_dl_mbps});
    global_shaper_ul = std::make_shared<Traffic::Shaper>(Traffic::Mbps{base_ul_mbps});

    Net::IPv4Net gw_ip = Config::parse_ip_str(Config::ROUTER_IP);
    if (auto pr = open_worker_poll_fds_for_start(); !pr) {
        std::println(stderr, "[App] {}", pr.error());
        return;
    }

    if (watchdog_stop_efd_ < 0) {
        watchdog_stop_efd_ = ::eventfd(0, EFD_CLOEXEC);
        if (watchdog_stop_efd_ < 0) {
            std::println(stderr, "[Fatal] watchdog stop eventfd: {}",
                std::strerror(errno));
            std::exit(1);
        }
    }
    running_watchdog.store(true, std::memory_order_relaxed);
    watchdog = std::thread([this]() { watchdog_loop(); });

    Utils::Network::force_arp_resolution(Utils::Network::get_gateway_ip());

    running_workers.store(true, std::memory_order_relaxed);

    worker_downstream = std::thread(
        [this, ps = &worker_poll_[0]](std::unique_ptr<Engine::RawSocketManager> iface,
                                       PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(iface_wan),
        PacketWorkerConfig{ fd_lan, 2, global_shaper_dl, nat_engine, dns_engine,
                            qos_config, device_shaper_dl, dhcp_engine,
                            firewall_engine, gw_ip });

    worker_upstream = std::thread(
        [this, ps = &worker_poll_[1]](std::unique_ptr<Engine::RawSocketManager> iface,
                                       PacketWorkerConfig cfg) {
            worker_event_loop(std::move(iface), std::move(cfg), *ps);
        },
        std::move(iface_lan),
        PacketWorkerConfig{ fd_wan, 3, global_shaper_ul, nat_engine, dns_engine,
                            qos_config, device_shaper_ul, dhcp_engine,
                            firewall_engine, gw_ip });

    std::println("[App] Data plane and control plane started.");
}

void App::wait_for_shutdown() {
    shutdown_future.wait();
    std::println("\n[System] Shutdown signal received, core services terminated gracefully.");
}

// ─── Worker event loop (Core 2 / Core 3) ─────────────────────────────────────
// RX thread: blocking poll(2) on AF_PACKET and stop_efd; copy each frame into an
// SPSC ring. Processing thread: parse and run PacketConsumer::on_packet_event;
// waits on frame_efd/stop_efd with no periodic timeout.

void App::worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr,
                             PacketWorkerConfig cfg,
                             WorkerPollSync& poll_sync) {
    std::println("[App] Core {} pipeline mounted and ready.", cfg.core_id);

    const int rx_fd_saved = rx_mgr->get_fd();
    PacketConsumer consumer(rx_fd_saved, cfg);
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
                auto pkt = Net::ParsedPacket::parse(
                    std::span<uint8_t>(copy.data.data(), copy.len));
                consumer.on_packet_event(pkt);
            }

            if (cfg.route_shaper) cfg.route_shaper->process_queue(cfg.tx_fd);

            if (consumer.qos_config
                && Config::IP_LIMIT_ACTIVE.load(std::memory_order_relaxed)) {
                size_t ai =
                    consumer.qos_config->active_idx.load(std::memory_order_acquire);
                consumer.qos_config->buffers[ai].for_each_occupied([&](auto& shaper) {
                    shaper->process_queue(cfg.tx_fd);
                });
            }

            Telemetry::instance().core_metrics[cfg.core_id].last_heartbeat.fetch_add(
                1, std::memory_order_relaxed);

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

// ─── Watchdog loop (Core 1, 1 Hz timerfd) ────────────────────────────────────

void App::watchdog_loop() {
    HPGTP::System::Optimizer::set_current_thread_affinity(1);
    auto& tel = Telemetry::instance();

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd == -1) {
        std::println(stderr, "[App] Fatal: timerfd_create: {}", std::strerror(errno));
        running_watchdog.store(false, std::memory_order_relaxed);
        std::call_once(shutdown_notify_once_, [this]() { shutdown_promise.set_value(); });
        return;
    }

    struct itimerspec its{};
    its.it_value.tv_sec    = 1;
    its.it_interval.tv_sec = 1;
    if (timerfd_settime(tfd, 0, &its, nullptr) == -1) {
        std::println(stderr, "[App] Fatal: timerfd_settime: {}", std::strerror(errno));
        ::close(tfd);
        running_watchdog.store(false, std::memory_order_relaxed);
        std::call_once(shutdown_notify_once_, [this]() { shutdown_promise.set_value(); });
        return;
    }

    uint64_t expirations;
    uint64_t last_bytes[4]  = {};
    uint64_t stat_idle[4]   = {};
    uint64_t stat_total[4]  = {};
    uint64_t watchdog_tick  = 0;
    int      last_throttle_pct = 100;

    auto& si = tel.sys_info;

    // Raw-fd sysfs reader — no heap, no ifstream
    auto read_sysfd = [](const char* path, std::span<char> out) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) { out[0] = '\0'; return; }
        ssize_t n = ::read(fd, out.data(), out.size() - 1);
        ::close(fd);
        if (n > 0) {
            out[n] = '\0';
            if (out[n - 1] == '\n') out[n - 1] = '\0';
        } else {
            out[0] = '\0';
        }
    };

    // Interface scan — shared by 5-tick refresh and on-demand UI rescan paths
    auto scan_ifaces = [&]() {
        uint8_t cnt = 0;
        DIR* d = opendir("/sys/class/net");
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr &&
                   cnt < Telemetry::SystemInfo::MAX_IFACES) {
                if (de->d_name[0] == '.'
                    || std::string_view{de->d_name} == "lo")
                    continue;
                {
                    std::string_view name_sv{de->d_name};
                    auto nn = name_sv.copy(si.ifaces[cnt].name.data(), 15);
                    si.ifaces[cnt].name[nn] = '\0';
                }
                char path[64];
                snprintf(path, sizeof(path),
                    "/sys/class/net/%s/operstate", de->d_name);
                char sbuf[8]{};
                int sfd = ::open(path, O_RDONLY);
                if (sfd >= 0) {
                    ssize_t n = ::read(sfd, sbuf, sizeof(sbuf) - 1);
                    ::close(sfd);
                    if (n > 0 && sbuf[n - 1] == '\n') sbuf[n - 1] = '\0';
                }
                {
                    std::string_view op_sv{sbuf[0] ? sbuf : "unknown"};
                    auto no = op_sv.copy(si.ifaces[cnt].operstate.data(), 7);
                    si.ifaces[cnt].operstate[no] = '\0';
                }
                ++cnt;
            }
            closedir(d);
        }
        si.iface_count.store(cnt, std::memory_order_release);
    };

    while (running_watchdog.load(std::memory_order_acquire)) {
        const int rescan_fd = si.rescan_poll_fd();
        struct pollfd pfds[3]{};
        pfds[0] = { tfd, POLLIN, 0 };
        pfds[1] = { watchdog_stop_efd_, POLLIN, 0 };
        int nfds = 2;
        if (rescan_fd >= 0) {
            pfds[2] = { rescan_fd, POLLIN, 0 };
            nfds    = 3;
        }
        int pr = poll(pfds, nfds, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if ((pfds[1].revents & POLLIN) != 0) {
            uint64_t v;
            (void)::eventfd_read(watchdog_stop_efd_, &v);
            continue;
        }

        bool timer_fired = (pfds[0].revents & POLLIN) != 0;
        bool force_scan  = false;

        if (timer_fired && ::read(tfd, &expirations, sizeof(expirations)) <= 0)
            timer_fired = false;
        if (nfds == 3 && (pfds[2].revents & POLLIN)) {
            si.consume_rescan();
            force_scan = true;
        }

        // On-demand rescan only — skip all 1 Hz work
        if (force_scan && !timer_fired) {
            scan_ifaces();
            si.signal_done();
            continue;
        }
        if (!timer_fired) continue;

        // CPU temperature (1 Hz)
        {
            char tbuf[16]{};
            int fd = ::open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
            if (fd >= 0) {
                ssize_t n = ::read(fd, tbuf, sizeof(tbuf) - 1);
                ::close(fd);
                if (n > 0) tel.cpu_temp_celsius.store(
                    atof(tbuf) / 1000.0, std::memory_order_relaxed);
            }
        }

        // Per-core CPU load from /proc/stat (1 Hz)
        {
            char sbuf[1024]{};
            int sfd = ::open("/proc/stat", O_RDONLY);
            if (sfd >= 0) {
                ssize_t n = ::read(sfd, sbuf, sizeof(sbuf) - 1);
                ::close(sfd);
                if (n > 0) {
                    sbuf[n] = '\0';
                    const char* p = sbuf;
                    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
                    if (nproc < 1) nproc = 1;
                    int max_ci = static_cast<int>(std::min<long>(
                        nproc, static_cast<long>(tel.core_metrics.size())));
                    for (int ci = 0; ci < max_ci; ++ci) {
                        char tag[8];
                        snprintf(tag, sizeof(tag), "cpu%d ", ci);
                        const char* ln = strstr(p, tag);
                        if (!ln) break;
                        ln += strlen(tag);
                        uint64_t user, nice, sys, idle, iowait, irq, softirq;
                        if (sscanf(ln, "%lu %lu %lu %lu %lu %lu %lu",
                                &user, &nice, &sys, &idle,
                                &iowait, &irq, &softirq) == 7) {
                            uint64_t total = user+nice+sys+idle+iowait+irq+softirq;
                            uint64_t dt    = total - stat_total[ci];
                            uint64_t di    = idle  - stat_idle[ci];
                            int pct = (dt > 0) ? static_cast<int>(100*(dt-di)/dt) : 0;
                            tel.core_metrics[ci].cpu_load_pct.store(
                                pct, std::memory_order_relaxed);
                            stat_total[ci] = total;
                            stat_idle[ci]  = idle;
                        }
                    }
                }
            }
        }

        // Shaper stats: hot path only increments atomics; log here at 1 Hz.
        {
            static uint64_t prev_ok = 0, prev_ovf = 0, prev_big = 0;
            uint64_t ok   = tel.shaper_normal_tx_complete.load(std::memory_order_relaxed);
            uint64_t ovf  = tel.shaper_queue_overflow_drops.load(std::memory_order_relaxed);
            uint64_t big  = tel.shaper_oversized_drops.load(std::memory_order_relaxed);
            uint64_t dok  = ok - prev_ok;
            uint64_t dovf = ovf - prev_ovf;
            uint64_t dbig = big - prev_big;
            if (dok != 0 || dovf != 0 || dbig != 0) {
                std::println(
                    "[Shaper] last 1s: normal_tx_ok +{}, queue_overflow_drops +{}, oversized_drops +{}",
                    dok, dovf, dbig);
            }
            prev_ok  = ok;
            prev_ovf = ovf;
            prev_big = big;
        }

        // System info refresh every 5 ticks (5 s)
        bool did_iface_scan = false;
        ++watchdog_tick;
        if (watchdog_tick % 5 == 0) {
            read_sysfd("/etc/hostname",   std::span<char>(si.hostname));
            read_sysfd("/proc/version",   std::span<char>(si.kernel_short));

            char ubuf[32]{};
            read_sysfd("/proc/uptime", std::span<char>(ubuf));
            if (ubuf[0])
                si.uptime_seconds.store(
                    static_cast<uint64_t>(atof(ubuf)), std::memory_order_relaxed);

            char mbuf[512]{};
            int mfd = ::open("/proc/meminfo", O_RDONLY);
            if (mfd >= 0) {
                ssize_t nr = ::read(mfd, mbuf, sizeof(mbuf) - 1);
                ::close(mfd);
                if (nr > 0) {
                    uint64_t total = 0, avail = 0;
                    const char* mt = strstr(mbuf, "MemTotal:");
                    const char* ma = strstr(mbuf, "MemAvailable:");
                    if (mt) sscanf(mt, "MemTotal: %lu",     &total);
                    if (ma) sscanf(ma, "MemAvailable: %lu", &avail);
                    si.mem_total_kb.store(total, std::memory_order_relaxed);
                    si.mem_avail_kb.store(avail, std::memory_order_relaxed);
                }
            }

            scan_ifaces();
            did_iface_scan = true;

            // ARP table → Telemetry::device_table (5 Hz)
            {
                char arp[4096]{};
                int afd = ::open("/proc/net/arp", O_RDONLY);
                if (afd >= 0) {
                    ssize_t nr = ::read(afd, arp, sizeof(arp) - 1);
                    ::close(afd);
                    uint8_t dcnt = 0;
                    if (nr > 0) {
                        char* line = arp;
                        char* end  = arp + nr;
                        while (line < end && *line != '\n') ++line;
                        if (line < end) ++line;
                        while (line < end && dcnt < Telemetry::MAX_TRACKED_DEVICES) {
                            char ip_str[20]{}, hw[8]{}, flags[8]{}, mac[20]{};
                            if (sscanf(line, "%19s %7s %7s %19s",
                                       ip_str, hw, flags, mac) == 4) {
                                Net::IPv4Net ip = Net::parse_ipv4(ip_str);
                                if (ip.raw() != INADDR_NONE && strcmp(flags, "0x0") != 0) {
                                    tel.device_table[dcnt].ip = ip;
                                    std::string_view mac_sv{mac};
                                    auto nm = mac_sv.copy(tel.device_table[dcnt].mac.data(), 17);
                                    tel.device_table[dcnt].mac[nm] = '\0';
                                    ++dcnt;
                                }
                            }
                            while (line < end && *line != '\n') ++line;
                            if (line < end) ++line;
                        }
                    }
                    tel.device_count.store(dcnt, std::memory_order_release);
                }
            }
        }

        if (force_scan && !did_iface_scan) scan_ifaces();
        if (force_scan) si.signal_done();

        // Bandwidth (1 Hz)
        uint64_t bd = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
        uint64_t bu = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);
        std::println("[Monitor] DL: {:.2f} Mbps | UL: {:.2f} Mbps | Mode: {}",
            (bd - last_bytes[2]) * 8.0 / 1e6,
            (bu - last_bytes[3]) * 8.0 / 1e6,
            tel.bridge_mode.load(std::memory_order_relaxed) ? "Bridge" : "Accel");
        last_bytes[2] = bd;
        last_bytes[3] = bu;

        // Game port whitelist (GUI staging → double-buffer swap)
        if (Config::GAME_PORTS_DIRTY.exchange(false, std::memory_order_acq_rel))
            Config::apply_pended_game_ports();

        // Device policy sync
        if (Config::DEVICE_POLICY_DIRTY.exchange(false, std::memory_order_acq_rel)) {
            const std::lock_guard<std::mutex> plk(Config::device_policy_mutex);
            if (firewall_engine) firewall_engine->sync_blocked_ips_locked();

            auto rebuild_device_shaper = [&](std::shared_ptr<QoSConfig>& cfg_ptr, bool use_dl) {
                if (!cfg_ptr) return;
                size_t active   = cfg_ptr->active_idx.load(std::memory_order_acquire);
                size_t inactive = 1 - active;
                cfg_ptr->buffers[inactive] = {};
                for (size_t i = 0; i < Config::DEVICE_POLICY_COUNT; ++i) {
                    const auto& p = Config::DEVICE_POLICY_TABLE[i];
                    if (p.rate_limited && p.ip.raw() != 0)
                        cfg_ptr->buffers[inactive].insert(
                            p.ip, std::make_shared<Traffic::Shaper>(
                                use_dl ? p.dl : p.ul));
                }
                cfg_ptr->active_idx.store(inactive, std::memory_order_release);
            };
            rebuild_device_shaper(device_shaper_dl, true);
            rebuild_device_shaper(device_shaper_ul, false);
            std::println("[Device] Policy synced: {} entries", Config::DEVICE_POLICY_COUNT);
        }

        // DNS config sync
        if (dns_engine && tel.dns_config_dirty.exchange(false, std::memory_order_acq_rel)) {
            dns_engine->set_upstream({
                Config::parse_ip_str(Config::DNS_UPSTREAM_PRIMARY),
                Config::parse_ip_str(Config::DNS_UPSTREAM_SECONDARY)});
            dns_engine->set_redirect(
                Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed));
            dns_engine->reload_static_records();
            std::println("[DNS] Config applied: upstream {} / {}, redirect={}, static={}",
                Config::DNS_UPSTREAM_PRIMARY, Config::DNS_UPSTREAM_SECONDARY,
                Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed) ? "on":"off",
                Config::STATIC_DNS_COUNT);
        }

        // DHCP config sync
        if (dhcp_engine && tel.dhcp_config_dirty.exchange(false, std::memory_order_acq_rel)) {
            if (auto dr = dhcp_engine->reconfigure({
                    Config::parse_ip_str(Config::DHCP_POOL_START),
                    Config::parse_ip_str(Config::DHCP_POOL_END),
                    Config::DHCP_LEASE_DURATION});
                !dr) {
                std::println(stderr, "[DHCP] reconfigure failed: {}", dr.error());
            } else {
                std::println("[DHCP] Pool reconfigured: {} – {}, lease {}s",
                    Config::DHCP_POOL_START, Config::DHCP_POOL_END,
                    Config::DHCP_LEASE_DURATION.count());
            }
        }

        // 1 Hz engine ticks
        if (nat_engine)      nat_engine->tick();
        if (dns_engine)    { dns_engine->tick(); dns_engine->process_background_tasks(); }
        if (dhcp_engine)     dhcp_engine->process_background_tasks(lan_fd_);
        if (firewall_engine) { firewall_engine->tick(); firewall_engine->cleanup(); }

        // Global bandwidth caps from QoS page (Apply button)
        if (tel.qos_global_bw_dirty.exchange(false, std::memory_order_acq_rel)) {
            base_dl_mbps = tel.qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
            base_ul_mbps = tel.qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
            int pct = tel.qos_throttle_pct.load(std::memory_order_relaxed);
            double factor = pct / 100.0;
            if (global_shaper_dl)
                global_shaper_dl->set_rate_limit(Traffic::Mbps{base_dl_mbps * factor});
            if (global_shaper_ul)
                global_shaper_ul->set_rate_limit(Traffic::Mbps{base_ul_mbps * factor});
            last_throttle_pct = pct;
            std::println("[QoS] Global limits applied — base DL {:.1f} / UL {:.1f} Mbps (throttle {}%)",
                base_dl_mbps, base_ul_mbps, pct);
        }

        // QoS throttle from GUI slider
        {
            int pct = tel.qos_throttle_pct.load(std::memory_order_relaxed);
            if (pct != last_throttle_pct) {
                last_throttle_pct = pct;
                double factor = pct / 100.0;
                if (global_shaper_dl)
                    global_shaper_dl->set_rate_limit(Traffic::Mbps{base_dl_mbps * factor});
                if (global_shaper_ul)
                    global_shaper_ul->set_rate_limit(Traffic::Mbps{base_ul_mbps * factor});
                std::println("[QoS] Throttle {}% — DL {:.1f} Mbps / UL {:.1f} Mbps",
                    pct, base_dl_mbps * factor, base_ul_mbps * factor);
            }
        }
    }

    close(tfd);
}

} // namespace HPGTP
