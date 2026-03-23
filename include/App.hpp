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

    using RouteFunc = void (*)(const RouteContext& ctx, std::span<const uint8_t> pkt, size_t prio_idx, int core_id);

    static void fast_path_handler(const RouteContext& ctx, std::span<const uint8_t> pkt, size_t prio_idx, int core_id) {
        if (send(ctx.tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) < 0) {
            // 遵守 ISR "立即完成" 与绝对零阻塞原则：如果发送缓冲区满，则果断尾丢弃，绝不挂起或轮询重试
            Telemetry::instance().core_metrics[core_id].dropped[prio_idx].fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void shaper_handler(const RouteContext& ctx, std::span<const uint8_t> pkt, size_t /*prio_idx*/, int /*core_id*/) {
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
        int tx_fd;
        int core_id; // 增加核心 ID 标识
        Telemetry::BatchStats stats;
        Logic::HeuristicProcessor processor;
        
        RouteContext ctx;
        std::array<std::array<RouteFunc, 3>, 2> routes;
        StaticIpMap<std::shared_ptr<Traffic::Shaper>, 256> ip_shaper_map;

    public:
        PacketConsumer(int tx_fd, int cid, std::shared_ptr<Traffic::Shaper> global_shaper)
            : tx_fd(tx_fd), core_id(cid), ctx{tx_fd, global_shaper} {

            routes = {{
                { fast_path_handler, fast_path_handler, shaper_handler }, // 加速模式
                { fast_path_handler, fast_path_handler, fast_path_handler } // 透明网桥模式
            }};

            for (auto const& [ip, rate] : Config::IP_LIMIT_MAP) {
                auto s = std::make_shared<Traffic::Shaper>(rate);
                ip_shaper_map.insert(ip, s);
            }
        }

        void on_packet_event(std::span<const uint8_t> pkt) {
            // 实时路径：基于静态哈希表的 IP 匹配
            if (pkt.size() > 34) {
                // 判断方向并读取源/目 IP (简化处理)
                uint32_t target_ip = (core_id == 2) ? *reinterpret_cast<const uint32_t*>(&pkt[30]) 
                                                   : *reinterpret_cast<const uint32_t*>(&pkt[26]);
                auto target_shaper = ip_shaper_map.find(target_ip);
                if (target_shaper) {
                    RouteContext ip_ctx{tx_fd, target_shaper};
                    shaper_handler(ip_ctx, pkt, 2, core_id);
                    return;
                }
            }

            // 游戏加速逻辑
            auto prio = processor.process(pkt);
            const size_t p_idx = static_cast<size_t>(prio);

            stats.pkts++;
            stats.bytes += pkt.size();
            stats.prio_pkts[p_idx]++;
            stats.prio_bytes[p_idx] += pkt.size();

            size_t mode_idx = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
            routes[mode_idx][p_idx](ctx, pkt, p_idx, core_id);

            // 批量提交至当前 core 的专有槽位
            if (stats.pkts % 32 == 0) {
                Telemetry::instance().commit_batch(stats, core_id);
                stats.reset();
            }
        }
    };

    // 主框架：App 类
    class App {
        std::unique_ptr<Engine::RawSocketManager> eth0, eth1;
        HW::RGBLed led;
        std::promise<void> shutdown_promise;
        std::future<void> shutdown_future;

    public:
        std::expected<void, std::string> init() {
            Utils::Network::disable_hardware_offloads(Config::IFACE_WAN);
            Utils::Network::disable_hardware_offloads(Config::IFACE_LAN);

            eth0 = std::make_unique<Engine::RawSocketManager>(Config::IFACE_WAN);
            eth1 = std::make_unique<Engine::RawSocketManager>(Config::IFACE_LAN);

            if (auto r = eth0->init(); !r) return r;
            if (auto r = eth1->init(); !r) return r;

            return {};
        }

        void run() {
            std::println("=== Scalpel High-Performance Software Router ===");
            Telemetry::instance().bridge_mode.store(!Config::ENABLE_ACCELERATION, std::memory_order_relaxed);
            Scalpel::System::lock_cpu_frequency();

            std::jthread ui_thread([this](std::stop_token st) { ui_render_loop(st); });
            std::jthread monitor([this](std::stop_token st) { watchdog_loop(st); });

            std::string gw_ip = Utils::Network::get_gateway_ip();
            Utils::Network::force_arp_resolution(gw_ip);

            Probe::Manager::run_internal_stress();

            int fd0 = eth0->get_fd();
            int fd1 = eth1->get_fd();

            auto& tel = Telemetry::instance();
            double dl = tel.isp_down_limit_mbps.load() > 1.0 ? tel.isp_down_limit_mbps.load() : 500.0;
            double ul = tel.isp_up_limit_mbps.load() > 1.0 ? tel.isp_up_limit_mbps.load() : 50.0;

            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl * 0.85);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul * 0.85);

            // Core 2: WAN -> LAN (Downstream)
            std::jthread t1([this, rx = std::move(eth0), tx_fd = fd1, shpr = shaper_dl](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 2, shpr, st);
            });

            // Core 3: LAN -> WAN (Upstream)
            std::jthread t2([this, rx = std::move(eth1), tx_fd = fd0, shpr = shaper_ul](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 3, shpr, st);
            });

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

        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core, std::shared_ptr<Traffic::Shaper> shpr, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core);
            Scalpel::System::set_realtime_priority();

            PacketConsumer consumer(tx_fd, core, shpr);

            while (!st.stop_requested()) {
                rx->poll_and_dispatch([&consumer](std::span<const uint8_t> pkt) {
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





