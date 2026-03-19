#include <chrono>
#include <cstring>
#include <vector>
#include <print>
#include <array>
#include <memory>
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

            while (std::chrono::high_resolution_clock::now() - start < std::chrono::seconds(5)) {
                temp_proc.process(std::span{dummy_data});
                count++;
                __asm__ __volatile__("yield" ::: "memory");
            }

            double pps = count / 5.0;
            tel.internal_limit_mbps = (pps * 64 * 8) / 1e6;
            std::println("[Probe A] CPU Capacity: {:.2f} Mbps ({} PPS)", tel.internal_limit_mbps.load(), pps);
            tel.is_probing = false;
        }

        // 模式 B：ISP PPS 探测 (简单发包，需连接 eth0)
        static void run_isp_probe(int socket_fd) {
            auto& tel = Telemetry::instance();
            tel.is_probing = true;
            std::println("[Probe B] Probing ISP limits...");
            uint8_t pkt[64] = { 0 };
            std::memset(pkt, 0xEE, 64);
            auto start = std::chrono::high_resolution_clock::now();
            uint64_t sent = 0;
            auto interval = std::chrono::nanoseconds(1000000000 / 1800000); // Target 1.8M PPS

            while (std::chrono::high_resolution_clock::now() - start < std::chrono::seconds(5)) {
                auto loop_start = std::chrono::high_resolution_clock::now();
                send(socket_fd, pkt, 64, MSG_DONTWAIT);
                //只有网卡物理层真正接收了该包才计数
                if (send(socket_fd, pkt, 64, MSG_DONTWAIT) >= 0) {
                    sent++;
                }
                while (std::chrono::high_resolution_clock::now() - loop_start < interval) {
                     __asm__ __volatile__("yield" ::: "memory");
                }
            }
            
            double pps = sent / 5.0;
            tel.isp_limit_mbps = (sent * 64.0 * 8.0) / (5.0 * 1e6);
            std::println("[Probe B] ISP Result: {:.2f} Mbps ({} PPS)", tel.isp_limit_mbps.load(), pps);
            tel.is_probing = false;
        }


        // 模式 C：异步调用 Ookla Speedtest
        // 传入一个回调函数，测速完成后自动触发
        static void run_async_real_isp_probe(std::function<void(double, double)> on_complete) {
            std::println("[Probe C] Spawning asynchronous speedtest thread. Realtime engine will NOT block.");

            // 启动独立后台线程执行耗时任务，完全脱离实时主干道
            std::thread([cb = std::move(on_complete)]() {
                std::array<char, 128> buffer{};
                std::string result;

                // 在纯后台线程中执行 popen，绝对不会影响网络抓包和转发的实时性
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
                    // 任务完成，触发回调函数通知主程序！
                    if (cb) cb(download_mbps, upload_mbps);
                }
                else {
                    std::println(stderr, "\n[Probe C Error] Speedtest returned invalid results.");
                }
                }).detach(); // 分离线程，让其在后台默默执行
        }
    };
}