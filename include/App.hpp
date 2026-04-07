#pragma once
#include <thread>
#include <atomic>
#include <memory>
#include <expected>
#include <chrono>
#include <ctime>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "NetworkUtils.hpp"
#include "NetworkEngine.hpp"
#include "Processor.hpp"
#include "NatEngine.hpp"
#include "DnsEngine.hpp"
#include "DhcpEngine.hpp"
#include "UpnpEngine.hpp"
#include "SystemOptimizer.hpp"
#include "Telemetry.hpp"
#include "ProbeManager.hpp"
#include "Scheduler.hpp"
#include "FirewallEngine.hpp"
#include <print>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <poll.h> 
#include <future>
#include <functional>
#include <array>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <dirent.h>
#include <sys/eventfd.h>

namespace Scalpel {

    // Data plane: efficient static dispatch based on function pointers (eliminates virtual function overhead)
    struct RouteContext {
        int tx_fd;
        std::shared_ptr<Traffic::Shaper> shaper;
    };

    using RouteFunc = void (*)(const RouteContext& ctx, std::span<uint8_t> pkt, size_t prio_idx, int core_id);

    static void fast_path_handler(const RouteContext& ctx, std::span<uint8_t> pkt, size_t prio_idx, int core_id) {
        if (send(ctx.tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) < 0) {
            // Follow ISR "complete immediately" and zero-blocking principle: if send buffer full, tail drop decisively, never hang or poll-retry
            Telemetry::instance().core_metrics[core_id].dropped[prio_idx].fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void shaper_handler(const RouteContext& ctx, std::span<uint8_t> pkt, size_t /*prio_idx*/, int /*core_id*/) {
        if (ctx.shaper) ctx.shaper->enqueue_normal(pkt);
    }

    // Helper: static hash table based on FNV-1a (optimized for embedded real-time)
    template<typename T, size_t Capacity = 256>
    class StaticIpMap {
    public:
        struct Entry {
            uint32_t key = 0;
            T value = nullptr;
            bool occupied = false;
        };

    private:
        std::array<Entry, Capacity> table{};
        static uint32_t fnv1a_hash(uint32_t val) {
            uint32_t h = 2166136261U;
            h ^= (val & 0xFF); h *= 16777619U;
            h ^= ((val >> 8) & 0xFF); h *= 16777619U;
            h ^= ((val >> 16) & 0xFF); h *= 16777619U;
            h ^= ((val >> 24) & 0xFF); h *= 16777619U;
            return h;
        }

    public:
        // Iterate over all occupied entries; cb receives each non-null value.
        template<typename Callback>
        void for_each_occupied(Callback&& cb) {
            for (auto& e : table)
                if (e.occupied && e.value) cb(e.value);
        }

        void insert(uint32_t ip, T val) {
            uint32_t h = fnv1a_hash(ip) % Capacity;
            for (size_t i = 0; i < Capacity; ++i) {
                size_t idx = (h + i) % Capacity;
                if (!table[idx].occupied || table[idx].key == ip) {
                    table[idx].key = ip;
                    table[idx].value = val;
                    table[idx].occupied = true;
                    return;
                }
            }
        }

        T find(uint32_t ip) const {
            uint32_t h = fnv1a_hash(ip) % Capacity;
            for (size_t i = 0; i < Capacity; ++i) {
                size_t idx = (h + i) % Capacity;
                if (!table[idx].occupied) return nullptr;
                if (table[idx].key == ip) return table[idx].value;
            }
            return nullptr;
        }
    };

    // Phase 2.4: Software router QoS lock-free double-buffer config core (RCU config swap)
    struct QoSConfig {
        std::array<StaticIpMap<std::shared_ptr<Traffic::Shaper>, 256>, 2> buffers;
        alignas(64) std::atomic<size_t> active_idx{0};

        void update(const std::array<Config::IpLimitEntry, Config::MAX_IP_LIMITS>& table, size_t count) {
            size_t active = active_idx.load(std::memory_order_relaxed);
            size_t inactive = 1 - active;
            buffers[inactive] = {}; // Zero Allocation Refresh
            for (size_t i = 0; i < count; ++i) {
                buffers[inactive].insert(table[i].ip, std::make_shared<Traffic::Shaper>(table[i].rate_mbps));
            }
            active_idx.store(inactive, std::memory_order_release);
        }
    };

    // Configuration bundle for a packet worker thread (§2.2.3: use struct not loads of arguments)
    struct PacketWorkerConfig {
        int tx_fd;
        int core_id;
        std::shared_ptr<Traffic::Shaper> global_shaper;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::shared_ptr<Logic::DnsEngine> dns_engine;
        std::shared_ptr<QoSConfig> qos_config;
        std::shared_ptr<QoSConfig> device_shaper;  // per-device hard rate cap (DL for core2, UL for core3) — independent of QoS
        std::shared_ptr<Logic::DhcpEngine> dhcp_engine;
        std::shared_ptr<Logic::FirewallEngine> firewall_engine;
        uint32_t gateway_ip;
    };

    // Data plane: event-consumer model — core packet processing
    class PacketConsumer {
    public:
        int rx_fd; // socket FD for reading (RX ring)
        int tx_fd; // socket FD for writing (TX)
        int core_id; // CPU core this consumer is pinned to
        Telemetry::BatchStats stats;
        Logic::HeuristicProcessor processor;
        
        RouteContext ctx;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::shared_ptr<Logic::DnsEngine> dns_engine;
        std::shared_ptr<QoSConfig> qos_config;
        std::shared_ptr<QoSConfig> device_shaper;
        std::shared_ptr<Logic::DhcpEngine> dhcp_engine;
        std::shared_ptr<Logic::FirewallEngine> firewall_engine;
        uint32_t gateway_ip = 0;

        std::array<std::array<RouteFunc, 3>, 2> routes;

        // Callback-based pipeline — eliminates all runtime if-else branching
        using PipelineStep = std::function<bool(PacketConsumer&, Net::ParsedPacket&)>;
        std::array<PipelineStep, 10> pipeline;

        PacketConsumer(int rx_fd, const PacketWorkerConfig& cfg)
            : rx_fd(rx_fd), tx_fd(cfg.tx_fd), core_id(cfg.core_id),
              ctx{cfg.tx_fd, cfg.global_shaper},
              nat_engine(cfg.nat_engine), dns_engine(cfg.dns_engine),
              qos_config(cfg.qos_config), device_shaper(cfg.device_shaper),
              dhcp_engine(cfg.dhcp_engine),
              firewall_engine(cfg.firewall_engine), gateway_ip(cfg.gateway_ip) {

            routes = {{
                { fast_path_handler, fast_path_handler, shaper_handler }, // acceleration mode
                { fast_path_handler, fast_path_handler, fast_path_handler } // transparent bridge mode
            }};

            // Pipeline assembled at construction time — eliminates all runtime if-else branching.
            // Core 2 (WAN -> LAN, downstream): apply per-IP download shaper before QoS routing.
            // Core 3 (LAN -> WAN, upstream):   apply per-IP upload shaper before QoS routing.
            if (core_id == 2) {
                // step_block_device_downstream runs after DNAT so daddr is the real LAN IP.
                pipeline = { step_dhcp_interceptor, step_dns_interceptor, step_firewall_inbound,
                             step_nat_downstream, step_block_device_downstream,
                             step_device_shaper_downstream, step_ip_shaper_downstream,
                             step_qos_routing };
            } else {
                // step_block_device_upstream checks saddr (LAN IP) before SNAT.
                pipeline = { step_dhcp_interceptor, step_dns_interceptor, step_local_delivery_blocker,
                             step_block_device_upstream, step_firewall_track_outbound,
                             step_nat_downstream, step_nat_upstream,
                             step_device_shaper_upstream, step_ip_shaper_upstream,
                             step_qos_routing };
            }
        } // end PacketConsumer constructor

        // --- Pipeline step handlers ---
        
        static bool step_local_delivery_blocker(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return false;
            if (self.core_id == 3) { // LAN -> WAN upstream only
                uint32_t daddr = pkt.ipv4->daddr;
                if (daddr == self.gateway_ip ||
                    daddr == 0xFAFFFFEF || // 239.255.255.250 SSDP multicast (little-endian)
                    daddr == 0xFFFFFFFF) { // broadcast
                    // Return true to cut the forwarding pipeline.
                    // Effect: packet is not sent via tx_fd (WAN); falls through to the Linux local stack (SSDP, SSH, etc.)
                    return true;
                }
            }
            return false;
        }
        
        static bool step_dhcp_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_dhcp.load(std::memory_order_relaxed)) return false;

            // Core 3 (LAN -> WAN) intercepts DHCP requests before they reach the WAN
            if (self.core_id == 3 && pkt.is_valid_ipv4() && pkt.l4_protocol == 17) {
                auto udp = pkt.udp();
                if (udp && (ntohs(udp->dest) == 67 || ntohs(udp->dest) == 68)) {
                    if (self.dhcp_engine) self.dhcp_engine->intercept_request(pkt);
                    return true; // intercepted — block forwarding to WAN
                }
            }
            return false;
        }

        static bool step_dns_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_dns_cache.load(std::memory_order_relaxed)) return false;

            if (self.core_id == 3) {
                // Core 3 (upstream: LAN -> WAN): intercept outbound DNS query
                if (self.dns_engine && self.dns_engine->process_query(pkt, self.rx_fd)) {
                    return true; // cache hit — reply sent back on rx_fd, stop pipeline
                }
            } else if (self.core_id == 2) {
                // Core 2 (downstream: WAN -> LAN): observe inbound DNS response for caching
                if (self.dns_engine) self.dns_engine->intercept_response(pkt);
                // Never block the response — always forward to the client
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
            if (pkt.is_valid_ipv4() && self.qos_config) {
                uint32_t target_ip = pkt.ipv4->daddr;

                size_t active_idx = self.qos_config->active_idx.load(std::memory_order_acquire);
                auto target_shaper = self.qos_config->buffers[active_idx].find(target_ip);

                if (target_shaper) {
                    RouteContext ip_ctx{self.tx_fd, target_shaper};
                    shaper_handler(ip_ctx, pkt.raw_span, 2, self.core_id);
                    return true; // per-IP shaper claimed the packet — stop pipeline
                }
            }
            return false;
        }

        static bool step_ip_shaper_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (pkt.is_valid_ipv4() && self.qos_config) {
                uint32_t target_ip = pkt.ipv4->saddr;
                
                size_t active_idx = self.qos_config->active_idx.load(std::memory_order_acquire);
                auto target_shaper = self.qos_config->buffers[active_idx].find(target_ip);

                if (target_shaper) {
                    RouteContext ip_ctx{self.tx_fd, target_shaper};
                    shaper_handler(ip_ctx, pkt.raw_span, 2, self.core_id);
                    return true;
                }
            }
            return false;
        }

        // Core 2 (WAN→LAN): drop unsolicited inbound packets when firewall is enabled
        static bool step_firewall_inbound(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_firewall.load(std::memory_order_relaxed)) return false;
            if (!self.firewall_engine) return false;
            // check_inbound returns true=allow, false=block; negate to decide whether to cut pipeline
            if (!self.firewall_engine->check_inbound(pkt)) {
                return true; // drop — cut pipeline, packet is not forwarded
            }
            return false; // established session — continue pipeline
        }

        // Core 3 (LAN→WAN): register outbound flow in conntrack table BEFORE SNAT
        static bool step_firewall_track_outbound(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_firewall.load(std::memory_order_relaxed)) return false;
            if (self.firewall_engine) self.firewall_engine->track_outbound(pkt);
            return false; // never blocks forwarding
        }

        // Core 2 (WAN→LAN): drop if LAN destination device is blocked (runs after DNAT)
        static bool step_block_device_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4() || !self.firewall_engine) return false;
            return self.firewall_engine->is_blocked_ip(pkt.ipv4->daddr);
        }

        // Core 3 (LAN→WAN): drop if LAN source device is blocked (runs before SNAT)
        static bool step_block_device_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4() || !self.firewall_engine) return false;
            return self.firewall_engine->is_blocked_ip(pkt.ipv4->saddr);
        }

        // Core 2 (WAN→LAN): per-device download rate shaping (daddr = LAN device)
        static bool step_device_shaper_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4() || !self.device_shaper) return false;
            size_t active_idx = self.device_shaper->active_idx.load(std::memory_order_acquire);
            auto shaper = self.device_shaper->buffers[active_idx].find(pkt.ipv4->daddr);
            if (shaper) {
                RouteContext ctx{self.tx_fd, shaper};
                shaper_handler(ctx, pkt.raw_span, 2, self.core_id);
                return true;
            }
            return false;
        }

        // Core 3 (LAN→WAN): per-device upload rate shaping (saddr = LAN device)
        static bool step_device_shaper_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4() || !self.device_shaper) return false;
            size_t active_idx = self.device_shaper->active_idx.load(std::memory_order_acquire);
            auto shaper = self.device_shaper->buffers[active_idx].find(pkt.ipv4->saddr);
            if (shaper) {
                RouteContext ctx{self.tx_fd, shaper};
                shaper_handler(ctx, pkt.raw_span, 2, self.core_id);
                return true;
            }
            return false;
        }

        static bool step_qos_routing(PacketConsumer& self, Net::ParsedPacket& pkt) {
            auto prio = self.processor.process(pkt);
            const size_t p_idx = static_cast<size_t>(prio);

            self.stats.pkts++;
            self.stats.bytes += pkt.raw_span.size();
            self.stats.prio_pkts[p_idx]++;
            self.stats.prio_bytes[p_idx] += pkt.raw_span.size();

            size_t mode_idx = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
            self.routes[mode_idx][p_idx](self.ctx, pkt.raw_span, p_idx, self.core_id);
            return true; // packet handed to route handler — end of pipeline
        }

        // --- Main packet entry point ---
        void on_packet_event(Net::ParsedPacket& pkt) {
            // Step array replaces all runtime if-else branches; null-check handles partially-filled arrays.
            for (auto step : pipeline) {
                if (step && step(*this, pkt)) break;
            }

            // Batch-commit to this core's telemetry slot every 32 packets (& 31 avoids division)
            if ((stats.pkts & 31) == 0) {
                Telemetry::instance().commit_batch(stats, core_id);
                stats.reset();
            }
        }
    };

    // Main application class
    class App {
        std::unique_ptr<Engine::RawSocketManager> iface_wan;
        std::unique_ptr<Engine::RawSocketManager> iface_lan;
        std::shared_ptr<Traffic::Shaper> global_shaper;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::shared_ptr<Logic::DnsEngine> dns_engine;
        std::shared_ptr<Logic::DhcpEngine> dhcp_engine;
        std::shared_ptr<Logic::FirewallEngine> firewall_engine;
        std::shared_ptr<Logic::UpnpEngine> upnp_engine;
        std::shared_ptr<QoSConfig> qos_config;
        int lan_fd_ = -1; // cached LAN fd for use by watchdog_loop

        std::shared_ptr<Traffic::Shaper> shaper_dl;
        std::shared_ptr<Traffic::Shaper> shaper_ul;
        double base_dl_mbps = 500.0;
        double base_ul_mbps = 50.0;
        std::shared_ptr<QoSConfig> device_shaper_dl;
        std::shared_ptr<QoSConfig> device_shaper_ul;

        std::thread worker_downstream;
        std::thread worker_upstream;
        std::thread watchdog;
        std::atomic<bool> running_workers{false};
        std::atomic<bool> running_watchdog{false};
        std::promise<void> shutdown_promise;
        std::future<void> shutdown_future;

    public:
        App() {
            shutdown_future = shutdown_promise.get_future();

            global_shaper = std::make_shared<Traffic::Shaper>(100.0);

            // Do NOT override service flags here — they are loaded from config.txt
            // before App is constructed (see main.cpp: Config::load_config() precedes App app).

            nat_engine      = std::make_shared<Logic::NatEngine>();
            dns_engine      = std::make_shared<Logic::DnsEngine>();
            dhcp_engine     = std::make_shared<Logic::DhcpEngine>(Config::ROUTER_IP);
            firewall_engine = std::make_shared<Logic::FirewallEngine>();
            // UpnpEngine constructor starts threads immediately; only create it when the
            // service is enabled so that enable_upnp=false actually suppresses the daemon.
            if (Config::global_state.enable_upnp.load(std::memory_order_relaxed))
                upnp_engine = std::make_shared<Logic::UpnpEngine>(nat_engine, Config::ROUTER_IP);
            qos_config    = std::make_shared<QoSConfig>();
            device_shaper_dl = std::make_shared<QoSConfig>();
            device_shaper_ul = std::make_shared<QoSConfig>();

            // Initialize QoS lock-free double-buffer table
            qos_config->update(Config::IP_LIMIT_TABLE, Config::IP_LIMIT_COUNT);
            nat_engine->set_wan_ip(Config::parse_ip_str(Config::ROUTER_IP));
        }
        std::expected<void, std::string> init() {
            Utils::Network::disable_hardware_offloads(Config::IFACE_WAN);
            Utils::Network::disable_hardware_offloads(Config::IFACE_LAN);

            iface_wan = std::make_unique<Engine::RawSocketManager>(Config::IFACE_WAN);
            iface_lan = std::make_unique<Engine::RawSocketManager>(Config::IFACE_LAN);

            if (auto r = iface_wan->init(); !r) return r;
            if (auto r = iface_lan->init(); !r) return r;

            return {};
        }

        void start() {
            std::println("=== Scalpel High-Performance Software Router ===");
            Telemetry::instance().bridge_mode.store(!Config::ENABLE_ACCELERATION, std::memory_order_relaxed);
            Scalpel::System::Optimizer::lock_cpu_frequency();

            running_watchdog.store(true, std::memory_order_relaxed);
            watchdog = std::thread([this]() { watchdog_loop(); });

            Utils::Network::force_arp_resolution(Utils::Network::get_gateway_ip());

            std::thread([]{
                Probe::Manager::run_internal_stress([](double mbps) {
                    Telemetry::instance().internal_limit_mbps.store(mbps, std::memory_order_relaxed);
                });
            }).detach();

            int fd_wan = iface_wan->get_fd();
            int fd_lan = iface_lan->get_fd();
            lan_fd_ = fd_lan; // cache before iface_lan is moved into the worker thread

            constexpr double dl = 500.0;
            constexpr double ul = 50.0;

            base_dl_mbps = dl;
            base_ul_mbps = ul;
            shaper_dl = std::make_shared<Traffic::Shaper>(base_dl_mbps);
            shaper_ul = std::make_shared<Traffic::Shaper>(base_ul_mbps);

            uint32_t gw_ip = Config::parse_ip_str(Config::ROUTER_IP);
            running_workers.store(true, std::memory_order_relaxed);
            worker_downstream = std::thread(
                [this](std::unique_ptr<Engine::RawSocketManager> iface, PacketWorkerConfig cfg) {
                    worker_event_loop(std::move(iface), std::move(cfg));
                }, std::move(iface_wan), PacketWorkerConfig{fd_lan, 2, shaper_dl, nat_engine, dns_engine, qos_config, device_shaper_dl, dhcp_engine, firewall_engine, gw_ip});

            worker_upstream = std::thread(
                [this](std::unique_ptr<Engine::RawSocketManager> iface, PacketWorkerConfig cfg) {
                    worker_event_loop(std::move(iface), std::move(cfg));
                }, std::move(iface_lan), PacketWorkerConfig{fd_wan, 3, shaper_ul, nat_engine, dns_engine, qos_config, device_shaper_ul, dhcp_engine, firewall_engine, gw_ip});

            std::println("[App] Data plane and control plane started.");
        }

        ~App() {
            // Stop data-plane workers first (Cores 2/3), then control-plane watchdog (Core 1).
            // Workers must stop before watchdog so the watchdog does not read stale shaper state.
            running_workers.store(false, std::memory_order_relaxed);
            if (worker_downstream.joinable()) worker_downstream.join();
            if (worker_upstream.joinable())   worker_upstream.join();
            running_watchdog.store(false, std::memory_order_relaxed);
            if (watchdog.joinable()) watchdog.join();
        }

        void stop() { shutdown_promise.set_value(); }

        void wait_for_shutdown() {
            shutdown_future.wait();
            std::println("\n[System] Shutdown signal received, core services terminated gracefully.");
        }

    private:
        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr, PacketWorkerConfig cfg) {
            Scalpel::System::Optimizer::set_current_thread_affinity(cfg.core_id);
            Scalpel::System::Optimizer::set_realtime_priority();
            std::println("[App] Core {} pipeline mounted and ready.", cfg.core_id);

            int rx_fd = rx_mgr->get_fd();
            PacketConsumer consumer(rx_fd, cfg);

            while (running_workers.load(std::memory_order_relaxed)) {
                rx_mgr->poll_and_dispatch([&consumer](std::span<uint8_t> raw_span) {
                    auto pkt_ctx = Net::ParsedPacket::parse(raw_span);
                    consumer.on_packet_event(pkt_ctx);
                }, 1);

                // Periodic QoS queue drain and hardware TX unblock
                if (cfg.global_shaper) cfg.global_shaper->process_queue(cfg.tx_fd);
                // Only scan per-IP shaper table when at least one IP limit is configured,
                // avoiding 256 empty-slot iterations per poll cycle when no limits are active.
                if (consumer.qos_config && Config::IP_LIMIT_ACTIVE.load(std::memory_order_relaxed)) {
                    size_t active_idx = consumer.qos_config->active_idx.load(std::memory_order_relaxed);
                    consumer.qos_config->buffers[active_idx].for_each_occupied([&](auto& shaper) {
                        shaper->process_queue(cfg.tx_fd);
                    });
                }

                // Lock-free heartbeat tick: single atomic add, zero syscalls
                Telemetry::instance().core_metrics[cfg.core_id].last_heartbeat.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void watchdog_loop() {
            Scalpel::System::Optimizer::set_current_thread_affinity(1);
            auto& tel = Telemetry::instance();
            
            int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
            if (tfd == -1) return;

            struct itimerspec its{};
            its.it_value.tv_sec = 1;      // first expiration after 1 second
            its.it_value.tv_nsec = 0;
            its.it_interval.tv_sec = 1;   // fire every 1 second
            its.it_interval.tv_nsec = 0;

            if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
                close(tfd);
                return;
            }

            uint64_t expirations;
            uint64_t last_bytes[4] = {0, 0, 0, 0};
            uint64_t last_ticks[4] = {0, 0, 0, 0};
            // /proc/stat CPU accounting: {user,nice,system,idle,iowait,irq,softirq}
            uint64_t stat_idle[4]  = {0, 0, 0, 0};
            uint64_t stat_total[4] = {0, 0, 0, 0};
            uint64_t watchdog_tick = 0;
            int last_throttle_pct = 100;

            auto& si = tel.sys_info;
            struct pollfd pfds[2]{};
            pfds[0] = { tfd, POLLIN, 0 };
            pfds[1] = { si.rescan_fd, POLLIN, 0 };

            // iface scan lambda — shared by 5-tick refresh and on-demand rescan paths
            auto scan_ifaces = [&]() {
                uint8_t cnt = 0;
                DIR* d = opendir("/sys/class/net");
                if (d) {
                    struct dirent* de;
                    while ((de = readdir(d)) != nullptr &&
                           cnt < Telemetry::SystemInfo::MAX_IFACES) {
                        if (de->d_name[0] == '.' ||
                            strncmp(de->d_name, "lo", 3) == 0) continue;
                        strncpy(si.ifaces[cnt].name.data(), de->d_name, 15);
                        si.ifaces[cnt].name[15] = '\0';
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
                        strncpy(si.ifaces[cnt].operstate.data(),
                            sbuf[0] ? sbuf : "unknown", 7);
                        si.ifaces[cnt].operstate[7] = '\0';
                        ++cnt;
                    }
                    closedir(d);
                }
                // Release store: Qt acquire-load in scan_interfaces() sees completed writes
                si.iface_count.store(cnt, std::memory_order_release);
            };

            while (running_watchdog.load(std::memory_order_relaxed)) {
                // poll on timerfd (1Hz) + rescan_fd (on-demand UI request)
                int nfds = (si.rescan_fd >= 0) ? 2 : 1;
                int pret = poll(pfds, nfds, 1000);
                if (pret <= 0) continue;

                bool timer_fired = (pfds[0].revents & POLLIN) != 0;
                bool force_scan  = false;

                if (timer_fired && ::read(tfd, &expirations, sizeof(expirations)) <= 0)
                    timer_fired = false;
                if (nfds == 2 && (pfds[1].revents & POLLIN)) {
                    uint64_t val;
                    ::eventfd_read(si.rescan_fd, &val);
                    force_scan = true;
                }

                // On-demand rescan only: iface scan + signal UI, skip all 1Hz work
                if (force_scan && !timer_fired) {
                    scan_ifaces();
                    if (si.done_fd >= 0) ::eventfd_write(si.done_fd, 1);
                    continue;
                }

                if (!timer_fired) continue;

                // Read CPU temperature via raw fd (Core 1, 1Hz) — no heap allocation, no ifstream overhead
                {
                    char tbuf[16]{};
                    int thermal_fd = ::open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
                    if (thermal_fd >= 0) {
                        ssize_t n = ::read(thermal_fd, tbuf, sizeof(tbuf) - 1);
                        ::close(thermal_fd);
                        if (n > 0) tel.cpu_temp_celsius.store(atof(tbuf) / 1000.0, std::memory_order_relaxed);
                    }
                }

                // Read per-core CPU load from /proc/stat (1Hz)
                {
                    char sbuf[1024]{};
                    int sfd = ::open("/proc/stat", O_RDONLY);
                    if (sfd >= 0) {
                        ssize_t n = ::read(sfd, sbuf, sizeof(sbuf) - 1);
                        ::close(sfd);
                        if (n > 0) {
                            sbuf[n] = '\0';
                            const char* p = sbuf;
                            for (int ci = 0; ci < 4; ++ci) {
                                // Find line "cpuN user nice system idle iowait irq softirq"
                                char tag[8]; snprintf(tag, sizeof(tag), "cpu%d ", ci);
                                const char* ln = strstr(p, tag);
                                if (!ln) break;
                                ln += strlen(tag);
                                uint64_t user, nice, sys, idle, iowait, irq, softirq;
                                if (sscanf(ln, "%lu %lu %lu %lu %lu %lu %lu",
                                        &user, &nice, &sys, &idle, &iowait, &irq, &softirq) == 7) {
                                    uint64_t total = user + nice + sys + idle + iowait + irq + softirq;
                                    uint64_t dt = total - stat_total[ci];
                                    uint64_t di = idle  - stat_idle[ci];
                                    int pct = (dt > 0) ? static_cast<int>(100 * (dt - di) / dt) : 0;
                                    tel.core_metrics[ci].cpu_load_pct.store(pct, std::memory_order_relaxed);
                                    stat_total[ci] = total;
                                    stat_idle[ci]  = idle;
                                }
                            }
                        }
                    }
                }

                // Refresh system info every 5 ticks (5 seconds) for UI display — Core 1 only
                bool did_iface_scan = false;
                ++watchdog_tick;
                if (watchdog_tick % 5 == 0) {
                    // Helper: read a sysfs/proc file into a fixed-size buffer via raw fd
                    auto read_sysfd = [](const char* path, std::span<char> out) {
                        int fd = ::open(path, O_RDONLY);
                        if (fd < 0) { out[0] = '\0'; return; }
                        ssize_t n = ::read(fd, out.data(), out.size() - 1);
                        ::close(fd);
                        if (n > 0) {
                            out[n] = '\0';
                            if (out[n - 1] == '\n') out[n - 1] = '\0'; // strip trailing newline
                        } else {
                            out[0] = '\0';
                        }
                    };

                    read_sysfd("/etc/hostname", std::span<char>(si.hostname));
                    read_sysfd("/proc/version", std::span<char>(si.kernel_short));

                    // Uptime: parse first numeric field from /proc/uptime
                    char ubuf[32]{};
                    read_sysfd("/proc/uptime", std::span<char>(ubuf));
                    if (ubuf[0]) si.uptime_seconds.store(
                        static_cast<uint64_t>(atof(ubuf)), std::memory_order_relaxed);

                    // Memory: scan /proc/meminfo for MemTotal and MemAvailable
                    char mbuf[512]{};
                    int mfd = ::open("/proc/meminfo", O_RDONLY);
                    if (mfd >= 0) {
                        ssize_t nr = ::read(mfd, mbuf, sizeof(mbuf) - 1);
                        ::close(mfd);
                        if (nr > 0) {
                            uint64_t total = 0, avail = 0;
                            const char* mt = strstr(mbuf, "MemTotal:");
                            const char* ma = strstr(mbuf, "MemAvailable:");
                            if (mt) sscanf(mt, "MemTotal: %lu", &total);
                            if (ma) sscanf(ma, "MemAvailable: %lu", &avail);
                            si.mem_total_kb.store(total, std::memory_order_relaxed);
                            si.mem_avail_kb.store(avail, std::memory_order_relaxed);
                        }
                    }

                    // Scan /sys/class/net via lambda — raw fd + POSIX readdir, Core 1 only
                    scan_ifaces();
                    did_iface_scan = true;

                    // Scan /proc/net/arp into Telemetry::device_table (Core 1, 5Hz)
                    {
                        char arp_buf[4096]{};
                        int arp_fd = ::open("/proc/net/arp", O_RDONLY);
                        if (arp_fd >= 0) {
                            ssize_t nr = ::read(arp_fd, arp_buf, sizeof(arp_buf) - 1);
                            ::close(arp_fd);
                            uint8_t dcnt = 0;
                            if (nr > 0) {
                                // Skip header line
                                char* line = arp_buf;
                                char* end  = arp_buf + nr;
                                while (line < end && *line != '\n') ++line;
                                if (line < end) ++line;
                                while (line < end && dcnt < Telemetry::MAX_TRACKED_DEVICES) {
                                    char ip_str[20]{}, hw[8]{}, flags[8]{}, mac_str[20]{};
                                    if (sscanf(line, "%19s %7s %7s %19s", ip_str, hw, flags, mac_str) == 4) {
                                        uint32_t ip = inet_addr(ip_str);
                                        if (ip != INADDR_NONE && strcmp(flags, "0x0") != 0) {
                                            tel.device_table[dcnt].ip = ip;
                                            strncpy(tel.device_table[dcnt].mac.data(), mac_str, 17);
                                            tel.device_table[dcnt].mac[17] = '\0';
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

                // If on-demand rescan was requested in same tick but 5-tick block didn't run, do it now
                if (force_scan && !did_iface_scan) scan_ifaces();
                if (force_scan && si.done_fd >= 0) ::eventfd_write(si.done_fd, 1);

                // Map-reduce bandwidth aggregation
                uint64_t total_bytes_down = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
                uint64_t total_bytes_up = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);

                double dl_mbps = (total_bytes_down - last_bytes[2]) * 8.0 / 1e6;
                double ul_mbps = (total_bytes_up - last_bytes[3]) * 8.0 / 1e6;

                std::println("[Monitor] DL: {:.2f} Mbps | UL: {:.2f} Mbps | Mode: {}",
                    dl_mbps, ul_mbps, tel.bridge_mode.load(std::memory_order_relaxed) ? "Bridge" : "Accel");

                last_bytes[2] = total_bytes_down;
                last_bytes[3] = total_bytes_up;

                // Sync per-device block list and rate shapers when GUI sets DEVICE_POLICY_DIRTY
                if (Config::DEVICE_POLICY_DIRTY.exchange(false, std::memory_order_acq_rel)) {
                    // Rebuild firewall blocked IP list
                    if (firewall_engine) firewall_engine->sync_blocked_ips();

                    // Rebuild device QoS DL table (keyed by LAN IP, used in Core 2 daddr lookup)
                    if (device_shaper_dl) {
                        size_t active = device_shaper_dl->active_idx.load(std::memory_order_relaxed);
                        size_t inactive = 1 - active;
                        device_shaper_dl->buffers[inactive] = {};
                        for (size_t i = 0; i < Config::DEVICE_POLICY_COUNT; ++i) {
                            const auto& p = Config::DEVICE_POLICY_TABLE[i];
                            if (p.rate_limited && p.ip != 0)
                                device_shaper_dl->buffers[inactive].insert(p.ip, std::make_shared<Traffic::Shaper>(p.dl_mbps));
                        }
                        device_shaper_dl->active_idx.store(inactive, std::memory_order_release);
                    }

                    // Rebuild device QoS UL table (keyed by LAN IP, used in Core 3 saddr lookup)
                    if (device_shaper_ul) {
                        size_t active = device_shaper_ul->active_idx.load(std::memory_order_relaxed);
                        size_t inactive = 1 - active;
                        device_shaper_ul->buffers[inactive] = {};
                        for (size_t i = 0; i < Config::DEVICE_POLICY_COUNT; ++i) {
                            const auto& p = Config::DEVICE_POLICY_TABLE[i];
                            if (p.rate_limited && p.ip != 0)
                                device_shaper_ul->buffers[inactive].insert(p.ip, std::make_shared<Traffic::Shaper>(p.ul_mbps));
                        }
                        device_shaper_ul->active_idx.store(inactive, std::memory_order_release);
                    }
                    std::println("[Device] Policy synced: {} entries", Config::DEVICE_POLICY_COUNT);
                }

                // Apply DNS config changes when GUI sets dns_config_dirty
                if (dns_engine && tel.dns_config_dirty.exchange(false, std::memory_order_acq_rel)) {
                    uint32_t primary   = Config::parse_ip_str(Config::DNS_UPSTREAM_PRIMARY);
                    uint32_t secondary = Config::parse_ip_str(Config::DNS_UPSTREAM_SECONDARY);
                    dns_engine->set_upstream(primary, secondary);
                    dns_engine->set_redirect(Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed));
                    dns_engine->reload_static_records();
                    std::println("[DNS] Config applied: upstream {} / {}, redirect={}, static={} records",
                        Config::DNS_UPSTREAM_PRIMARY, Config::DNS_UPSTREAM_SECONDARY,
                        Config::DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed) ? "on" : "off",
                        Config::STATIC_DNS_COUNT);
                }

                // Reconfigure DHCP engine when GUI has changed pool settings
                if (dhcp_engine && tel.dhcp_config_dirty.exchange(false, std::memory_order_acq_rel)) {
                    uint32_t start_ip = Config::parse_ip_str(Config::DHCP_POOL_START);
                    uint32_t end_ip   = Config::parse_ip_str(Config::DHCP_POOL_END);
                    dhcp_engine->reconfigure(start_ip, end_ip, Config::DHCP_LEASE_SECONDS);
                    std::println("[DHCP] Pool reconfigured: {} – {}, lease {}s",
                        Config::DHCP_POOL_START, Config::DHCP_POOL_END, Config::DHCP_LEASE_SECONDS);
                }

                // Drive control-plane engines at 1Hz (low-frequency async work pool)
                if (nat_engine) nat_engine->tick();
                if (dns_engine) {
                    dns_engine->tick();
                    dns_engine->process_background_tasks();
                }
                if (dhcp_engine) {
                    dhcp_engine->process_background_tasks(lan_fd_); // send replies out the LAN port
                }
                if (firewall_engine) {
                    firewall_engine->tick();
                    firewall_engine->cleanup();
                }

                // Apply QoS throttle from GUI slider (written by Core 0, consumed here on Core 1)
                {
                    int pct = tel.qos_throttle_pct.load(std::memory_order_relaxed);
                    if (pct != last_throttle_pct) {
                        last_throttle_pct = pct;
                        double factor = pct / 100.0;
                        if (shaper_dl) shaper_dl->set_rate_limit(base_dl_mbps * factor);
                        if (shaper_ul) shaper_ul->set_rate_limit(base_ul_mbps * factor);
                        std::println("[QoS] Throttle {}% — DL {:.1f} Mbps / UL {:.1f} Mbps",
                            pct, base_dl_mbps * factor, base_ul_mbps * factor);
                    }
                }

                // Heartbeat-based fault detection (high/low frequency decoupling)
                uint64_t current_tick = tel.core_metrics[2].last_heartbeat.load(std::memory_order_relaxed);
                last_ticks[2] = current_tick;
            }
            close(tfd);
        }
    };
}











