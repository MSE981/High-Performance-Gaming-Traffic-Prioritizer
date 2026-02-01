#pragma once
#include <chrono>
#include <cstring>  // memset
#include <vector>
#include <print>    // std::println
#include <sys/socket.h> // send()
#include "Telemetry.hpp"
#include "Processor.hpp"

namespace Scalpel::Probe {
    class Manager {
    public:
        // 模式 A：内部计算压力测试
        static void run_internal_stress() {
            auto& tel = Telemetry::instance();
            tel.is_probing = true;
            std::println("[Probe A] Benchmarking internal logic...");

            Logic::HeuristicProcessor temp_proc;
            uint8_t dummy_data[64] = {0};
            // 构造假 IPv4 头防止解析崩溃
            dummy_data[12] = 0x08; dummy_data[13] = 0x00; // Eth Proto
            dummy_data[14] = 0x45; // IP Ver/IHL
            
            auto start = std::chrono::high_resolution_clock::now();
            uint64_t count = 0;

            while (std::chrono::high_resolution_clock::now() - start < std::chrono::seconds(2)) {
                temp_proc.process(std::span{dummy_data});
                count++;
                __asm__ __volatile__("yield" ::: "memory");
            }

            double pps = count / 2.0;
            tel.internal_limit_mbps = (pps * 64 * 8) / 1e6;
            std::println("[Probe A] CPU Capacity: {:.2f} Mbps ({} PPS)", tel.internal_limit_mbps.load(), pps);
            tel.is_probing = false;
        }

        // 模式 B：ISP PPS 探测 (简单发包，需连接 eth0)
        static void run_isp_probe(int socket_fd) {
            auto& tel = Telemetry::instance();
            tel.is_probing = true;
            std::println("[Probe B] Probing ISP limits...");

            uint8_t pkt[64]; std::memset(pkt, 0xEE, 64);
            auto start = std::chrono::high_resolution_clock::now();
            uint64_t sent = 0;
            auto interval = std::chrono::nanoseconds(1000000000 / 150000); // Target 150k PPS

            while (std::chrono::high_resolution_clock::now() - start < std::chrono::seconds(2)) {
                auto loop_start = std::chrono::high_resolution_clock::now();
                send(socket_fd, pkt, 64, MSG_DONTWAIT);
                sent++;
                while (std::chrono::high_resolution_clock::now() - loop_start < interval) {
                     __asm__ __volatile__("yield" ::: "memory");
                }
            }
            
            tel.isp_limit_mbps = (sent * 64.0 * 8.0) / (2.0 * 1e6);
            std::println("[Probe B] ISP Result: {:.2f} Mbps", tel.isp_limit_mbps.load());
            tel.is_probing = false;
        }
    };
}