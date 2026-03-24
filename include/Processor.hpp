#pragma once
#include <span>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <bit>
#include <array>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    // 5元组流量标识
    struct FlowKey {
        uint32_t saddr, daddr;
        uint16_t sport, dport;
        bool operator==(const FlowKey&) const = default;
    };

    // 流量统计信息
    struct FlowStats {
        uint32_t total_pkts = 0;
        uint32_t large_pkts = 0;
        bool is_disguised = false;
        std::chrono::steady_clock::time_point last_seen;
    };

    // 基于 FNV-1a 算法的静态流表 (零动态分配)
    template<size_t Capacity = 4096>
    class StaticFlowMap {
        struct Entry {
            FlowKey key{};
            FlowStats stats{};
            bool occupied = false;
        };

        std::array<Entry, Capacity> table{};

        // FNV-1a 字节哈希实现
        static uint32_t fnv1a_hash(const FlowKey& k) {
            uint32_t h = 2166136261U;
            auto process_bytes = [&](const auto& val) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
                for (size_t i = 0; i < sizeof(val); ++i) {
                    h ^= p[i]; h *= 16777619U;
                }
            };
            process_bytes(k.saddr); process_bytes(k.daddr);
            process_bytes(k.sport); process_bytes(k.dport);
            return h;
        }

    public:
        // 在热路径中查找或创建流实体
        FlowStats* get_or_create(const FlowKey& key) {
            uint32_t h = fnv1a_hash(key) % Capacity;
            for (size_t i = 0; i < Capacity; ++i) {
                size_t idx = (h + i) % Capacity;
                if (!table[idx].occupied) {
                    table[idx].key = key;
                    table[idx].occupied = true;
                    table[idx].stats = {};
                    return &table[idx].stats;
                }
                if (table[idx].key == key) {
                    return &table[idx].stats;
                }
            }
            return nullptr;
        }

        // 定期清理过期流
        void cleanup(std::chrono::seconds timeout) {
            auto now = std::chrono::steady_clock::now();
            for (auto& entry : table) {
                if (entry.occupied && (now - entry.stats.last_seen > timeout)) {
                    entry.occupied = false;
                }
            }
        }
    };

    // 启发式流量识别引擎
    class HeuristicProcessor {
        StaticFlowMap<4096> flows;
        uint32_t process_counter = 0;

        using ProtocolHandler = Net::Priority(*)(HeuristicProcessor*, std::span<const uint8_t>, const Net::IPv4Header*, size_t);
        std::array<ProtocolHandler, 256> protocol_handlers;

        // UDP 协议特定识别逻辑
        static Net::Priority handle_udp(HeuristicProcessor* self, std::span<const uint8_t> pkt, const Net::IPv4Header* ip, size_t ihl) {
            size_t offset = sizeof(Net::EthernetHeader) + ihl;
            if (pkt.size() < offset + sizeof(Net::UDPHeader)) return Net::Priority::Normal;

            auto udp = reinterpret_cast<const Net::UDPHeader*>(pkt.data() + offset);
            uint16_t dport = ntohs(udp->dest);
            uint16_t sport = ntohs(udp->source);

            // DNS 优先放行
            if (dport == 53 || sport == 53) return Net::Priority::Critical;

            FlowKey key{ ip->saddr, ip->daddr, sport, dport };
            auto* stats = self->flows.get_or_create(key);
            
            if (stats) {
                stats->total_pkts++;
                stats->last_seen = std::chrono::steady_clock::now();
                if (pkt.size() > Config::LARGE_PACKET_THRESHOLD) stats->large_pkts++;
                
                // 伪装流量检测 (如 UDP-Ping 洪水)
                if (!stats->is_disguised && stats->total_pkts < 50) {
                    if (stats->large_pkts > Config::PUNISH_TRIGGER_COUNT) stats->is_disguised = true;
                }
                if (stats->is_disguised) return Net::Priority::Normal;
            }

            // 周期性清理
            if (++self->process_counter > Config::CLEANUP_INTERVAL) {
                self->flows.cleanup(std::chrono::seconds(30));
                self->process_counter = 0;
            }

            if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
            return pkt.size() < 256 ? Net::Priority::High : Net::Priority::Normal;
        }

        // TCP 协议识别
        static Net::Priority handle_tcp(HeuristicProcessor*, std::span<const uint8_t> pkt, const Net::IPv4Header* ip, size_t ihl) {
            if (pkt.size() < 74) return Net::Priority::Critical; // 优先小包 (SYN/ACK)
            size_t offset = sizeof(Net::EthernetHeader) + ihl;
            if (pkt.size() >= offset + sizeof(Net::TCPHeader)) {
                auto tcp = reinterpret_cast<const Net::TCPHeader*>(pkt.data() + offset);
                uint16_t dport = ntohs(tcp->dest);
                uint16_t sport = ntohs(tcp->source);
                if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
            }
            return Net::Priority::Normal;
        }

        static Net::Priority handle_default(HeuristicProcessor*, std::span<const uint8_t>, const Net::IPv4Header*, size_t) {
            return Net::Priority::Normal;
        }

    public:
        HeuristicProcessor() {
            protocol_handlers.fill(handle_default);
            protocol_handlers[17] = handle_udp;
            protocol_handlers[6] = handle_tcp;
        }

        // 识别主入口
        Net::Priority process(std::span<const uint8_t> pkt) {
            if (pkt.size() < sizeof(Net::EthernetHeader)) return Net::Priority::Normal;
            auto eth = reinterpret_cast<const Net::EthernetHeader*>(pkt.data());
            if (ntohs(eth->proto) != 0x0800) return Net::Priority::Normal;

            if (pkt.size() < sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header)) return Net::Priority::Normal;
            auto ip = reinterpret_cast<const Net::IPv4Header*>(pkt.data() + sizeof(Net::EthernetHeader));
            size_t ihl = (ip->ver_ihl & 0x0F) * 4;
            uint8_t protocol = ip->protocol;

            return protocol_handlers[protocol](this, pkt, ip, ihl);
        }
    };
}

