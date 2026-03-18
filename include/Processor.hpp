#pragma once
#include <span>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <bit>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {
    struct FlowKey {
        uint32_t saddr, daddr;
        uint16_t sport, dport;
        bool operator==(const FlowKey&) const = default;
    };

    struct FlowHash {
        std::size_t operator()(const FlowKey& k) const {
            // 引入 std::rotl 进行位旋转打散。
            // 解决双向流 (A->B 与 B->A) 的 100% 哈希冲突
            return std::rotl(static_cast<std::size_t>(k.saddr), 16) ^
                static_cast<std::size_t>(k.daddr) ^
                std::rotl(static_cast<std::size_t>(k.sport), 8) ^
                static_cast<std::size_t>(k.dport);
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

            // ===== 替换后 =====
            if (ntohs(eth->proto) != 0x0800) return Priority::Normal; // Only IPv4

            // 安全检查：确保包长足够提取 IP 协议号和 UDP 端口防止内存越界 (14+20+4 = 38)
            if (pkt.size() < 38) return Priority::Normal;

            // 2. 定位到 IP 协议位 (14字节偏移 + 9字节偏移 = 23字节处)
            uint8_t protocol = pkt[23];

            // 3. DNS 极速判定 (核心优化点)
            // 只要是 UDP (17)，立即检查端口，不进入下面的流表统计逻辑
            if (protocol == 17 && (pkt[14] & 0x0F) == 5) {
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
            if (ip->protocol == 6 && pkt.size() < 74) return Priority::Critical;

            // 2. UDP Heuristic Analysis 
            if (ip->protocol == 17) {
                size_t offset = sizeof(EthernetHeader) + ihl;
                if (pkt.size() < offset + sizeof(UDPHeader)) return Priority::Normal;

                auto udp = reinterpret_cast<const UDPHeader*>(pkt.data() + offset);
                uint16_t dport = ntohs(udp->dest);
                uint16_t sport = ntohs(udp->source);

                if (dport == 53 || sport == 53) return Priority::Critical; // DNS
                // --- QUIC (443) 属于加速对象，但不计入惩罚 ---
                if ((dport == 443 || sport == 443) && pkt.size() < 512) {
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

            if (ip->protocol == 6) {
                // 识别 SYN 包 (TCP 头部偏移在 IHL 之后)
                if (pkt.size() < 74) return Priority::Critical;

                // 利用新增的 TCPHeader 提取端口，拯救 Bing 搜索的高延迟
                size_t offset = sizeof(EthernetHeader) + ihl;
                if (pkt.size() >= offset + sizeof(TCPHeader)) {
                    auto tcp = reinterpret_cast<const TCPHeader*>(pkt.data() + offset);
                    uint16_t dport = ntohs(tcp->dest);
                    uint16_t sport = ntohs(tcp->source);

                    // 修复：游戏端口无条件极速。但 443(HTTPS) 只有小于 512 字节的包才免排队。
                    // 否则看视频/下文件会撑爆网卡物理硬件队列导致海量 High 级别丢包。
                    if (Config::is_game_port(dport) || Config::is_game_port(sport)) {
                        return Priority::High;
                    }
                    if ((dport == 443 || sport == 443) && pkt.size() < 512) {
                        return Priority::High;
                    }
                }
            }

            return Priority::Normal;
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