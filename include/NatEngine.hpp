#pragma once
#include <span>
#include <array>
#include <atomic>
#include <cstdint>
#include "Headers.hpp"
#include "Processor.hpp"

namespace HPGTP::Logic {
    // True zero-copy user-space NAT engine
    class NatEngine {
        struct alignas(64) NatSession {
            std::atomic<uint32_t> seq{0};
            FlowKey internal_key;
            uint16_t external_port = 0;
            std::atomic<uint32_t> last_active_tick{0};
            std::atomic<bool> active{false};
        };

        static constexpr size_t MAX_SESSIONS = 65536;
        static constexpr size_t MAX_ICMP_SESSIONS = 4096;

        struct alignas(64) IcmpEchoSession {
            std::atomic<uint32_t> seq{0};
            Net::IPv4Net int_saddr{};
            Net::IPv4Net remote_daddr{};
            uint16_t     int_id_nbo  = 0;
            uint16_t     ext_id_nbo  = 0;
            std::atomic<uint32_t> last_active_tick{0};
            std::atomic<bool>     active{false};
        };

        std::array<NatSession, MAX_SESSIONS> sessions;
        std::array<std::atomic<int32_t>, 65536> port_to_index{};

        std::array<IcmpEchoSession, MAX_ICMP_SESSIONS> icmp_sessions{};
        std::array<std::atomic<int32_t>, 65536>        icmp_id_to_index{};


        uint16_t     port_cursor = 10000;
        uint16_t     icmp_id_cursor = 25000;
        alignas(64) std::atomic<uint32_t> wan_ip_nbo{0};
        std::atomic<uint32_t> current_tick{0};

        uint32_t hash_flow(const FlowKey& k) const;
        uint32_t hash_icmp_flow(Net::IPv4Net sa, Net::IPv4Net da, uint16_t id_nbo) const;
        uint16_t alloc_external_icmp_id() noexcept;
        bool     process_outbound_icmp(Net::ParsedPacket& pkt);
        bool     process_inbound_icmp(Net::ParsedPacket& pkt);

    public:
        explicit NatEngine();
        void set_wan_ip(Net::IPv4Net ip);
        [[nodiscard]] Net::IPv4Net wan_ip_snapshot() const noexcept {
            return Net::IPv4Net{wan_ip_nbo.load(std::memory_order_acquire)};
        }
        void tick();
        bool process_outbound(Net::ParsedPacket& pkt);
        bool process_inbound(Net::ParsedPacket& pkt);
    };
}
