#pragma once
#include <span>
#include <array>
#include <atomic>
#include <cstdint>
#include "Headers.hpp"
#include "Processor.hpp"

namespace Scalpel::Logic {
    // True zero-copy user-space NAT engine
    class NatEngine {
        struct alignas(64) NatSession {
            FlowKey internal_key;
            uint16_t external_port = 0;
            std::atomic<uint32_t> last_active_tick{0};
            std::atomic<bool> active{false};
        };

        struct UpnpMapping {
            Net::IPv4Net internal_ip{};
            uint16_t     internal_port = 0;
            uint16_t     external_port = 0;
            uint8_t      protocol = 0;
            alignas(64) std::atomic<bool> active{false};
        };

        static constexpr size_t MAX_SESSIONS = 65536;

        std::array<NatSession, MAX_SESSIONS> sessions{};
        std::array<int32_t, 65536> port_to_index{};

        std::array<UpnpMapping, 256> upnp_rules{};
        alignas(64) std::atomic<size_t> upnp_cursor{0};

        uint16_t     port_cursor = 10000;
        Net::IPv4Net wan_ip{};
        std::atomic<uint32_t> current_tick{0};

        uint32_t hash_flow(const FlowKey& k) const;

    public:
        struct UpnpRule {
            uint16_t     ext_port  = 0;
            Net::IPv4Net int_ip{};
            uint16_t     int_port  = 0;
            uint8_t      proto     = 0;
        };

        explicit NatEngine();
        void set_wan_ip(Net::IPv4Net ip);
        void add_upnp_rule(UpnpRule rule);
        void tick();
        bool process_outbound(Net::ParsedPacket& pkt);
        bool process_inbound(Net::ParsedPacket& pkt);
    };
}
