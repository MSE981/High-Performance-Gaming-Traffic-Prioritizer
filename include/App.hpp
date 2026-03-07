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

namespace Scalpel {
    class App {
        std::shared_ptr<Engine::RawSocketManager> eth0, eth1;
        HW::RGBLed led;

    public:
        std::expected<void, std::string> init() {
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
                // 发送一个 ping 包强制刷新内核 ARP 表
                int ret = system(("ping -c 1 -W 1 " + gw_ip + " > /dev/null 2>&1").c_str());
                (void)ret;
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
                worker(eth0, eth1, 2, Telemetry::instance().last_heartbeat_core2, st);
                });

            std::jthread t2([this](std::stop_token st) {
                worker(eth1, eth0, 3, Telemetry::instance().last_heartbeat_core3, st);
                });

            // 主线程挂起
            while (true) std::this_thread::sleep_for(std::chrono::hours(24));
        }

    private:
        void worker(std::shared_ptr<Engine::RawSocketManager> rx, std::shared_ptr<Engine::RawSocketManager> tx, int core_id, auto& heartbeat, std::stop_token st) {
            Scalpel::System::set_thread_affinity(core_id);
            Scalpel::System::set_realtime_priority();

            Logic::HeuristicProcessor processor;
            auto& tel = Telemetry::instance();

            // ---初始化整形器 ---
            double current_isp_limit = tel.isp_limit_mbps.load();
            if (current_isp_limit < 10.0) current_isp_limit = 500.0; // 默认值

            // 把普通流量的上限锁死在物理带宽的 80%
            Traffic::Shaper shaper(current_isp_limit * 0.80);

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

            while (!st.stop_requested()) {
                auto* hdr = reinterpret_cast<tpacket_hdr*>(rx->get_ring() + (idx * rx->frame_size()));

                if (hdr->tp_status & TP_STATUS_USER) {
                    std::span pkt{ reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len };
                    auto prio = processor.process(pkt);

                    // --- 记录各级别流量状态 ---
                    if (prio == Net::Priority::Critical) { local_pkts_crit++; local_bytes_crit += pkt.size(); }
                    else if (prio == Net::Priority::High) { local_pkts_high++; local_bytes_high += pkt.size(); }
                    else { local_pkts_norm++; local_bytes_norm += pkt.size(); }

                    // ---三级调度分流逻辑 ---
                    if (prio == Net::Priority::Critical || prio == Net::Priority::High) {
                        // 游戏包/DNS：直接走零拷贝通道
                        int retries = 3; // 允许微秒级重试 3 次以吸收瞬间硬件拥塞
                        while (send(tx->get_fd(), pkt.data(), pkt.size(), MSG_DONTWAIT) < 0) {
                            if (--retries == 0) {
                                // 重试 3 次依然塞不进网卡，判定为真实物理丢包
                                if (prio == Net::Priority::Critical) tel.dropped_critical.fetch_add(1, std::memory_order_relaxed);
                                else tel.dropped_high.fetch_add(1, std::memory_order_relaxed);
                                break;
                            }
                            // 极短暂停顿，让出一点点 CPU 周期给网卡驱动去发包清空队列
                            __asm__ __volatile__("yield" ::: "memory");
                        }
                    }
                    else {
                        // 下载包：扔进 4MB 的内存池里排队等候发落
                        shaper.enqueue_normal(pkt);
                    }

                    // --- 每 32 个包才更新一次全局原子变量 ---
                    local_pkts++;
                    local_bytes += pkt.size();
                    if (local_pkts % 32 == 0) {
                        tel.pkts_forwarded.fetch_add(local_pkts, std::memory_order_relaxed);
                        tel.bytes_forwarded.fetch_add(local_bytes, std::memory_order_relaxed);
                        tel.pkts_critical.fetch_add(local_pkts_crit, std::memory_order_relaxed);
                        tel.pkts_high.fetch_add(local_pkts_high, std::memory_order_relaxed);
                        tel.pkts_normal.fetch_add(local_pkts_norm, std::memory_order_relaxed);
                        tel.bytes_critical.fetch_add(local_bytes_crit, std::memory_order_relaxed);
                        tel.bytes_high.fetch_add(local_bytes_high, std::memory_order_relaxed);
                        tel.bytes_normal.fetch_add(local_bytes_norm, std::memory_order_relaxed);
                        heartbeat.store(time(nullptr), std::memory_order_relaxed);

                        local_pkts = 0;
                        local_bytes = 0;
                        local_pkts_crit = 0;
                        local_pkts_high = 0;
                        local_pkts_norm = 0;
                        local_bytes_crit = 0;
                        local_bytes_high = 0;
                        local_bytes_norm = 0;
                    }

                    hdr->tp_status = TP_STATUS_KERNEL;
                    idx = (idx + 1) % rx->frame_nr();
                }
                else {
                    __asm__ __volatile__("yield" ::: "memory");
                }

                // ---不断抽空下载包队列 ---
                shaper.process_queue(tx->get_fd());
            }
        }

        void watchdog_loop(std::stop_token st) {
            auto& tel = Telemetry::instance();
            // ---记录上一秒状态 ---
            uint64_t last_pkts = 0;
            uint64_t last_bytes = 0;
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
                uint64_t cur_pkts = tel.pkts_forwarded.load(std::memory_order_relaxed);
                uint64_t cur_bytes = tel.bytes_forwarded.load(std::memory_order_relaxed);
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

                // 使用 \r 覆盖当前行，实现动态刷新，全方位展示各个级别的 Mbps、PPS 和 Drop
                std::print("\r Mbps[C:{:4.1f} H:{:4.1f} N:{:5.1f}] | PPS[C:{:<4} H:{:<4} N:{:<5}] | Drp[C:{} H:{} N:{:<3}]  ",
                    mbps_crit, mbps_high, mbps_norm, pps_crit, pps_high, pps_norm, drops_crit, drops_high, drops_norm);
                std::cout.flush();

                last_pkts = cur_pkts;
                last_bytes = cur_bytes;
                last_crit = cur_crit;
                last_high = cur_high;
                last_norm = cur_norm;
                last_bytes_crit = cur_b_crit;
                last_bytes_high = cur_b_high;
                last_bytes_norm = cur_b_norm;
                last_time = now;

                if (!tel.is_probing) {
                    auto heartbeat_now = time(nullptr);
                    if (heartbeat_now - tel.last_heartbeat_core2 > 5 || heartbeat_now - tel.last_heartbeat_core3 > 5) {
                        led.set_red();
                        std::println(stderr, "\nWatchdog: Forwarding STALLED!");
                    }
                    else {
                        led.set_green();
                    }
                }
            }
        }
    };
}