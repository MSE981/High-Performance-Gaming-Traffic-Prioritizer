#include <chrono>
#include <cstring>  // memset
#include <vector>
#include <print>    // std::println
#include <sys/socket.h> // send()
#include "Telemetry.hpp"
#include "Processor.hpp"
#include <cstdint>
#include <cstdio>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

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

            uint8_t pkt[64]; std::memset(pkt, 0xEE, 64);
            auto start = std::chrono::high_resolution_clock::now();
            uint64_t sent = 0;
            auto interval = std::chrono::nanoseconds(1000000000 / 900000); // Target 450k PPS

            while (std::chrono::high_resolution_clock::now() - start < std::chrono::seconds(5)) {
                auto loop_start = std::chrono::high_resolution_clock::now();
                send(socket_fd, pkt, 64, MSG_DONTWAIT);
                sent++;
                while (std::chrono::high_resolution_clock::now() - loop_start < interval) {
                     __asm__ __volatile__("yield" ::: "memory");
                }
            }
            
            double pps = sent / 5.0;
            tel.isp_limit_mbps = (sent * 64.0 * 8.0) / (2.0 * 1e6);
            std::println("[Probe B] ISP Result: {:.2f} Mbps ({} PPS)", tel.isp_limit_mbps.load(), pps);
            tel.is_probing = false;
        }
        // --- [新增] 模式 C：真实 ISP 限制探测 (穿透公网) ---
      /**
       * @param gateway_mac 路由器的真实 MAC 地址
       * @param local_ip 树莓派当前内网 IP
       * @param target_ip 探测目标 (默认使用 Google DNS 8.8.8.8)
       */
        static void run_real_isp_probe(int fd, const std::string& gateway_mac, const std::string& local_ip, const std::string& target_ip = "8.8.8.8") {
            auto& tel = Telemetry::instance();
            tel.is_probing = true;
            std::println("[Probe C] Starting Real-world ISP PPS Probe to {}...", target_ip);

            uint8_t frame[64]; std::memset(frame, 0, 64);

            // 1. 构造以太网头 (L2)
            auto* eth = reinterpret_cast<struct ether_header*>(frame);
            sscanf(gateway_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                &eth->ether_dhost[0], &eth->ether_dhost[1], &eth->ether_dhost[2],
                &eth->ether_dhost[3], &eth->ether_dhost[4], &eth->ether_dhost[5]);
            eth->ether_type = htons(0x0800); // IPv4

            // 2. 构造 IP 头 (L3)
            auto* ip = reinterpret_cast<struct iphdr*>(frame + 14);
            ip->ihl = 5; ip->version = 4;
            ip->tot_len = htons(64 - 14);
            ip->ttl = 64; ip->protocol = IPPROTO_UDP;
            ip->saddr = inet_addr(local_ip.c_str());
            ip->daddr = inet_addr(target_ip.c_str());
            ip->check = calculate_checksum(reinterpret_cast<uint16_t*>(ip), 10);

            // 3. 构造 UDP 头 (L4)
            auto* udp = reinterpret_cast<struct udphdr*>(frame + 14 + 20);
            udp->source = htons(12345);
            udp->dest = htons(53); // 使用 DNS 端口降低拦截风险
            udp->len = htons(64 - 14 - 20);

            // --- 阶梯式压测逻辑 ---
            for (uint32_t step_pps = 100000; step_pps <= 500000; step_pps += 50000) {
                uint64_t sent_step = 0;
                auto interval = std::chrono::nanoseconds(1000000000 / step_pps);
                auto step_start = std::chrono::high_resolution_clock::now();

                while (std::chrono::high_resolution_clock::now() - step_start < std::chrono::seconds(1)) {
                    auto loop_start = std::chrono::high_resolution_clock::now();
                    if (send(fd, frame, 64, MSG_DONTWAIT) > 0) sent_step++;

                    while (std::chrono::high_resolution_clock::now() - loop_start < interval) {
                        __asm__ __volatile__("yield" ::: "memory");
                    }
                }

                double mbps = (sent_step * 64.0 * 8.0) / 1e6;
                std::println("  - Step {:6} PPS: Actual {:10.2f} PPS | {:7.2f} Mbps", step_pps, (double)sent_step, mbps);
                tel.isp_limit_mbps = mbps; // 更新 Telemetry 记录
            }

            std::println("[Probe C] ISP Probe Complete.");
            tel.is_probing = false;
        }






    };
}