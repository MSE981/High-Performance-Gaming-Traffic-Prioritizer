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

namespace Scalpel {

    // =========================================================================
    // 1. 独立解耦的路由处理器 
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
            while (true) {
                if (send(tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) break;

                int err = errno;
                if (err == EMSGSIZE || err == EINVAL || (err != ENOBUFS && err != EAGAIN)) break;

                if (--retries == 0) {
                    if (prio_idx == 0) Telemetry::instance().dropped_critical.fetch_add(1, std::memory_order_relaxed);
                    else Telemetry::instance().dropped_high.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
            }
        }
    };

    struct ShaperHandler final : public ITrafficHandler {
        std::shared_ptr<Traffic::Shaper> shaper;
        explicit ShaperHandler(std::shared_ptr<Traffic::Shaper> s) : shaper(std::move(s)) {}

        void handle(std::span<const uint8_t> pkt, size_t /*prio_idx*/) override {
            shaper->enqueue_normal(pkt);
        }
    };


    // =========================================================================
    // 2. 独立的事件消费者 
    // =========================================================================
    class PacketConsumer {
        int tx_fd;
        bool is_download;
        std::atomic<uint64_t>& heartbeat;

        Logic::HeuristicProcessor processor;
        Telemetry::BatchStats stats;
        std::array<std::array<std::shared_ptr<ITrafficHandler>, 3>, 2> active_routes;

    public:
        PacketConsumer(int tx_fd, bool is_down, std::shared_ptr<Traffic::Shaper> shaper, std::atomic<uint64_t>& hb)
            : tx_fd(tx_fd), is_download(is_down), heartbeat(hb) {

            auto fast_handler = std::make_shared<FastPathHandler>(tx_fd);
            auto shaper_handler = std::make_shared<ShaperHandler>(shaper);

            // 行 0: Shaping Mode (限速模式)
            // 行 1: Bridge Mode (透明网桥模式)
            active_routes = { {
                { fast_handler, fast_handler, shaper_handler },
                { fast_handler, fast_handler, fast_handler }
            } };
        }

        //将冗长的 Lambda 闭包剥离为独立的类方法！
        void on_packet_event(std::span<const uint8_t> pkt) {
            auto prio = processor.process(pkt);
            const auto p_idx = static_cast<size_t>(prio);

            stats.pkts++;
            stats.bytes += pkt.size();
            stats.prio_pkts[p_idx]++;
            stats.prio_bytes[p_idx] += pkt.size();

            size_t mode_idx = Telemetry::instance().bridge_mode.load(std::memory_order_relaxed);
            active_routes[mode_idx][p_idx]->handle(pkt, p_idx);

            if (stats.pkts % 32 == 0) {
                Telemetry::instance().commit_batch(stats, is_download);
                heartbeat.store(time(nullptr), std::memory_order_relaxed);
                stats.reset();
            }
        }
    };


    // =========================================================================
    // 3. 主应用程序，只负责管理生命周期与 Reactor 事件循环
    // =========================================================================
    class App {
        std::unique_ptr<Engine::RawSocketManager> eth0, eth1;
        HW::RGBLed led;
        std::promise<void> shutdown_promise;
        std::future<void> shutdown_future;

    public:
        std::expected<void, std::string> init() {
            std::println("[System] Disabling hardware offloads via C API on {}...", Config::IFACE_WAN);
            if (!Utils::Network::disable_hardware_offloads(Config::IFACE_WAN)) {
                std::println(stderr, "[Warning] IOCTL failed to disable GRO on {}. Ensure program has CAP_NET_ADMIN.", Config::IFACE_WAN);
            }

            std::println("[System] Disabling hardware offloads via C API on {}...", Config::IFACE_LAN);
            if (!Utils::Network::disable_hardware_offloads(Config::IFACE_LAN)) {
                std::println(stderr, "[Warning] IOCTL failed to disable GRO on {}. Ensure program has CAP_NET_ADMIN.", Config::IFACE_LAN);
            }

            eth0 = std::make_unique<Engine::RawSocketManager>(Config::IFACE_WAN);
            eth1 = std::make_unique<Engine::RawSocketManager>(Config::IFACE_LAN);

            if (auto r = eth0->init(); !r) return r;
            if (auto r = eth1->init(); !r) return r;

            return {};
        }

        void run() {
            std::println("=== GamingTrafficPrioritizer ===");

            Scalpel::System::lock_cpu_frequency();

            std::jthread monitor([this](std::stop_token st) { watchdog_loop(st); });

            std::string wan_name(Config::IFACE_WAN);
            std::string local_ip = Utils::Network::get_local_ip(wan_name);
            std::string gw_ip = Utils::Network::get_gateway_ip();

            std::string gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            if (gw_mac.empty() || gw_mac == "00:00:00:00:00:00") {
                Utils::Network::force_arp_resolution(gw_ip);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            }

            Probe::Manager::run_internal_stress();
            Probe::Manager::run_isp_probe(eth0->get_fd());

            int fd0 = eth0->get_fd();
            int fd1 = eth1->get_fd();

            auto& tel = Telemetry::instance();

            double dl_limit = tel.isp_down_limit_mbps.load();
            if (dl_limit < 1.0) dl_limit = 500.0;
            double ul_limit = tel.isp_up_limit_mbps.load();
            if (ul_limit < 1.0) ul_limit = 50.0;

            auto shaper_dl = std::make_shared<Traffic::Shaper>(dl_limit * 0.80);
            auto shaper_ul = std::make_shared<Traffic::Shaper>(ul_limit * 0.80);

            Probe::Manager::run_async_real_isp_probe([shaper_dl, shaper_ul](double dl, double ul) {
                Telemetry::instance().isp_down_limit_mbps.store(dl, std::memory_order_relaxed);
                Telemetry::instance().isp_up_limit_mbps.store(ul, std::memory_order_relaxed);

                shaper_dl->set_rate_limit(dl * 0.80);
                shaper_ul->set_rate_limit(ul * 0.80);
                std::println("\n[App] Bandwidth limit dynamically updated via Mode C Setter.");
                });

            std::jthread t1([this, rx = std::move(eth0), tx_fd = fd1, shpr = shaper_dl](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 2, true, shpr, Telemetry::instance().last_heartbeat_core2, st);
                });

            std::jthread t2([this, rx = std::move(eth1), tx_fd = fd0, shpr = shaper_ul](std::stop_token st) mutable {
                worker_event_loop(std::move(rx), tx_fd, 3, false, shpr, Telemetry::instance().last_heartbeat_core3, st);
                });

            shutdown_future = shutdown_promise.get_future();
            shutdown_future.wait();

            std::println("\n[System] Shutting down... Joining threads and releasing hardware resources.");
        }

        void stop() {
            shutdown_promise.set_value();
        }

    private:
        void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core_id, bool is_download, std::shared_ptr<Traffic::Shaper> shaper, auto& heartbeat, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core_id);
            Scalpel::System::set_realtime_priority();

            // 1. 初始化消费者
            PacketConsumer consumer(tx_fd, is_download, shaper, heartbeat);

            // 2. 绑定事件回调
            rx->registerCallback([&consumer](std::span<const uint8_t> pkt) {
                consumer.on_packet_event(pkt);
                });

            uint64_t idle_loops = 0;

            // 3. 底层事件驱动主循环
            while (!st.stop_requested()) {
                rx->poll_and_dispatch(1);      // 阻塞监听事件，硬件中断将主动 Push 触发 Callback
                shaper->process_queue(tx_fd);  // 定时器事件：抽空限速队列

                if (++idle_loops % 1000 == 0) {
                    heartbeat.store(time(nullptr), std::memory_order_relaxed);
                }
            }
        }

        void watchdog_loop(std::stop_token st) {
            auto& tel = Telemetry::instance();

            uint64_t last_pkts_down = 0, last_bytes_down = 0;
            uint64_t last_pkts_up = 0, last_bytes_up = 0;
            uint64_t last_crit = 0, last_high = 0, last_norm = 0;
            auto last_time = std::chrono::steady_clock::now();

            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (tel.is_probing) {
                    led.set_yellow();
                }

                auto now = std::chrono::steady_clock::now();
                uint64_t cur_pkts_down = tel.pkts_down.load(std::memory_order_relaxed);
                uint64_t cur_bytes_down = tel.bytes_down.load(std::memory_order_relaxed);
                uint64_t cur_pkts_up = tel.pkts_up.load(std::memory_order_relaxed);
                uint64_t cur_bytes_up = tel.bytes_up.load(std::memory_order_relaxed);

                uint64_t cur_crit = tel.pkts_critical.load(std::memory_order_relaxed);
                uint64_t cur_high = tel.pkts_high.load(std::memory_order_relaxed);
                uint64_t cur_norm = tel.pkts_normal.load(std::memory_order_relaxed);

                uint64_t drops_crit = tel.dropped_critical.load(std::memory_order_relaxed);
                uint64_t drops_high = tel.dropped_high.load(std::memory_order_relaxed);
                uint64_t drops_norm = tel.dropped_normal.load(std::memory_order_relaxed);

                double seconds = std::chrono::duration<double>(now - last_time).count();

                uint64_t pps_crit = static_cast<uint64_t>((cur_crit - last_crit) / seconds);
                uint64_t pps_high = static_cast<uint64_t>((cur_high - last_high) / seconds);
                uint64_t pps_norm = static_cast<uint64_t>((cur_norm - last_norm) / seconds);

                double mbps_down = ((cur_bytes_down - last_bytes_down) * 8.0 / 1e6) / seconds;
                double mbps_up = ((cur_bytes_up - last_bytes_up) * 8.0 / 1e6) / seconds;

                bool is_bridge = tel.bridge_mode.load(std::memory_order_relaxed);
                const char* mode_str = is_bridge ? "BRIDGE" : "SHAPER";

                std::print("\r Mode:[{:<6}] | Mbps[DL:{:5.1f} UL:{:5.1f}] | PPS[C:{:<4} H:{:<4} N:{:<5}] | Drp[C:{} H:{} N:{:<3}]  ",
                    mode_str, mbps_down, mbps_up, pps_crit, pps_high, pps_norm, drops_crit, drops_high, drops_norm);
                std::cout.flush();

                last_pkts_down = cur_pkts_down; last_bytes_down = cur_bytes_down;
                last_pkts_up = cur_pkts_up; last_bytes_up = cur_bytes_up;
                last_crit = cur_crit; last_high = cur_high; last_norm = cur_norm;
                last_time = now;

                if (!tel.is_probing) {
                    auto heartbeat_now = time(nullptr);
                    if (heartbeat_now - tel.last_heartbeat_core2 > 100 || heartbeat_now - tel.last_heartbeat_core3 > 100) {
                        led.set_red();
                        std::println(stderr, "\r Watchdog: Forwarding STALLED!");
                    }
                    else {
                        led.set_green();
                    }
                }
            }
        }
    };
}