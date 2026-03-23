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
#include "NatEngine.hpp" // Added NatEngine.hpp
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

    // 数据面：基于函数指针的高效静态分发 (消除虚函数开销)
    struct RouteContext {
        int tx_fd;
        std::shared_ptr<Traffic::Shaper> shaper;
    };

    using RouteFunc = void (*)(const RouteContext& ctx, std::span<uint8_t> pkt, size_t prio_idx, int core_id);

    static void fast_path_handler(const RouteContext& ctx, std::span<uint8_t> pkt, size_t prio_idx, int core_id) {
        if (send(ctx.tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) < 0) {
            // 遵守 ISR "立即完成" 与绝对零阻塞原则：如果发送缓冲区满，则果断尾丢弃，绝不挂起或轮询重试
            Telemetry::instance().core_metrics[core_id].dropped[prio_idx].fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void shaper_handler(const RouteContext& ctx, std::span<uint8_t> pkt, size_t /*prio_idx*/, int /*core_id*/) {
        if (ctx.shaper) ctx.shaper->enqueue_normal(pkt);
    }

    // 辅助工具：基于 FNV-1a 静态哈希表 (针对嵌入式实时性能优化)
    template<typename T, size_t Capacity = 256>
    class StaticIpMap {
        struct Entry {
            uint32_t key = 0;
            T value = nullptr;
            bool occupied = false;
        };
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

    // 数据面：事件消费者模型 (PacketConsumer) - 处理核心逻辑
    class PacketConsumer {
    public:
        int tx_fd;
        int core_id; // 增加核心 ID 标识
        Telemetry::BatchStats stats;
        Logic::HeuristicProcessor processor;
        
        RouteContext ctx;
        std::shared_ptr<Logic::NatEngine> nat_engine;
        std::array<std::array<RouteFunc, 3>, 2> routes;
        StaticIpMap<std::shared_ptr<Traffic::Shaper>, 256> ip_shaper_map;

        // 核心解耦：拦截器流水线机制 (Callback-based Pipeline)
        using PipelineStep = bool (*)(PacketConsumer& self, std::span<uint8_t> pkt);
        std::array<PipelineStep, 3> pipeline;

        PacketConsumer(int tx_fd, int cid, std::shared_ptr<Traffic::Shaper> global_shaper, std::shared_ptr<Logic::NatEngine> nat)
            : tx_fd(tx_fd), core_id(cid), ctx{tx_fd, global_shaper}, nat_engine(nat) {

            routes = {{
                { fast_path_handler, fast_path_handler, shaper_handler }, // 加速模式
                { fast_path_handler, fast_path_handler, fast_path_handler } // 透明网桥模式
            }};

            for (auto const& [ip, rate] : Config::IP_LIMIT_MAP) {
                auto s = std::make_shared<Traffic::Shaper>(rate);
                ip_shaper_map.insert(ip, s);
            }

            // 编译期决断组装流水线：彻底消灭运行时的 if-else 嵌套
            if (core_id == 2) {
                pipeline = { step_nat_downstream, step_ip_shaper_downstream, step_qos_routing };
            } else {
                pipeline = { step_nat_upstream, step_ip_shaper_upstream, step_qos_routing };
            }
        }

        // --- 回调流水线处理模块 (Pipeline Handlers) ---

        static bool step_nat_downstream(PacketConsumer& self, std::span<uint8_t> pkt) {
            if (self.nat_engine) self.nat_engine->process_inbound(pkt);
            return false; // 继续流水线
        }
        
        static bool step_nat_upstream(PacketConsumer& self, std::span<uint8_t> pkt) {
            if (self.nat_engine) self.nat_engine->process_outbound(pkt);
            return false;
        }

        static bool step_ip_shaper_downstream(PacketConsumer& self, std::span<uint8_t> pkt) {
            if (pkt.size() > 34) {
                uint32_t target_ip = *reinterpret_cast<const uint32_t*>(&pkt[30]);
                auto target_shaper = self.ip_shaper_map.find(target_ip);
                if (target_shaper) {
                    RouteContext ip_ctx{self.tx_fd, target_shaper};
                    shaper_handler(ip_ctx, pkt, 2, self.core_id);
                    return true; // 拦截成功，终止流水线
                }
            }
            return false;
        }

        static bool step_ip_shaper_upstream(PacketConsumer& self, std::span<uint8_t> pkt) {
            if (pkt.size() > 34) {
                uint32_t target_ip = *reinterpret_cast<const uint32_t*>(&pkt[26]);
                auto target_shaper = self.ip_shaper_map.find(target_ip);
                if (target_shaper) {
                    RouteContext ip_ctx{self.tx_fd, target_shaper};
                    shaper_handler(ip_ctx, pkt, 2, self.core_id);
                    return true;
                }
            }
            return false;
        }

        static bool step_qos_routing(PacketConsumer& self, std::span<uint8_t> pkt) {
            auto prio = self.processor.process(pkt);
            const size_t p_idx = static_cast<size_t>(prio);

            self.stats.pkts++;
            self.stats.bytes += pkt.size();
            self.stats.prio_pkts[p_idx]++;
            self.stats.prio_bytes[p_idx] += pkt.size();

            size_t mode_idx = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
            self.routes[mode_idx][p_idx](self.ctx, pkt, p_idx, self.core_id);
            return true; // 数据包已进入路由发送，生命周期结束
        }

        // --- 主入口事作 ---
        void on_packet_event(std::span<uint8_t> pkt) {
            // Callback-based Pipeline: 以数组步进取代所有的运行态条件分支
            for (auto step : pipeline) {
                if (step(*this, pkt)) break;
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
        
        std::jthread worker_wan_lan;
        std::jthread worker_lan_wan;
        std::jthread watchdog;

    public:
        App() {
            global_shaper = std::make_shared<Traffic::Shaper>(100.0); // 默认全局限速 100Mbps
            nat_engine = std::make_shared<Logic::NatEngine>();
            // 默认网关 IP (可通过 Config 更新)
            nat_engine->set_wan_ip(Config::parse_ip_str("192.168.1.100"));
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

        void run() {
            std::println("=== Scalpel High-Performance Software Router ===");
            Telemetry::instance().bridge_mode.store(!Config::ENABLE_ACCELERATION, std::memory_order_relaxed);
            Scalpel::System::lock_cpu_frequency();

            std::jthread ui_thread([this](std::stop_token st) { ui_render_loop(st); });
            watchdog = std::jthread([this](std::stop_token st) { watchdog_loop(st); });

            std::string gw_ip = Utils::Network::get_gateway_ip();
            Utils::Network::force_arp_resolution(gw_ip);

            Probe::Manager::run_internal_stress();

            int fd_wan = iface_wan->get_fd();
            int fd_lan = iface_lan->get_fd();

            auto& tel = Telemetry::instance();
            double dl = tel.isp_down_limit_mbps.load() > 1.0 ? tel.isp_down_limit_mbps.load() : 500.0;
            double ul = tel.isp_up_limit_mbps.load() > 1.0 ? tel.isp_up_limit_mbps.load() : 50.0;

            // These shapers are for global limits, individual IP shapers are in PacketConsumer
            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl * 0.85);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul * 0.85);

            // Core 2: WAN -> LAN (Downstream)
            // Core 2: WAN -> LAN (Downstream)
            worker_lan_wan = std::jthread(&App::worker_event_loop, this,
                std::move(iface_lan), fd_wan, 2, shaper_dl, nat_engine);

            // Core 3: LAN -> WAN (Upstream)
            worker_wan_lan = std::jthread(&App::worker_event_loop, this,
                std::move(iface_wan), fd_lan, 3, shaper_ul, nat_engine);

            shutdown_future = shutdown_promise.get_future();
            shutdown_future.wait();
            std::println("\n[System] Graceful shutdown complete.");
        }

        void stop() { shutdown_promise.set_value(); }

    private:
        void ui_render_loop(std::stop_token st) {
            Scalpel::System::set_thread_affinity(0);
            int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
            if (tfd == -1) return;

            struct itimerspec its{};
            its.it_value.tv_sec = 0;
            its.it_value.tv_nsec = 16666666;
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 16666666; 
            timerfd_settime(tfd, 0, &its, NULL);

            uint64_t expirations;
            while (!st.stop_requested()) {
                if (read(tfd, &expirations, sizeof(expirations)) > 0) {
                    // UI 采集聚合数据并渲染 (暂略)
                }
            }
            close(tfd);
        }

        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core, std::shared_ptr<Traffic::Shaper> shpr, std::shared_ptr<Logic::NatEngine> nat, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core);
            Scalpel::System::set_realtime_priority();

            PacketConsumer consumer(tx_fd, core, shpr, nat);

            while (!st.stop_requested()) {
                rx->poll_and_dispatch([&consumer](std::span<uint8_t> pkt) {
                    consumer.on_packet_event(pkt);
                }, 1);

                if (!Telemetry::instance().bridge_mode.load(std::memory_order_relaxed)) {
                    shpr->process_queue(tx_fd);
                }
                
                // 无锁无调用的心跳滴答 (Tick-based Heartbeat)：底层汇编的单周期单指令，0 系统调用！
                Telemetry::instance().core_metrics[core].last_heartbeat.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void watchdog_loop(std::stop_token st) {
            Scalpel::System::set_thread_affinity(1);
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

            while (!st.stop_requested()) {
                // 真正的内核级阻塞，不受系统负载抖动影响
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

                // 驱动 NAT 引擎滴答
                if (app->nat_engine) {
                    app->nat_engine->tick();
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







