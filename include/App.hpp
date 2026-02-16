#pragma once
#include <thread>      // std::jthread
#include <stop_token>  // std::stop_token
#include <memory>      // std::shared_ptr, std::unique_ptr
#include <expected>    // std::expected
#include <chrono>      // 24h, 100ms
#include <ctime>       // time()
#include <sys/socket.h> // send() 用于工作循环
#include "NetworkUtils.hpp"
#include "NetworkEngine.hpp"
#include "Processor.hpp"
#include "SystemOptimizer.hpp"
#include "Telemetry.hpp"
#include "ProbeManager.hpp"
#include "Indicator.hpp"

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
            std::println("=== GamingTrafficPrioritizer V2.2 ===");

            // 1. 系统级锁定
            System::lock_cpu_frequency();

            // 2. 启动监控线程
            std::jthread monitor([this](std::stop_token st) { watchdog_loop(st); });
            // 2.1 自动识别 WAN 口环境参数
            std::string wan_name(Config::IFACE_WAN);
            std::string local_ip = Utils::Network::get_local_ip(wan_name);
            std::string gw_ip = Utils::Network::get_gateway_ip();

            // 技巧：为了确保 ARP 表里有网关 MAC，先执行一次微型探测包
            std::println("[Config] Detected Local IP: {}", local_ip);
            std::println("[Config] Detected Gateway: {}", gw_ip);

            // 获取网关 MAC
            std::string gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            if (gw_mac.empty() || gw_mac == "00:00:00:00:00:00") {
                std::println("[Config] ARP cache empty, attempting to wake up gateway...");
                // 简单发送一个 UDP 包给网关，诱导内核进行 ARP 寻址
                system(("ping -c 1 -W 1 " + gw_ip + " > /dev/null").c_str());
                gw_mac = Utils::Network::get_mac_from_arp(gw_ip);
            }
            std::println("[Config] Resolved Gateway MAC: {}", gw_mac);

            // 3. 执行自检 (使用 eth0 发包)
            Probe::Manager::run_internal_stress();
            Probe::Manager::run_isp_probe(eth0->get_fd());
            // 模式 C：现在使用自动识别的参数
            if (!gw_mac.empty()) {
                Probe::Manager::run_real_isp_probe(eth0->get_fd(), gw_mac, local_ip);
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
        void worker(std::shared_ptr<Engine::RawSocketManager> rx,
            std::shared_ptr<Engine::RawSocketManager> tx,
            int core_id, std::atomic<uint64_t>& heartbeat,
            std::stop_token st) {

            System::set_thread_affinity(core_id);
            System::set_realtime_priority();

            // 关键：每个线程拥有独立的处理器，无锁设计
            Logic::HeuristicProcessor processor;
            auto& tel = Telemetry::instance();

            uint32_t idx = 0;
            while (!st.stop_requested()) {
                auto* hdr = reinterpret_cast<tpacket_hdr*>(rx->get_ring() + (idx * rx->frame_size()));

                if (hdr->tp_status & TP_STATUS_USER) {
                    std::span pkt{ reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len };

                    // 启发式决策
                    auto prio = processor.process(pkt);

                    // 转发 (V3.0 将在此处加入令牌桶逻辑)
                    // MSG_DONTWAIT 防止阻塞
                    send(tx->get_fd(), pkt.data(), pkt.size(), MSG_DONTWAIT);

                    // 更新统计
                    tel.pkts_forwarded.fetch_add(1, std::memory_order_relaxed);
                    heartbeat.store(time(nullptr), std::memory_order_relaxed);

                    // 归还 Frame 给内核
                    hdr->tp_status = TP_STATUS_KERNEL;
                    idx = (idx + 1) % rx->frame_nr();
                }
                else {
                    __asm__ __volatile__("yield" ::: "memory");
                }
            }
        }

        void watchdog_loop(std::stop_token st) {
            auto& tel = Telemetry::instance();
            while (!st.stop_requested()) {
                if (tel.is_probing) {
                    led.set_yellow();
                }
                else {
                    auto now = time(nullptr);
                    if (now - tel.last_heartbeat_core2 > 5 || now - tel.last_heartbeat_core3 > 5) {
                        led.set_red();
                        std::println(stderr, "Watchdog: Forwarding STALLED!");
                    }
                    else {
                        led.set_green();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    };
}