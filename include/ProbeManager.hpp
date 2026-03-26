#pragma once
#include <chrono>
#include <cstring>
#include <vector>
#include <print>
#include <array>
#include <memory>
#include <functional>
#include <sys/socket.h>
#include "Telemetry.hpp"
#include "Processor.hpp"
#include <cstdint>
#include <cstdio>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <string>
#include <thread>
#include <sys/timerfd.h>
#include <unistd.h>

namespace Scalpel::Probe {
    class Manager {       
        static uint16_t calculate_checksum(uint16_t* buf, int nwords) {
            unsigned long sum = 0;
            for (; nwords > 0; nwords--) sum += *buf++;
            sum = (sum >> 16) + (sum & 0xffff);
            sum += (sum >> 16);
            return (uint16_t)(~sum);
        }

    public:
        // Mode A: internal compute stress test
        static void run_internal_stress() {
            auto& tel = Telemetry::instance();
            tel.is_probing.store(true, std::memory_order_relaxed);
            std::println("[Probe A] Benchmarking internal logic...");

            Logic::HeuristicProcessor temp_proc;
            uint8_t dummy_data[64] = {0};
            // Construct fake IPv4 header to prevent parse crash
            dummy_data[12] = 0x08; dummy_data[13] = 0x00; // Eth proto
            dummy_data[14] = 0x45; // IP Ver/IHL
            auto start = std::chrono::steady_clock::now();
            uint64_t count = 0;

            // Pure low-level algorithm stress, no yield to max CPU throughput
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
                auto pkt_ctx = Net::ParsedPacket::parse(std::span<uint8_t>{dummy_data});
                temp_proc.process(pkt_ctx);
                count++;
            }

            double pps = count / 5.0;
            tel.internal_limit_mbps.store((pps * 64 * 8) / 1e6, std::memory_order_relaxed);
            std::println("[Probe A] CPU capacity: {:.2f} Mbps ({} PPS)", tel.internal_limit_mbps.load(), pps);
            tel.is_probing.store(false, std::memory_order_relaxed);
        }

        // Mode B: ISP PPS probing (deterministic precise rate limiting via timerfd)
        static void run_isp_probe(int socket_fd) {
            auto& tel = Telemetry::instance();
            tel.is_probing.store(true, std::memory_order_relaxed);
            std::println("[Probe B] Probing ISP limits...");
            uint8_t pkt[64] = { 0 };
            std::memset(pkt, 0xEE, 64);

            // Create hardware monotonic timer
            int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
            if (tfd == -1) return;

            struct itimerspec its{};
            its.it_value.tv_sec = 0;
            its.it_value.tv_nsec = 555; // Target 1.8M PPS (about every 555 nanoseconds)
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 555;
            timerfd_settime(tfd, 0, &its, NULL);

            auto start = std::chrono::steady_clock::now();
            uint64_t sent = 0;
            uint64_t expirations = 0;

            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
                // Block in kernel; returns number of ticks missed since last wakeup
                if (read(tfd, &expirations, sizeof(expirations)) > 0) {
                    // Deterministic burst: send one packet per missed tick, eliminates busy-spin
                    for (uint64_t i = 0; i < expirations; ++i) {
                        if (send(socket_fd, pkt, 64, MSG_DONTWAIT) >= 0) {
                            sent++;
                        }
                    }
                }
            }
            
            close(tfd);

            double pps = sent / 5.0;
            double hw_mbps = (sent * 64.0 * 8.0) / (5.0 * 1e6);
            std::println("[Probe B] Local Hardware Tx Limit: {:.2f} Mbps ({} PPS)", hw_mbps, pps);
            tel.is_probing.store(false, std::memory_order_relaxed);
        }


        // Mode C: async Ookla Speedtest
        // Accepts a callback; fires when the test completes
        static void run_async_real_isp_probe(std::function<void(double, double)> on_complete) {
            std::println("[Probe C] Spawning asynchronous speedtest thread. Realtime engine will NOT block.");

            // Spawn background thread for blocking task; fire-and-forget, no stop-token needed
            std::thread([cb = std::move(on_complete)]() {
                std::array<char, 128> buffer{};
                std::string result;

                // popen runs in background thread only — never blocks packet forwarding
                std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("speedtest-cli --simple 2>/dev/null", "r"), pclose);
                if (!pipe) {
                    std::println(stderr, "[Probe C Error] Speedtest process failed to start.");
                    return;
                }

                while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                    result += buffer.data();
                }

                double download_mbps = 0.0;
                double upload_mbps = 0.0;

                auto pos_down = result.find("Download:");
                if (pos_down != std::string::npos) {
                    try { download_mbps = std::stod(result.substr(pos_down + 9)); }
                    catch (...) {}
                }

                auto pos_up = result.find("Upload:");
                if (pos_up != std::string::npos) {
                    try { upload_mbps = std::stod(result.substr(pos_up + 8)); }
                    catch (...) {}
                }

                if (download_mbps > 0.0 && upload_mbps > 0.0) {
                    std::println("\n[Probe C] Async Speedtest Success! Down: {:.2f} Mbps | Up: {:.2f} Mbps", download_mbps, upload_mbps);
                    // Task complete: fire callback to notify main program
                    if (cb) cb(download_mbps, upload_mbps);
                }
                else {
                    std::println(stderr, "\n[Probe C Error] Speedtest returned invalid results.");
                }
                }).detach();
        }
    };
}

