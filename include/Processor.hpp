#pragma once
#include <span>
#include <cstdint>
#include <cstddef>
#include <array>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    // Big-endian wire uint16 → host (Pi 5 is little-endian). Avoids public <netinet/in.h> in this header.
    constexpr inline uint16_t net16_to_host(uint16_t be) noexcept {
        return static_cast<uint16_t>((be << 8) | (be >> 8));
    }

    // 5-tuple flow identifier
    struct FlowKey {
        Net::IPv4Net saddr, daddr;   // NBO — matched directly against IPv4Header fields
        uint16_t     sport, dport;
        bool operator==(const FlowKey&) const = default;
    };

    // Flow statistics
    struct FlowStats {
        uint32_t total_pkts = 0;
        uint32_t large_pkts = 0;
        uint32_t last_pkt   = 0;   // processor-local packet count at last observation
        bool is_disguised = false;
    };

    // Static flow table based on FNV-1a algorithm (zero dynamic allocation)
    template<size_t Capacity = 4096>
    class StaticFlowMap {
        struct Entry {
            FlowKey key{};
            FlowStats stats{};
            bool occupied = false;
        };

        std::array<Entry, Capacity> table{};

        // FNV-1a byte hash implementation
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
        // Find or create flow entity in hot path
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

        // Periodically clean up flows idle for more than timeout_pkts packets
        void cleanup(uint32_t current_pkt, uint32_t timeout_pkts) {
            for (auto& entry : table) {
                if (entry.occupied && (current_pkt - entry.stats.last_pkt) > timeout_pkts) {
                    entry.occupied = false;
                }
            }
        }
    };

    // Heuristic traffic identification engine
    class HeuristicProcessor {
        StaticFlowMap<4096> flows;
        uint32_t process_counter = 0;
        uint32_t pkt_count = 0;   // monotonic packet counter — lightweight logical clock

        using ProtocolHandler =
            Net::Priority (*)(HeuristicProcessor*, const Net::ParsedPacket&);
        std::array<ProtocolHandler, 256> protocol_handlers;

        // UDP protocol-specific identification logic
        static Net::Priority handle_udp(HeuristicProcessor* self, const Net::ParsedPacket& parsed) {
            auto udp = parsed.udp();
            if (!udp) return Net::Priority::Normal;

            uint16_t dport = net16_to_host(udp->dest);
            uint16_t sport = net16_to_host(udp->source);

            // DNS priority pass
            if (dport == 53 || sport == 53) return Net::Priority::Critical;

            FlowKey key{ parsed.ipv4->saddr, parsed.ipv4->daddr, sport, dport };
            auto* stats = self->flows.get_or_create(key);

            if (stats) {
                stats->total_pkts++;
                stats->last_pkt = self->pkt_count;   // free integer store, no syscall
                if (parsed.raw_span.size() > Config::LARGE_PACKET_THRESHOLD_BYTES) stats->large_pkts++;

                // Disguised traffic detection (e.g., UDP-Ping flood)
                if (!stats->is_disguised && stats->total_pkts < 50) {
                    if (stats->large_pkts > Config::PUNISH_TRIGGER_COUNT) stats->is_disguised = true;
                }
                if (stats->is_disguised) return Net::Priority::Normal;
            }

            ++self->pkt_count;

            // Periodic cleanup: expire flows not seen in 3 × CLEANUP_INTERVAL packets
            if (++self->process_counter > Config::CLEANUP_INTERVAL_PKTS) {
                self->flows.cleanup(self->pkt_count, Config::CLEANUP_INTERVAL_PKTS * 3);
                self->process_counter = 0;
            }

            if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
            return parsed.raw_span.size() < 256 ? Net::Priority::High : Net::Priority::Normal;
        }

        // TCP protocol identification
        static Net::Priority handle_tcp(HeuristicProcessor*, const Net::ParsedPacket& parsed) {
            if (parsed.raw_span.size() < 74) return Net::Priority::Critical; // Prioritize small packets (SYN/ACK)
            auto tcp = parsed.tcp();
            if (tcp) {
                uint16_t dport = net16_to_host(tcp->dest);
                uint16_t sport = net16_to_host(tcp->source);
                if (Config::is_game_port(dport) || Config::is_game_port(sport)) return Net::Priority::High;
            }
            return Net::Priority::Normal;
        }

        static Net::Priority handle_default(HeuristicProcessor*, const Net::ParsedPacket&) {
            return Net::Priority::Normal;
        }

    public:
        HeuristicProcessor() {
            protocol_handlers.fill(handle_default);
            protocol_handlers[17] = handle_udp;
            protocol_handlers[6] = handle_tcp;
        }

        // Main identification entry point
        Net::Priority process(const Net::ParsedPacket& parsed) {
            if (!parsed.is_valid_ipv4()) return Net::Priority::Normal;
            return protocol_handlers[parsed.l4_protocol](this, parsed);
        }
    };
}


