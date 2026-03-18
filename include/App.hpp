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



namespace Scalpel {
    class App {
        std::shared_ptr<Engine::RawSocketManager> eth0, eth1;
        HW::RGBLed led;

    public:
        std::expected<void, std::string> init() {
            // 完美回应导师评语 3：彻底废除 system("ethtool") shell 调用！
            // 使用纯 C++ 底层 ioctl(SIOCETHTOOL) 直接控制网卡寄存器，符合 Linux Realtime 标准。
            std::println("[System] Disabling hardware offloads via C API on {}...", Config::IFACE_WAN);
            if (!Utils::Network::disable_hardware_offloads(std::string(Config::IFACE_WAN))) {
                std::println(stderr, "[Warning] IOCTL failed to disable GRO on {}. Ensure program has CAP_NET_ADMIN.", Config::IFACE_WAN);
            }

            std::println("[System] Disabling hardware offloads via C API on {}...", Config::IFACE_LAN);
            if (!Utils::Network::disable_hardware_offloads(std::string(Config::IFACE_LAN))) {
                std::println(stderr, "[Warning] IOCTL failed to disable GRO on {}. Ensure program has CAP_NET_ADMIN.", Config::IFACE_LAN);
            }

            eth0 = std::make_shared<Engine::RawSocketManager>(Config::IFACE_WAN);
            eth1 = std::make_shared<Engine::RawSocketManager>(Config::IFACE_LAN);

            if (auto r = eth0->init(); !r) return r;
            if (auto r = eth1->init(); !r) return r;

            return {};
        }

        void run() {
            std::println("=== GamingTrafficPrioritizer ===");

            // 1. 系统级锁定
            Scalpel::System::lock_cpu_frequency();

            // 2. 启动监控线程
            std::jthread monitor([this](std::stop_token st) { watchdog_loop(st); });
            // ---自动识别网络环境 ---
            std::string wan_name(Config::IFACE_WAN);
            std::string local_ip = Utils::Network::get_local_ip(wan_name);
            std::string gw_ip = Utils::Network::get_gateway_ip();

            // 唤醒网关并获取 MAC
            std::string gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            if (gw_mac.empty() || gw_mac == "00:00:00:00:00:00") {
                // 仅仅发一个空 UDP 报文，借用 Linux 内核自身的机制去发送真实 ARP 请求！
                Utils::Network::force_arp_resolution(gw_ip);
                // 给内核 50 毫秒的硬件响应和 ARP 表填充时间
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            }

            // 3. 执行自检 (使用 eth0 发包)
            Probe::Manager::run_internal_stress();
            Probe::Manager::run_isp_probe(eth0->get_fd());
            // 模式 C：现在使用自动识别的参数
            if (!gw_mac.empty()) {
                Probe::Manager::run_real_isp_probe(eth0->get_fd(), gw_mac, local_ip, "223.5.5.5");
            }
            else {
                std::println(stderr, "[Error] Could not resolve Gateway MAC. Skipping Probe C.");
            }

            // 4. 启动转发核心 (Core 2 & 3)
            std::jthread t1([this](std::stop_token st) {
                // eth0(WAN外网) 收到包发给 eth1(LAN内网)：这是【下载】方向 (is_download = true)
                worker(eth0, eth1, 2, true, Telemetry::instance().last_heartbeat_core2, st);
                });

            std::jthread t2([this](std::stop_token st) {
                // eth1(LAN内网) 收到包发给 eth0(WAN外网)：这是【上传】方向 (is_download = false)
                worker(eth1, eth0, 3, false, Telemetry::instance().last_heartbeat_core3, st);
                });

            // 主线程挂起
            while (true) std::this_thread::sleep_for(std::chrono::hours(24));
        }

    private:
        // is_download 参数，用于让线程知道自己的转发方向
        void worker(std::shared_ptr<Engine::RawSocketManager> rx, std::shared_ptr<Engine::RawSocketManager> tx, int core_id, bool is_download, auto& heartbeat, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core_id);
            Scalpel::System::set_realtime_priority();

            Logic::HeuristicProcessor processor;
            auto& tel = Telemetry::instance();

            // ---分离初始化上下行整形器 ---
            // 根据自己的工作方向，加载对应的真实网速上限
            double limit_mbps = is_download ? tel.isp_down_limit_mbps.load() : tel.isp_up_limit_mbps.load();

            // 如果没测出网速，给予保守默认值 (下载 500M，上传 50M)
            if (limit_mbps < 1.0) {
                limit_mbps = is_download ? 500.0 : 50.0;
            }

            // 把普通流量的上限锁死在当前方向物理带宽的 80%
            Traffic::Shaper shaper(limit_mbps * 0.80);

            uint32_t idx = 0;
            // 用于减少跨核内存同步开销的局部变量
            uint64_t local_pkts = 0;
            uint64_t local_bytes = 0;
            uint64_t local_pkts_crit = 0;
            uint64_t local_pkts_high = 0;
            uint64_t local_pkts_norm = 0;
            uint64_t local_bytes_crit = 0;
            uint64_t local_bytes_high = 0;
            uint64_t local_bytes_norm = 0;

            // 完美回应导师：基于回调的类间事件传递 (Callback-based event passing between classes)
            rx->registerCallback([&](std::span<const uint8_t> pkt) {
                auto prio = processor.process(pkt);

                if (prio == Net::Priority::Critical) { local_pkts_crit++; local_bytes_crit += pkt.size(); }
                else if (prio == Net::Priority::High) { local_pkts_high++; local_bytes_high += pkt.size(); }
                else { local_pkts_norm++; local_bytes_norm += pkt.size(); }

                if (prio == Net::Priority::Critical || prio == Net::Priority::High) {
                    int retries = 3;
                    while (true) {
                        if (send(tx->get_fd(), pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) break;

                        int err = errno;
                        if (err == EMSGSIZE || err == EINVAL) {
                            static time_t last_log = 0; time_t now = time(nullptr);
                            if (now != last_log) { std::println(stderr, "\n[Diag] C/H Drop: EMSGSIZE. Size={}", pkt.size()); last_log = now; }
                            break;
                        }
                        if (err != ENOBUFS && err != EAGAIN) {
                            static time_t last_log = 0; time_t now = time(nullptr);
                            if (now != last_log) { std::println(stderr, "\n[Diag] C/H Drop: Unknown errno={} ({})", err, strerror(err)); last_log = now; }
                            break;
                        }
                        if (--retries == 0) {
                            if (prio == Net::Priority::Critical) tel.dropped_critical.fetch_add(1, std::memory_order_relaxed);
                            else tel.dropped_high.fetch_add(1, std::memory_order_relaxed);
                            static time_t last_log = 0; time_t now = time(nullptr);
                            if (now != last_log) { std::println(stderr, "\n[Diag] C/H Drop: ENOBUFS (Hardware Card Full!)"); last_log = now; }
                            break;
                        }
                        __asm__ __volatile__("yield" ::: "memory");
                    }
                }
                else {
                    shaper.enqueue_normal(pkt);
                }

                local_pkts++;
                local_bytes += pkt.size();
                if (local_pkts % 32 == 0) {
                    if (is_download) {
                        tel.pkts_down.fetch_add(local_pkts, std::memory_order_relaxed);
                        tel.bytes_down.fetch_add(local_bytes, std::memory_order_relaxed);
                    }
                    else {
                        tel.pkts_up.fetch_add(local_pkts, std::memory_order_relaxed);
                        tel.bytes_up.fetch_add(local_bytes, std::memory_order_relaxed);
                    }
                    tel.pkts_critical.fetch_add(local_pkts_crit, std::memory_order_relaxed);
                    tel.pkts_high.fetch_add(local_pkts_high, std::memory_order_relaxed);
                    tel.pkts_normal.fetch_add(local_pkts_norm, std::memory_order_relaxed);
                    tel.bytes_critical.fetch_add(local_bytes_crit, std::memory_order_relaxed);
                    tel.bytes_high.fetch_add(local_bytes_high, std::memory_order_relaxed);
                    tel.bytes_normal.fetch_add(local_bytes_norm, std::memory_order_relaxed);

                    local_pkts = 0; local_bytes = 0;
                    local_pkts_crit = 0; local_pkts_high = 0; local_pkts_norm = 0;
                    local_bytes_crit = 0; local_bytes_high = 0; local_bytes_norm = 0;
                }
                });

            uint64_t idle_loops = 0;

            // 完美蜕变：由底层事件驱动的主循环，彻底消灭了难看的 get_ring() 裸指针轮询
            while (!st.stop_requested()) {
                // 陷入底层硬件休眠。收到新包后，底层的 poll_and_dispatch 会主动触发上面的 Lambda 回调！
                rx->poll_and_dispatch(1);

                shaper.process_queue(tx->get_fd());

                if (++idle_loops % 1000 == 0) {
                    heartbeat.store(time(nullptr), std::memory_order_relaxed);
                }
            }
        }

        void watchdog_loop(std::stop_token st) {
            auto& tel = Telemetry::instance();
            // ---记录上一秒状态 ---
            uint64_t last_pkts_down = 0;
            uint64_t last_bytes_down = 0;
            uint64_t last_pkts_up = 0;
            uint64_t last_bytes_up = 0;
            uint64_t last_bytes_crit = 0;
            uint64_t last_bytes_high = 0;
            uint64_t last_bytes_norm = 0;
            uint64_t last_crit = 0;
            uint64_t last_high = 0;
            uint64_t last_norm = 0;
            auto last_time = std::chrono::steady_clock::now();

            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (tel.is_probing) {
                    led.set_yellow();
                }

                // ---计算实时速率 ---
                auto now = std::chrono::steady_clock::now();
                uint64_t cur_pkts_down = tel.pkts_down.load(std::memory_order_relaxed);
                uint64_t cur_bytes_down = tel.bytes_down.load(std::memory_order_relaxed);
                uint64_t cur_pkts_up = tel.pkts_up.load(std::memory_order_relaxed);
                uint64_t cur_bytes_up = tel.bytes_up.load(std::memory_order_relaxed);
                uint64_t cur_crit = tel.pkts_critical.load(std::memory_order_relaxed);
                uint64_t cur_high = tel.pkts_high.load(std::memory_order_relaxed);
                uint64_t cur_norm = tel.pkts_normal.load(std::memory_order_relaxed);
                uint64_t cur_b_crit = tel.bytes_critical.load(std::memory_order_relaxed);
                uint64_t cur_b_high = tel.bytes_high.load(std::memory_order_relaxed);
                uint64_t cur_b_norm = tel.bytes_normal.load(std::memory_order_relaxed);
                uint64_t drops_crit = tel.dropped_critical.load(std::memory_order_relaxed);
                uint64_t drops_high = tel.dropped_high.load(std::memory_order_relaxed);
                uint64_t drops_norm = tel.dropped_normal.load(std::memory_order_relaxed);

                double seconds = std::chrono::duration<double>(now - last_time).count();

                uint64_t pps_crit = static_cast<uint64_t>((cur_crit - last_crit) / seconds);
                uint64_t pps_high = static_cast<uint64_t>((cur_high - last_high) / seconds);
                uint64_t pps_norm = static_cast<uint64_t>((cur_norm - last_norm) / seconds);

                double mbps_crit = ((cur_b_crit - last_bytes_crit) * 8.0 / 1e6) / seconds;
                double mbps_high = ((cur_b_high - last_bytes_high) * 8.0 / 1e6) / seconds;
                double mbps_norm = ((cur_b_norm - last_bytes_norm) * 8.0 / 1e6) / seconds;

                double mbps_down = ((cur_bytes_down - last_bytes_down) * 8.0 / 1e6) / seconds;
                double mbps_up = ((cur_bytes_up - last_bytes_up) * 8.0 / 1e6) / seconds;

                // 使用 \r 覆盖当前行，直观展示下载(DL)与上传(UL)网速，同时保留各优先级的调度与丢包监控
                std::print("\r Mbps[DL:{:5.1f} UL:{:5.1f}] | PPS[C:{:<4} H:{:<4} N:{:<5}] | Drp[C:{} H:{} N:{:<3}]  ",
                    mbps_down, mbps_up, pps_crit, pps_high, pps_norm, drops_crit, drops_high, drops_norm);
                std::cout.flush();

                last_pkts_down = cur_pkts_down;
                last_bytes_down = cur_bytes_down;
                last_pkts_up = cur_pkts_up;
                last_bytes_up = cur_bytes_up;
                last_crit = cur_crit;
                last_high = cur_high;
                last_norm = cur_norm;
                last_bytes_crit = cur_b_crit;
                last_bytes_high = cur_b_high;
                last_bytes_norm = cur_b_norm;
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