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

    // =========================================================================
    // 1. 数据面：独立解耦的路由处理器 (ITrafficHandler)
    // =========================================================================
    struct ITrafficHandler {
        virtual void handle(std::span<const uint8_t> pkt, size_t prio_idx) = 0;
        virtual ~ITrafficHandler() = default;
    };

    struct FastPathHandler final : public ITrafficHandler {
        int tx_fd;
        explicit FastPathHandler(int fd) : tx_fd(fd) {}
        void handle(std::span<const uint8_t> pkt, size_t prio_idx) override {
            int retries = 3;
            while (retries--) {
                if (send(tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) return;
                if (errno != ENOBUFS && errno != EAGAIN) break;
                std::this_thread::yield();
            }
            if (prio_idx == 0) Telemetry::instance().dropped_critical.fetch_add(1, std::memory_order_relaxed);
            else Telemetry::instance().dropped_high.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct ShaperHandler final : public ITrafficHandler {
        std::shared_ptr<Traffic::Shaper> shaper;
        explicit ShaperHandler(std::shared_ptr<Traffic::Shaper> s) : shaper(std::move(s)) {}
        void handle(std::span<const uint8_t> pkt, size_t) override {
            shaper->enqueue_normal(pkt);
        }
    };

    // =========================================================================
    // 1.5 辅助工具：基于 PDF 推荐的 FNV-1a 静态哈希表 (针对嵌入式实时性能优化)
    // =========================================================================
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


    // =========================================================================
    // 2. 数据面：事件消费者模型 (PacketConsumer) - 处理核心逻辑
    // =========================================================================
    class PacketConsumer {
        int tx_fd;
        bool is_download;
        std::atomic<uint64_t>& heartbeat;

        Logic::HeuristicProcessor processor;
        Telemetry::BatchStats stats;
        
        std::array<std::array<std::shared_ptr<ITrafficHandler>, 3>, 2> routes;

        // 软路由：基于 FNV-1a 静态哈希表的独立限速器池 (零动态分配)
        StaticIpMap<std::shared_ptr<ITrafficHandler>, 256> ip_shaper_map;

    public:
        PacketConsumer(int tx_fd, bool is_down, std::shared_ptr<Traffic::Shaper> global_shaper, std::atomic<uint64_t>& hb)
            : tx_fd(tx_fd), is_download(is_down), heartbeat(hb) {

            auto fast = std::make_shared<FastPathHandler>(tx_fd);
            auto global_shpr = std::make_shared<ShaperHandler>(global_shaper);

            routes = {{
                { fast, fast, global_shpr }, // 加速模式
                { fast, fast, fast }         // 透明网桥模式
            }};

            // 预加载特定终端的限速处理器
            for (auto const& [ip, rate] : Config::IP_LIMIT_MAP) {
                auto s = std::make_shared<Traffic::Shaper>(rate);
                ip_shaper_map.insert(ip, std::make_shared<ShaperHandler>(s));
            }
        }

        void on_packet_event(std::span<const uint8_t> pkt) {
            // 实时路径：基于静态哈希表的 IP 匹配
            if (pkt.size() > 34) {
                uint32_t target_ip = is_download ? *reinterpret_cast<const uint32_t*>(&pkt[30]) 
                                                : *reinterpret_cast<const uint32_t*>(&pkt[26]);
                auto handler = ip_shaper_map.find(target_ip);
                if (handler) {
                    handler->handle(pkt, 2);
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
            routes[mode_idx][p_idx]->handle(pkt, p_idx);

            if (stats.pkts % 32 == 0) {
                Telemetry::instance().commit_batch(stats, is_download);
                heartbeat.store(time(nullptr), std::memory_order_relaxed);
                stats.reset();
            }
        }
    };


    // =========================================================================
    // 3. 主框架：App 类负责多核异构调度、生命周期与渲染面
    // =========================================================================
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
            Probe::Manager::run_isp_probe(eth0->get_fd());

            int fd0 = eth0->get_fd();
            int fd1 = eth1->get_fd();

            auto& tel = Telemetry::instance();
            double dl = tel.isp_down_limit_mbps.load() > 1.0 ? tel.isp_down_limit_mbps.load() : 500.0;
            double ul = tel.isp_up_limit_mbps.load() > 1.0 ? tel.isp_up_limit_mbps.load() : 50.0;

            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl * 0.85);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul * 0.85);

            std::jthread t1([this, rx = std::move(eth0), tx_fd = fd1, shpr = shaper_dl](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 2, true, shpr, Telemetry::instance().last_heartbeat_core2, st);
            });

            std::jthread t2([this, rx = std::move(eth1), tx_fd = fd0, shpr = shaper_ul](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 3, false, shpr, Telemetry::instance().last_heartbeat_core3, st);
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
            if (tfd == -1) {
                std::println(stderr, "[GUI] 错误: 无法创建 timerfd");
                return;
            }

            struct itimerspec its{};
            its.it_value.tv_sec = 0;
            its.it_value.tv_nsec = 16666666; // 16.6ms
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 16666666; 

            if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
                std::println(stderr, "[GUI] 错误: 无法设置 timerfd");
                close(tfd);
                return;
            }

            uint64_t expirations;
            while (!st.stop_requested()) {
                // 阻塞直到定时器到期 (VSync 同步)
                if (read(tfd, &expirations, sizeof(expirations)) > 0) {
                    // 此处将调用物理仿真解算器实现 iOS 25 风格动效
                }
            }
            close(tfd);
        }

        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core, bool down, std::shared_ptr<Traffic::Shaper> shpr, std::atomic<uint64_t>& hb, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core);
            Scalpel::System::set_realtime_priority();

            PacketConsumer consumer(tx_fd, down, shpr, hb);
            rx->registerCallback([&consumer](std::span<const uint8_t> pkt) {
                consumer.on_packet_event(pkt);
            });

            while (!st.stop_requested()) {
                rx->poll_and_dispatch(1);
                if (!Telemetry::instance().bridge_mode.load(std::memory_order_relaxed)) {
                    shpr->process_queue(tx_fd);
                }
            }
        }

        void watchdog_loop(std::stop_token st) {
            Scalpel::System::set_thread_affinity(1);
            auto& tel = Telemetry::instance();
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (time(nullptr) - tel.last_heartbeat_core2 > 100) led.set_red();
                else led.set_green();
            }
        }
    };
}


