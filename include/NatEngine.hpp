#pragma once
#include <span>
#include <array>
#include <atomic>
#include <cstdint>
#include "Headers_util.hpp"
#include "Processor.hpp"

namespace HPGTP::Engine::Nat {
    class NatEngine {
        struct alignas(64) NatSession {
            std::atomic<uint32_t> seq{0};
            Logic::FlowKey internal_key;
            uint16_t external_port = 0;
            std::atomic<uint32_t> last_active_tick{0};
            std::atomic<bool> active{false};
        };

        static constexpr size_t MAX_SESSIONS = 65536;
        static constexpr size_t MAX_ICMP_SESSIONS = 4096;

        struct alignas(64) IcmpEchoSession {
            std::atomic<uint32_t> seq{0};
            Utils::Net::IPv4Net int_saddr{};
            Utils::Net::IPv4Net remote_daddr{};
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

        uint32_t hash_flow(const Logic::FlowKey& k) const;
        uint32_t hash_icmp_flow(Utils::Net::IPv4Net sa, Utils::Net::IPv4Net da, uint16_t id_nbo) const;
        uint16_t alloc_external_icmp_id() noexcept;
        bool     process_outbound_icmp(Utils::Net::ParsedPacket& pkt);
        bool     process_inbound_icmp(Utils::Net::ParsedPacket& pkt);

    public:
        explicit NatEngine();
        void set_wan_ip(Utils::Net::IPv4Net ip);
        [[nodiscard]] Utils::Net::IPv4Net wan_ip_snapshot() const noexcept {
            return Utils::Net::IPv4Net{wan_ip_nbo.load(std::memory_order_acquire)};
        }
        void tick();
        bool process_outbound(Utils::Net::ParsedPacket& pkt);
        bool process_inbound(Utils::Net::ParsedPacket& pkt);
    };
} // namespace HPGTP::Engine::Nat
