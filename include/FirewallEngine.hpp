#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    enum class ConnState : uint8_t {
        SYN_SENT    = 0,
        ESTABLISHED = 1,
        FIN_WAIT    = 2,
    };

    class FirewallEngine {
        struct alignas(64) ConnTrackEntry {
            uint32_t  remote_ip   = 0;
            uint16_t  remote_port = 0;
            uint32_t  lan_ip      = 0;
            uint16_t  lan_port    = 0;
            uint8_t   protocol    = 0;
            std::atomic<ConnState> state{ConnState::SYN_SENT};
            std::atomic<bool>     active{false};
            std::atomic<uint32_t> last_tick{0};
        };

        static constexpr size_t   TABLE_SIZE          = 65536;
        static constexpr size_t   PROBE_LIMIT         = 64;
        static constexpr uint32_t TIMEOUT_SYN_SENT    = 30;
        static constexpr uint32_t TIMEOUT_ESTABLISHED = 300;
        static constexpr uint32_t TIMEOUT_FIN_WAIT    = 30;

        std::array<ConnTrackEntry, TABLE_SIZE> table{};
        std::atomic<uint32_t> current_tick{0};

        static constexpr size_t MAX_BLOCKED = 64;
        std::array<Net::IPv4Net, MAX_BLOCKED> blocked_ips;
        std::atomic<uint8_t> blocked_count{0};

        static uint32_t hash_remote(uint32_t remote_ip, uint16_t remote_port, uint8_t proto);
        static uint32_t timeout_for(ConnState s);
        bool is_expired(const ConnTrackEntry& e) const;

    public:
        void tick();
        void sync_blocked_ips();
        bool is_blocked_ip(Net::IPv4Net ip) const;
        void track_outbound(const Net::ParsedPacket& pkt);
        bool check_inbound(const Net::ParsedPacket& pkt);
        void cleanup();
    };
}
