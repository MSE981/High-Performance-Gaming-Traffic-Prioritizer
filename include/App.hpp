#pragma once
#include <thread>
#include <stop_token>
#include <memory>
#include <expected>
#include <chrono>
#include <ctime>
#include <sys/socket.h>
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
#include "Indicator.hpp"
#include "Scheduler.hpp"
#include <print>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <poll.h> 
#include <future>
#include <array>
#include <map>
#include <sys/timerfd.h>
#include <unistd.h>

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
        std::array<Entry, Capacity> table{};
        
    private:
        static uint32_t fnv1a_hash(uint32_t val) {
            uint32_t h = 2166136261U;
            h ^= (val & 0xFF); h *= 16777619U;
            h ^= ((val >> 8) & 0xFF); h *= 16777619U;
            h ^= ((val >> 16) & 0xFF); h *= 16777619U;
            h ^= ((val >> 24) & 0xFF); h *= 16777619U;
            return h;
        }

    public:
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

        void update(const std::map<uint32_t, double>& limits) {
            size_t active = active_idx.load(std::memory_order_relaxed);
            size_t inactive = 1 - active;
            buffers[inactive] = {}; // Zero Allocation Refresh
            for (auto const& [ip, rate] : limits) {
                buffers[inactive].insert(ip, std::make_shared<Traffic::Shaper>(rate));
            }
            active_idx.store(inactive, std::memory_order_release);
        }
    };

    // 数据面：事件消费者模型 (PacketConsumer) - 处理核心逻辑
    class PacketConsumer {
    public:
        int rx_fd; // Socket FD for reading/bouncing
        int tx_fd; // Socket FD for writing upstream
        int core_id; // 具有核心身份
        Telemetry::BatchStats stats;
        Logic::HeuristicProcessor processor;
        
        RouteContext ctx;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::shared_ptr<Logic::DnsEngine> dns_engine;
        std::shared_ptr<QoSConfig> qos_config;
        std::shared_ptr<Logic::DhcpEngine> dhcp_engine;
        uint32_t gateway_ip = 0;

        std::array<std::array<RouteFunc, 3>, 2> routes;

        // 核心解耦：拦截器流水线机制 (Callback-based Pipeline)
        using PipelineStep = bool (*)(PacketConsumer& self, Net::ParsedPacket& pkt);
        std::array<PipelineStep, 6> pipeline;

        PacketConsumer(int rx_fd, int tx_fd, int cid, std::shared_ptr<Traffic::Shaper> global_shaper, std::shared_ptr<Logic::NatEngine> nat, std::shared_ptr<Logic::DnsEngine> dns, std::shared_ptr<QoSConfig> qos, std::shared_ptr<Logic::DhcpEngine> dhcp, uint32_t gw_ip)
            : rx_fd(rx_fd), tx_fd(tx_fd), core_id(cid), ctx{tx_fd, global_shaper}, nat_engine(nat), dns_engine(dns), qos_config(qos), dhcp_engine(dhcp), gateway_ip(gw_ip) {

            routes = {{
                { fast_path_handler, fast_path_handler, shaper_handler }, // 加速模式
                { fast_path_handler, fast_path_handler, fast_path_handler } // 透明网桥模式
            }};

            // 编译期决断组装流水线：彻底消灭运行时的 if-else 嵌套
            if (core_id == 2) {
                pipeline = { step_dhcp_interceptor, step_dns_interceptor, step_nat_downstream, step_ip_shaper_downstream, step_qos_routing };
            } else {
                pipeline = {
                step_dhcp_interceptor,
                step_dns_interceptor,
                step_local_delivery_blocker,
                step_nat_downstream,
                step_nat_upstream,
                step_qos_routing
            };
        }
        } // 补充闭合 PacketConsumer 构造函数

        // --- 回调流水线处理模块 (Pipeline Handlers) ---
        
        static bool step_local_delivery_blocker(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return false;
            if (self.core_id == 3) { // Only checking LAN -> WAN (Upstream)
                uint32_t daddr = pkt.ipv4->daddr;
                if (daddr == self.gateway_ip || 
                    daddr == 0xFAFFFFEF || // 239.255.255.250 SSDP 组播 (Little Endian)
                    daddr == 0xFFFFFFFF) { // 广播
                    // 返回 true 截断用户面转发流水线。
                    // 效果：包未通过 `tx_fd` (WAN) 发送，自然落入 Linux 本地协议栈处理（如 SSDP, SSH, Nginx 等）
                    return true; 
                }
            }
            return false;
        }
        
        static bool step_dhcp_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_dhcp.load(std::memory_order_relaxed)) return false;
            
            // Core 3 (LAN -> WAN) 负责拦截向网关获取 IP 的 DHCP 请求包
            if (self.core_id == 3 && pkt.is_valid_ipv4() && pkt.l4_protocol == 17) {
                auto udp = pkt.udp();
                if (udp && (ntohs(udp->dest) == 67 || ntohs(udp->dest) == 68)) {
                    if (self.dhcp_engine) self.dhcp_engine->intercept_request(pkt);
                    return true; // 拦截成功，阻塞转发到公网
                }
            }
            return false;
        }

        static bool step_dns_interceptor(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (!Config::global_state.enable_dns_cache.load(std::memory_order_relaxed)) return false;
            
            if (self.core_id == 3) {
                // Core 3 (Upstream: LAN -> WAN): 截取用户的 Query
                if (self.dns_engine && self.dns_engine->process_query(pkt, self.rx_fd)) {
                    return true; // 缓存命中且已原地弹回 LAN 口 (rx_fd)，即刻阻断流水线
                }
            } else if (self.core_id == 2) {
                // Core 2 (Downstream: WAN -> LAN): 监视公网回馈的 Response
                if (self.dns_engine) {
                    self.dns_engine->intercept_response(pkt);
                } // 从不阻断 Response 放行给玩家
            }
            return false; 
        }

        static bool step_nat_downstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
            if (self.nat_engine) self.nat_engine->process_inbound(pkt);
            return false; // 继续流水线
        }
        
        static bool step_nat_upstream(PacketConsumer& self, Net::ParsedPacket& pkt) {
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
                    return true; // 拦截成功，终止流水线
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

        static bool step_qos_routing(PacketConsumer& self, Net::ParsedPacket& pkt) {
            auto prio = self.processor.process(pkt);
            const size_t p_idx = static_cast<size_t>(prio);

            self.stats.pkts++;
            self.stats.bytes += pkt.raw_span.size();
            self.stats.prio_pkts[p_idx]++;
            self.stats.prio_bytes[p_idx] += pkt.raw_span.size();

            size_t mode_idx = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
            self.routes[mode_idx][p_idx](self.ctx, pkt.raw_span, p_idx, self.core_id);
            return true; // 数据包已进入路由发送，生命周期结束
        }

        // --- Main packet entry point ---
        void on_packet_event(Net::ParsedPacket& pkt) {
            // Callback-based pipeline: step array replaces all runtime if-else branches.
            // Null-check guards against partially-filled pipeline arrays (e.g. core 2 uses 5 of 6 slots).
            for (auto step : pipeline) {
                if (step && step(*this, pkt)) break;
            }

            // 批量提交至当前 core 的专有槽位
            if (stats.pkts % 32 == 0) {
                Telemetry::instance().commit_batch(stats, core_id);
                stats.reset();
            }
        }
    };

    // 主框架：App 类
    class App {
        std::unique_ptr<Engine::RawSocketManager> iface_wan;
        std::unique_ptr<Engine::RawSocketManager> iface_lan;
        std::shared_ptr<Traffic::Shaper> global_shaper;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::shared_ptr<Logic::DnsEngine> dns_engine;
        std::shared_ptr<Logic::DhcpEngine> dhcp_engine;
        std::shared_ptr<Logic::UpnpEngine> upnp_engine;
        std::shared_ptr<QoSConfig> qos_config;
        HW::RGBLed led;
        int lan_fd_ = -1;
        
        std::jthread worker_downstream;
        std::jthread worker_upstream;
        std::jthread watchdog;
        std::promise<void> shutdown_promise;
        std::future<void> shutdown_future;

    public:
        App() {
            shutdown_future = shutdown_promise.get_future();

            global_shaper = std::make_shared<Traffic::Shaper>(100.0);

            // Do NOT override service flags here — they are loaded from config.txt
            // before App is constructed (see main.cpp: Config::load_config() precedes App app).

            nat_engine = std::make_shared<Logic::NatEngine>();
            dns_engine = std::make_shared<Logic::DnsEngine>();
            dhcp_engine = std::make_shared<Logic::DhcpEngine>(Config::ROUTER_IP);
            upnp_engine = std::make_shared<Logic::UpnpEngine>(nat_engine, Config::ROUTER_IP);
            qos_config = std::make_shared<QoSConfig>();

            // Initialize QoS lock-free double-buffer table
            qos_config->update(Config::IP_LIMIT_MAP);
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

            watchdog = std::jthread([this](std::stop_token st) { watchdog_loop(st); });

            std::string gw_ip = Utils::Network::get_gateway_ip();
            Utils::Network::force_arp_resolution(gw_ip);

            Probe::Manager::run_internal_stress();

            int fd_wan = iface_wan->get_fd();
            int fd_lan = iface_lan->get_fd();
            lan_fd_ = fd_lan; // 缓存 fd，防止 move 后 use-after-move

            auto& tel = Telemetry::instance();
            double dl = tel.isp_down_limit_mbps.load() > 1.0 ? tel.isp_down_limit_mbps.load() : 500.0;
            double ul = tel.isp_up_limit_mbps.load() > 1.0 ? tel.isp_up_limit_mbps.load() : 50.0;

            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl * 0.85);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul * 0.85);

            worker_downstream = std::jthread(
                [this](std::stop_token st, std::unique_ptr<Engine::RawSocketManager> iface, int tx, int c, std::shared_ptr<Traffic::Shaper> sh, std::shared_ptr<Logic::NatEngine> ne, std::shared_ptr<Logic::DnsEngine> de, std::shared_ptr<QoSConfig> qc, std::shared_ptr<Logic::DhcpEngine> d) {
                    worker_event_loop(std::move(st), std::move(iface), tx, c, sh, ne, de, qc, d);
                }, std::move(iface_wan), fd_lan, 2, shaper_dl, nat_engine, dns_engine, qos_config, dhcp_engine);

            worker_upstream = std::jthread(
                [this](std::stop_token st, std::unique_ptr<Engine::RawSocketManager> iface, int tx, int c, std::shared_ptr<Traffic::Shaper> sh, std::shared_ptr<Logic::NatEngine> ne, std::shared_ptr<Logic::DnsEngine> de, std::shared_ptr<QoSConfig> qc, std::shared_ptr<Logic::DhcpEngine> d) {
                    worker_event_loop(std::move(st), std::move(iface), tx, c, sh, ne, de, qc, d);
                }, std::move(iface_lan), fd_wan, 3, shaper_ul, nat_engine, dns_engine, qos_config, dhcp_engine);

            std::println("[App] 核心数据平面与控制平面已启动完成.");
        }

        void stop() { shutdown_promise.set_value(); }

        void wait_for_shutdown() {
            shutdown_future.wait();
            std::println("\n[System] 收到退出信号，核心服务已优雅关闭.");
        }

    private:
        void worker_event_loop(std::stop_token st, std::unique_ptr<Engine::RawSocketManager> rx_mgr, int tx_fd, int core, std::shared_ptr<Traffic::Shaper> shpr, std::shared_ptr<Logic::NatEngine> nat, std::shared_ptr<Logic::DnsEngine> dns, std::shared_ptr<QoSConfig> qos, std::shared_ptr<Logic::DhcpEngine> dhcp) {
            Scalpel::System::Optimizer::set_current_thread_affinity(core);
            Scalpel::System::Optimizer::set_realtime_priority();
            std::println("[App] Core {} Pipeline 挂载就绪.", core);

            int rx_fd = rx_mgr->get_fd();
            PacketConsumer consumer(rx_fd, tx_fd, core, shpr, nat, dns, qos, dhcp, Config::parse_ip_str(Config::ROUTER_IP));

            while (!st.stop_requested()) {
                rx_mgr->poll_and_dispatch([&consumer](std::span<uint8_t> raw_span) {
                    auto pkt_ctx = Net::ParsedPacket::parse(raw_span);
                    consumer.on_packet_event(pkt_ctx);
                }, 1);

                // QoS 队列周期性派发与硬件解堵塞
                if (shpr) shpr->process_queue(tx_fd);
                if (consumer.qos_config) {
                    size_t active_idx = consumer.qos_config->active_idx.load(std::memory_order_relaxed);
                    for (auto& entry : consumer.qos_config->buffers[active_idx].table) { // Assuming table is accessible or find is used
                        if (entry.occupied && entry.value) {
                            entry.value->process_queue(tx_fd);
                        }
                    }
                }
                
                // 无锁无调用的心跳滴答 (Tick-based Heartbeat)：底层汇编的单周期单指令，0 系统调用！
                Telemetry::instance().core_metrics[core].last_heartbeat.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void watchdog_loop(std::stop_token st) {
            Scalpel::System::Optimizer::set_current_thread_affinity(1);
            auto& tel = Telemetry::instance();
            
            int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
            if (tfd == -1) return;

            struct itimerspec its{};
            its.it_value.tv_sec = 1;      // 1秒后启动
            its.it_value.tv_nsec = 0;
            its.it_interval.tv_sec = 1;   // 每隔1秒触发一次
            its.it_interval.tv_nsec = 0;

            if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
                close(tfd);
                return;
            }

            uint64_t expirations;
            uint64_t last_bytes[4] = {0, 0, 0, 0};
            uint64_t last_ticks[4] = {0, 0, 0, 0};

            struct pollfd pfd{};
            pfd.fd = tfd;
            pfd.events = POLLIN;

            while (!st.stop_requested()) {
                // 使用 poll 防止 fd 失效或者意外 O_NONBLOCK 导致的 CPU 空转 Spin Lock 死循环
                int pret = poll(&pfd, 1, 1000); 
                if (pret <= 0) continue;

                if (read(tfd, &expirations, sizeof(expirations)) <= 0) continue;
                
                // Map-Reduce 聚合逻辑
                uint64_t total_bytes_down = tel.core_metrics[2].bytes.load(std::memory_order_relaxed);
                uint64_t total_bytes_up = tel.core_metrics[3].bytes.load(std::memory_order_relaxed);

                double dl_mbps = (total_bytes_down - last_bytes[2]) * 8.0 / 1e6;
                double ul_mbps = (total_bytes_up - last_bytes[3]) * 8.0 / 1e6;
                
                std::println("[Monitor] DL: {:.2f} Mbps | UL: {:.2f} Mbps | Mode: {}", 
                    dl_mbps, ul_mbps, tel.bridge_mode.load() ? "Bridge" : "Accel");

                last_bytes[2] = total_bytes_down;
                last_bytes[3] = total_bytes_up;

                // 喂给 Watchdog 各大控制面引擎运行（低频异步运算池）
                // The original code had `app->nat_engine` etc. but `app` is not defined here.
                // Assuming `this->nat_engine` is intended.
                if (nat_engine) nat_engine->tick();
                if (dns_engine) {
                    dns_engine->tick();
                    dns_engine->process_background_tasks(); // 异步分解 DNS
                }
                if (dhcp_engine) {
                    // 回答下发向 LAN 口
                    dhcp_engine->process_background_tasks(lan_fd_);
                }

                // 基于核心心跳滴答的故障检测 (高低频解耦哲学)
                uint64_t current_tick = tel.core_metrics[2].last_heartbeat.load(std::memory_order_relaxed);
                if (current_tick == last_ticks[2]) led.set_red(); // 1 秒内滴答值毫无变化，说明线程绝地卡死
                else led.set_green();
                last_ticks[2] = current_tick;
            }
            close(tfd);
        }
    };
}











