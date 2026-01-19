#pragma once
#include <atomic>
#include <chrono>
#include <thread>
#include <print>
#include <fstream>
#include "NetworkEngine.hpp"
#include "Processor.hpp"

class NetworkScalpelApp {
    struct Stats {
        std::atomic<std::uint64_t> forwarded{0};
        std::atomic<std::uint64_t> heartbeat{0};
    };

public:
    std::expected<void, std::string> init() {
        eth0 = std::make_shared<RawSocketManager>("eth0");
        eth1 = std::make_shared<RawSocketManager>("eth1");
        if (auto r = eth0->open(); !r) return r;
        if (auto r = eth1->open(); !r) return r;
        
        processor = std::make_shared<PassThroughProcessor>();
        return {};
    }

    void run() {
        std::print("Core Engine Online. Starting Threads...\n");

        // 线程 1: eth0 -> eth1 (Core 2)
        std::jthread t1([this](std::stop_token st) { worker(eth0, eth1, stats_0, 2, st); });
        // 线程 2: eth1 -> eth0 (Core 3)
        std::jthread t2([this](std::stop_token st) { worker(eth1, eth0, stats_1, 3, st); });
        
        // 指示灯与看门狗线程 (Core 0)
        std::jthread monitor([this](std::stop_token st) { watchdog_and_led(st); });

        while (true) std::this_thread::sleep_for(std::chrono::hours(24));
    }

private:
    std::shared_ptr<RawSocketManager> eth0, eth1;
    std::shared_ptr<IPacketProcessor> processor;
    Stats stats_0, stats_1;

    void worker(std::shared_ptr<RawSocketManager> rx, std::shared_ptr<RawSocketManager> tx, 
                Stats& s, int core, std::stop_token st) {
        // 核心隔离与实时优先级设置
        cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        struct sched_param param{.sched_priority = 99};
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        std::uint32_t idx = 0;
        while (!st.stop_requested()) {
            auto* hdr = reinterpret_cast<tpacket_hdr*>(rx->get_ring() + (idx * rx->frame_size()));
            if (hdr->tp_status & TP_STATUS_USER) {
                std::span packet{reinterpret_cast<std::uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len};
                
                if (processor->process(packet)) {
                    send(tx->get_fd(), packet.data(), packet.size(), MSG_DONTWAIT);
                    s.forwarded.fetch_add(1, std::memory_order_relaxed);
                }
                hdr->tp_status = TP_STATUS_KERNEL;
                idx = (idx + 1) % rx->frame_nr();
                s.heartbeat.store(time(nullptr), std::memory_order_relaxed);
            } else {
                __builtin_ia32_pause(); // 自旋减少延迟
            }
        }
    }

    void watchdog_and_led(std::stop_token st) {
        std::ofstream led("/sys/class/leds/led0/brightness");
        while (!st.stop_requested()) {
            auto now = time(nullptr);
            // 看门狗：如果线程 5秒未响应则自愈
            if (now - stats_0.heartbeat > 5 || now - stats_1.heartbeat > 5) {
                std::print(stderr, "FAIL-SAFE: Thread hang! Resetting Bridge...\n");
                system("ip link set eth0 master br0 && ip link set eth1 master br0"); // 模拟自愈脚本
                exit(1);
            }
            // 正常运行：LED 呼吸效果
            if (led.is_open()) { led << "1" << std::endl; std::this_thread::sleep_for(100ms); led << "0" << std::endl; }
            std::this_thread::sleep_for(900ms);
        }
    }
};