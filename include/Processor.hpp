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

        // 定义基于协议的查表处理器 (Functor Table)
        using ProtocolHandler = Net::Priority(*)(HeuristicProcessor*, std::span<const uint8_t>, const Net::IPv4Header*, size_t);
        std::array<ProtocolHandler, 256> protocol_handlers;

        // UDP 解析逻辑封装
        static Net::Priority handle_udp(HeuristicProcessor* self, std::span<const uint8_t> pkt, const Net::IPv4Header* ip, size_t ihl) {
            size_t offset = sizeof(Net::EthernetHeader) + ihl;
            if (pkt.size() < offset + sizeof(Net::UDPHeader)) return Net::Priority::Normal;

            auto udp = reinterpret_cast<const Net::UDPHeader*>(pkt.data() + offset);
            uint16_t dport = ntohs(udp->dest);
            uint16_t sport = ntohs(udp->source);

            if (dport == 53 || sport == 53) return Net::Priority::Critical;
            if ((dport == 443 || sport == 443) && pkt.size() < 512) return Net::Priority::High;

            FlowKey key{ ip->saddr, ip->daddr, sport, dport };
            auto& stats = self->flows[key];
            stats.total_pkts++;
            stats.last_seen = std::chrono::steady_clock::now();

            if (pkt.size() > Config::LARGE_PACKET_THRESHOLD) stats.large_pkts++;

            if (!stats.is_disguised && stats.total_pkts < 50) {
                if (stats.large_pkts > Config::PUNISH_TRIGGER_COUNT) stats.is_disguised = true;
            }

            // 只有影响了 UDP 状态机时，才增加清理计数器，节省无关协议的 CPU 损耗
            if (++self->process_counter > Config::CLEANUP_INTERVAL) {
                self->cleanup();
                self->process_counter = 0;
            }

            if (stats.is_disguised) return Net::Priority::Normal;
            if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
            if (pkt.size() < 256) return Net::Priority::High;

            return Net::Priority::Normal;
        }

        // TCP 解析逻辑封装
        static Net::Priority handle_tcp(HeuristicProcessor*, std::span<const uint8_t> pkt, const Net::IPv4Header* ip, size_t ihl) {
            // 识别 SYN / ACK 建连确认包
            if (pkt.size() < 74) return Net::Priority::Critical;

            size_t offset = sizeof(Net::EthernetHeader) + ihl;
            if (pkt.size() >= offset + sizeof(Net::TCPHeader)) {
                auto tcp = reinterpret_cast<const Net::TCPHeader*>(pkt.data() + offset);
                uint16_t dport = ntohs(tcp->dest);
                uint16_t sport = ntohs(tcp->source);

                if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
                if ((dport == 443 || sport == 443) && pkt.size() < 512) return Net::Priority::High;
            }
            return Net::Priority::Normal;
        }

        static Net::Priority handle_default(HeuristicProcessor*, std::span<const uint8_t>, const Net::IPv4Header*, size_t) {
            return Net::Priority::Normal;
        }

    public:
        HeuristicProcessor() {
            // 初始化协议路由表
            protocol_handlers.fill(handle_default);
            protocol_handlers[17] = handle_udp; // UDP 处理器
            protocol_handlers[6] = handle_tcp; // TCP 处理器
        }

        Net::Priority process(std::span<const uint8_t> pkt) {
            using namespace Scalpel::Net;

            if (pkt.size() < sizeof(EthernetHeader)) return Priority::Normal;
            auto eth = reinterpret_cast<const EthernetHeader*>(pkt.data());
            if (ntohs(eth->proto) != 0x0800) return Priority::Normal;

            if (pkt.size() < 38) return Priority::Normal;
            uint8_t protocol = pkt[23];

            // DNS 极速绕过通道保留
            if (protocol == 17 && (pkt[14] & 0x0F) == 5) {
                uint16_t dport = (pkt[36] << 8) | pkt[37];
                uint16_t sport = (pkt[34] << 8) | pkt[35];
                if (dport == 53 || sport == 53) return Priority::Critical;
            }

            if (pkt.size() < sizeof(EthernetHeader) + sizeof(IPv4Header)) return Priority::Normal;
            auto ip = reinterpret_cast<const IPv4Header*>(pkt.data() + sizeof(EthernetHeader));
            size_t ihl = (ip->ver_ihl & 0x0F) * 4;

            //  函数指针查表分发，
            return protocol_handlers[protocol](this, pkt, ip, ihl);
        }

    private:
        void cleanup() {
            auto now = std::chrono::steady_clock::now();
            std::erase_if(flows, [&](const auto& item) {
                return (now - item.second.last_seen) > std::chrono::seconds(30);
                });
        }
    };