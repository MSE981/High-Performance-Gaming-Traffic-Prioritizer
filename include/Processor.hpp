#pragma once
#include <span>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <netinet/in.h> // 用于解析时的 ntohs
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {
    struct FlowKey {
        uint32_t saddr, daddr;
        uint16_t sport, dport;
        bool operator==(const FlowKey&) const = default; // C++20 default compare
    };

    struct FlowHash {
        std::size_t operator()(const FlowKey& k) const {
            return k.saddr ^ k.daddr ^ k.sport ^ k.dport;
        }
    };

    struct FlowStats {
        uint32_t total_pkts = 0;
        uint32_t large_pkts = 0;
        bool is_disguised = false;
        std::chrono::steady_clock::time_point last_seen;
    };

    // 启发式处理器：每个线程拥有独立实例，无需锁
    class HeuristicProcessor {
        std::unordered_map<FlowKey, FlowStats, FlowHash> flows;
        uint32_t process_counter = 0;

    public:
        Net::Priority process(std::span<const uint8_t> pkt) {
            using namespace Scalpel::Net;

            if (pkt.size() < sizeof(EthernetHeader)) return Priority::Normal;
            auto eth = reinterpret_cast<const EthernetHeader*>(pkt.data());

            if (ntohs(eth->proto) != 0x0800) return Priority::Normal; // Only IPv4


            // 2. 定位到 IP 协议位 (14字节偏移 + 9字节偏移 = 23字节处)
            uint8_t protocol = pkt[23];

            // 3. DNS 极速判定 (核心优化点)
            // 只要是 UDP (17)，立即检查端口，不进入下面的流表统计逻辑
            if (protocol == 17) {
                uint16_t dport = (pkt[36] << 8) | pkt[37]; // 手动提取目的端口
                uint16_t sport = (pkt[34] << 8) | pkt[35]; // 手动提取源端口

                if (dport == 53 || sport == 53) {
                    return Priority::Critical; // 发现 DNS 立即返回，不更新 FlowTable
                }
            }









            // Check bounds for IP
            if (pkt.size() < sizeof(EthernetHeader) + sizeof(IPv4Header)) return Priority::Normal;
            auto ip = reinterpret_cast<const IPv4Header*>(pkt.data() + sizeof(EthernetHeader));
            size_t ihl = (ip->ver_ihl & 0x0F) * 4;

            // 1. TCP ACK Optimization 
            if (ip->protocol == 6 && pkt.size() < 64) return Priority::Critical;

            // 2. UDP Heuristic Analysis 
            if (ip->protocol == 17) {
                size_t offset = sizeof(EthernetHeader) + ihl;
                if (pkt.size() < offset + sizeof(UDPHeader)) return Priority::Normal;

                auto udp = reinterpret_cast<const UDPHeader*>(pkt.data() + offset);
                uint16_t dport = ntohs(udp->dest);
                uint16_t sport = ntohs(udp->source);

                if (dport == 53 || sport == 53) return Priority::Critical; // DNS
                // --- 优化 2：QUIC (443) 属于加速对象，但不计入惩罚 ---
                if (dport == 443 || sport == 443) {
                    // 赋予 High 优先级，但不执行下面判断大包并降级的逻辑
                    return Priority::High;
                }






                // Flow Analysis
                FlowKey key{ ip->saddr, ip->daddr, sport, dport };
                auto& stats = flows[key];
                stats.total_pkts++;
                stats.last_seen = std::chrono::steady_clock::now();

                if (pkt.size() > Config::LARGE_PACKET_THRESHOLD) stats.large_pkts++;

                // Punishment Logic
                if (!stats.is_disguised && stats.total_pkts < 50) {
                    if (stats.large_pkts > Config::PUNISH_TRIGGER_COUNT) {
                        stats.is_disguised = true;
                    }
                }

                if (stats.is_disguised) return Priority::Normal;
                if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Priority::High;
                if (pkt.size() < 256) return Priority::High; // Unknown small packet
            }

            // Periodic Cleanup
            if (++process_counter > Config::CLEANUP_INTERVAL) {
                cleanup();
                process_counter = 0;
            }

            return Priority::Normal;
            if (ip->protocol == 6) {
                // 识别 SYN 包 (TCP 头部偏移在 IHL 之后)
                // 简单判定：长度很小的 TCP 包通常是握手或确认包
                if (pkt.size() < 70) return Priority::Critical;
            }
        }

    private:
        void cleanup() {
            auto now = std::chrono::steady_clock::now();
            std::erase_if(flows, [&](const auto& item) {
                return (now - item.second.last_seen) > std::chrono::seconds(30);
                });
        }
    };
}