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
        // 模式 C：真实 ISP 限制探测 (调用 Ookla Speedtest 官方极简测速)
        static void run_real_isp_probe(int /*fd*/, const std::string& /*gateway_mac*/, const std::string& /*local_ip*/, const std::string & /*target_ip*/ = "") {
            auto& tel = Telemetry::instance();
            tel.is_probing = true;
            std::println("[Probe C] Running Ookla Speedtest (speedtest-cli)... This may take 15-20 seconds.");

            std::array<char, 128> buffer;
            std::string result;

            // 使用 popen 调用系统的 speedtest-cli 命令，--simple 参数输出纯文本，2>/dev/null 屏蔽错误警告
            std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("speedtest-cli --simple 2>/dev/null", "r"), pclose);
            if (!pipe) {
                std::println(stderr, "[Error] Failed to start speedtest-cli. Please run 'sudo apt install speedtest-cli' first.");
                tel.is_probing = false;
                return;
            }

            // 读取命令行输出
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }

            // Speedtest-cli --simple 的输出格式通常为：
            // Ping: 12.34 ms
            // Download: 21.50 Mbit/s
            // Upload: 5.20 Mbit/s

            double download_mbps = 0.0;
            double upload_mbps = 0.0;

            auto pos_down = result.find("Download:");
            if (pos_down != std::string::npos) {
                try {
                    std::string sub = result.substr(pos_down + 9);
                    download_mbps = std::stod(sub);
                }
                catch (...) {}
            }

            auto pos_up = result.find("Upload:");
            if (pos_up != std::string::npos) {
                try {
                    std::string sub = result.substr(pos_up + 8);
                    upload_mbps = std::stod(sub);
                }
                catch (...) {}
            }

            // 成功测出上下行宽带时，分别写入全局变量供限速器分离使用
            if (download_mbps > 0.0 && upload_mbps > 0.0) {
                tel.isp_down_limit_mbps = download_mbps;
                tel.isp_up_limit_mbps = upload_mbps;
                std::println("[Probe C] Speedtest Complete! Down: {:.2f} Mbps | Up: {:.2f} Mbps", download_mbps, upload_mbps);
            }
            else {
                std::println(stderr, "[Error] Speedtest failed or returned 0. Please check network connection.");
            }
            tel.is_probing =    false;
        }
    };
}