#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include "Headers_util.hpp"
#include "Config.hpp"

namespace HPGTP::Logic {

    constexpr inline uint16_t net16_to_host(uint16_t be) noexcept {
        return static_cast<uint16_t>((be << 8) | (be >> 8));
    }

    // 5-tuple flow identifier.
    struct FlowKey {
        Net::IPv4Net saddr, daddr;   // NBO - matched directly against IPv4Header fields
        uint16_t     sport = 0;
        uint16_t     dport = 0;

        constexpr FlowKey() noexcept : saddr(), daddr(), sport(0), dport(0) {}
        constexpr FlowKey(Net::IPv4Net sa, Net::IPv4Net da, uint16_t sp, uint16_t dp) noexcept
            : saddr(sa), daddr(da), sport(sp), dport(dp) {}

        bool operator==(const FlowKey&) const = default;
    };

    // Heuristic traffic identification engine: two-tier classification.
    // ICMP, DNS and small packets go to High; large packets are Normal.
    class HeuristicProcessor {
        using ProtocolHandler =
            Net::Priority (*)(HeuristicProcessor*, const Net::ParsedPacket&);
        std::array<ProtocolHandler, 256> protocol_handlers;

        static Net::Priority handle_udp(HeuristicProcessor*, const Net::ParsedPacket& parsed) {
            auto udp = parsed.udp();
            if (!udp) return Net::Priority::Normal;
            const uint16_t dport = net16_to_host(udp->dest);
            const uint16_t sport = net16_to_host(udp->source);
            // DNS --> High priority
            if (dport == 53 || sport == 53) return Net::Priority::High;
            return parsed.raw_span.size() < Config::LARGE_PACKET_THRESHOLD_BYTES
                ? Net::Priority::High : Net::Priority::Normal;
        }

        static Net::Priority handle_tcp(HeuristicProcessor*, const Net::ParsedPacket& parsed) {
            return parsed.raw_span.size() < Config::LARGE_PACKET_THRESHOLD_BYTES
                ? Net::Priority::High : Net::Priority::Normal;
        }

        // ICMP --> High priority
        static Net::Priority handle_icmp(HeuristicProcessor*, const Net::ParsedPacket&) {
            return Net::Priority::High;
        }

        static Net::Priority handle_default(HeuristicProcessor*, const Net::ParsedPacket& parsed) {
            return parsed.raw_span.size() < Config::LARGE_PACKET_THRESHOLD_BYTES
                ? Net::Priority::High : Net::Priority::Normal;
        }

    public:
        HeuristicProcessor() {
            protocol_handlers.fill(handle_default);
            protocol_handlers[17] = handle_udp;
            protocol_handlers[6]  = handle_tcp;
            protocol_handlers[1]  = handle_icmp;
        }

        Net::Priority process(const Net::ParsedPacket& parsed) {
            if (!parsed.is_valid_ipv4()) return Net::Priority::Normal;
            return protocol_handlers[parsed.l4_protocol](this, parsed);
        }
    };
}
