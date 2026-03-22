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
    // 2. 数据面：事件消费者模型 (PacketConsumer) - 处理核心逻辑
    // =========================================================================
    class PacketConsumer {
        int tx_fd;
        bool is_download;
        std::atomic<uint64_t>& heartbeat;

        Logic::HeuristicProcessor processor;
        Telemetry::BatchStats stats;
        
        // 路由通路映射：[工作模式][优先级索引]
        // 工作模式 0 = 加速 (Shaping), 1 = 通透 (Bridge)
        std::array<std::array<std::shared_ptr<ITrafficHandler>, 3>, 2> routes;

        // 软路由：基于 IP 的独立限速处理器
        std::map<uint32_t, std::shared_ptr<ITrafficHandler>> ip_shapers;

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
                ip_shapers[ip] = std::make_shared<ShaperHandler>(s);
            }
        }

        void on_packet_event(std::span<const uint8_t> pkt) {
            // 软路由：检查是否存在 IP 特定限速规则 (IPv4 Offset 26/30)
            if (pkt.size() > 34) {
                uint32_t target_ip = is_download ? *reinterpret_cast<const uint32_t*>(&pkt[30]) 
                                                : *reinterpret_cast<const uint32_t*>(&pkt[26]);
                auto it = ip_shapers.find(target_ip);
                if (it != ip_shapers.end()) {
                    it->second->handle(pkt, 2); // 强制进入限速队列
                    return;
                }
            }

            // 游戏加速处理逻辑
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
            // 1. 关闭网卡硬件分载，确保 Raw Socket 获得原始包 (Core 2/3)
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
            
            // 同步配置文件中的运行模式
            Telemetry::instance().bridge_mode.store(!Config::ENABLE_ACCELERATION, std::memory_order_relaxed);

            Scalpel::System::lock_cpu_frequency();

            // Core 0: 独占渲染面 (Graphics Plane) - 未来接入 DRM/KMS
            std::jthread ui_thread([this](std::stop_token st) { 
                ui_render_loop(st); 
            });

            // Core 1: 控制面 (Control Plane) & 监控
            std::jthread monitor([this](std::stop_token st) { 
                watchdog_loop(st); 
            });

            // 获取基础网络信息
            std::string gw_ip = Utils::Network::get_gateway_ip();
            Utils::Network::force_arp_resolution(gw_ip);

            // 网络探测与压力预演
            Probe::Manager::run_internal_stress();
            Probe::Manager::run_isp_probe(eth0->get_fd());

            int fd0 = eth0->get_fd();
            int fd1 = eth1->get_fd();

            // 数据面逻辑初始化
            auto& tel = Telemetry::instance();
            double dl = tel.isp_down_limit_mbps.load() > 1.0 ? tel.isp_down_limit_mbps.load() : 500.0;
            double ul = tel.isp_up_limit_mbps.load() > 1.0 ? tel.isp_up_limit_mbps.load() : 50.0;

            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl * 0.85);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul * 0.85);

            // Core 2: 转发数据面 A (WAN -> LAN)
            std::jthread t1([this, rx = std::move(eth0), tx_fd = fd1, shpr = shaper_dl](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 2, true, shpr, Telemetry::instance().last_heartbeat_core2, st);
            });

            // Core 3: 转发数据面 B (LAN -> WAN)
            std::jthread t2([this, rx = std::move(eth1), tx_fd = fd0, shpr = shaper_ul](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 3, false, shpr, Telemetry::instance().last_heartbeat_core3, st);
            });

            shutdown_future = shutdown_promise.get_future();
            shutdown_future.wait();
            std::println("\n[System] Graceful shutdown complete.");
        }

        void stop() { shutdown_promise.set_value(); }

    private:
        // Core 0 渲染循环占位符
        void ui_render_loop(std::stop_token st) {
            Scalpel::System::set_thread_affinity(0);
            while (!st.stop_requested()) {
                // 16.6ms 物理步进计算
                std::this_thread::sleep_for(std::chrono::microseconds(16666)); 
            }
        }

        // 数据平面主循环 (Core 2/3)
        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core, bool down, std::shared_ptr<Traffic::Shaper> shpr, auto& hb, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core);
            Scalpel::System::set_realtime_priority();

            PacketConsumer consumer(tx_fd, down, shpr, hb);
            rx->registerCallback([&consumer](std::span<const uint8_t> pkt) {
                consumer.on_packet_event(pkt);
            });

            while (!st.stop_requested()) {
                rx->poll_and_dispatch(1);
                // 非桥接模式下处理限速队列
                if (!Telemetry::instance().bridge_mode.load(std::memory_order_relaxed)) {
                    shpr->process_queue(tx_fd);
                }
            }
        }

        // 控制面监控循环 (Core 1)
        void watchdog_loop(std::stop_token st) {
            Scalpel::System::set_thread_affinity(1);
            auto& tel = Telemetry::instance();
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                // 打印状态统计 (略...)
                if (time(nullptr) - tel.last_heartbeat_core2 > 100) led.set_red();
                else led.set_green();
            }
        }
    };
}
