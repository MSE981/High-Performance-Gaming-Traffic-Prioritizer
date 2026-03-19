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

    // 利用抽象接口类消除 if-else 路由分支
    struct ITrafficHandler {
        virtual void handle(std::span<const uint8_t> pkt, size_t prio_idx) = 0;
        virtual ~ITrafficHandler() = default;
    };

    // 极速通道处理器 (负责 Critical = 0 和 High = 1)
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

    // 限速排队处理器 (负责 Normal = 2)
    struct ShaperHandler final : public ITrafficHandler {
        std::shared_ptr<Traffic::Shaper> shaper;
        explicit ShaperHandler(std::shared_ptr<Traffic::Shaper> s) : shaper(std::move(s)) {}

        void handle(std::span<const uint8_t> pkt, size_t /*prio_idx*/) override {
            shaper->enqueue_normal(pkt);
        }
    };

    class App {
        // 核心优化：改用 unique_ptr。RX 对象在线程间互不共享，完全消除内存竞争。
        std::unique_ptr<Engine::RawSocketManager> eth0, eth1;
        HW::RGBLed led;

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

            std::jthread t1([this, rx = std::move(eth0), tx_fd = fd1](std::stop_token st) mutable {
                worker(std::move(rx), tx_fd, 2, true, Telemetry::instance().last_heartbeat_core2, st);
                });

            std::jthread t2([this, rx = std::move(eth1), tx_fd = fd0](std::stop_token st) mutable {
                worker(std::move(rx), tx_fd, 3, false, Telemetry::instance().last_heartbeat_core3, st);
                });

            shutdown_future = shutdown_promise.get_future();
            shutdown_future.wait();

            std::println("\n[System] Shutting down... Joining threads and releasing hardware resources.");
        }

        void stop() {
            shutdown_promise.set_value();
        }

    private:
        std::promise<void> shutdown_promise;
        std::future<void> shutdown_future;

        void worker(std::unique_ptr<Engine::RawSocketManager> rx, int tx_fd, int core_id, bool is_download, auto& heartbeat, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core_id);
            Scalpel::System::set_realtime_priority();

            Logic::HeuristicProcessor processor;

            double limit_mbps = is_download ? Telemetry::instance().isp_down_limit_mbps.load() : Telemetry::instance().isp_up_limit_mbps.load();
            if (limit_mbps < 1.0) limit_mbps = is_download ? 500.0 : 50.0;
            auto shaper = std::make_shared<Traffic::Shaper>(limit_mbps * 0.80);

            Probe::Manager::run_async_real_isp_probe([shaper, is_download](double dl, double ul) {
                if (is_download) {
                    Telemetry::instance().isp_down_limit_mbps.store(dl, std::memory_order_relaxed);
                    shaper->set_rate_limit(dl * 0.80);
                }
                else {
                    Telemetry::instance().isp_up_limit_mbps.store(ul, std::memory_order_relaxed);
                    shaper->set_rate_limit(ul * 0.80);
                }
                std::println("\n[App] Bandwidth limit dynamically updated via Mode C Setter.");
                });

            Telemetry::BatchStats stats;

            auto fast_handler = std::make_shared<FastPathHandler>(tx_fd);
            auto shaper_handler = std::make_shared<ShaperHandler>(shaper);

            std::array<std::shared_ptr<ITrafficHandler>, 3> routing_table = {
                fast_handler,
                fast_handler,
                shaper_handler
            };

            // 基于回调的类间事件传递
            rx->registerCallback([&, routing_table](std::span<const uint8_t> pkt) {
                auto prio = processor.process(pkt);
                const auto p_idx = static_cast<size_t>(prio);

                stats.pkts++;
                stats.bytes += pkt.size();
                stats.prio_pkts[p_idx]++;
                stats.prio_bytes[p_idx] += pkt.size();

                // O(1) 多态查表直接分发，彻底粉碎 Massive Loop！
                routing_table[p_idx]->handle(pkt, p_idx);

                if (stats.pkts % 32 == 0) {
                    Telemetry::instance().commit_batch(stats, is_download);
                    heartbeat.store(time(nullptr), std::memory_order_relaxed);
                    stats.reset();
                }
                });

            uint64_t idle_loops = 0;

            // 由底层事件驱动的主循环
            while (!st.stop_requested()) {
                rx->poll_and_dispatch(1);      // 阻塞监听事件，触发 Callback
                shaper->process_queue(tx_fd);  // 定时抽空限速队列

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

                std::print("\r Mbps[DL:{:5.1f} UL:{:5.1f}] | PPS[C:{:<4} H:{:<4} N:{:<5}] | Drp[C:{} H:{} N:{:<3}]  ",
                    mbps_down, mbps_up, pps_crit, pps_high, pps_norm, drops_crit, drops_high, drops_norm);
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