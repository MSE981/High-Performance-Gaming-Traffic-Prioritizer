#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include "Headers.hpp"

namespace Scalpel::Logic {

    struct DhcpMessage {
        size_t len = 0;
        std::array<uint8_t, 512> data{};
    };

    struct DhcpLease {
        std::array<uint8_t, 6> mac{};
        Net::IPv4Net ip{};
        bool         active = false;
        std::chrono::steady_clock::time_point lease_expiry;
    };

    struct DhcpPoolConfig {
        Net::IPv4Net         pool_start{};
        Net::IPv4Net         pool_end{};
        std::chrono::seconds lease{86400};
    };

    class DhcpEngine {
        Net::SpscRingBuffer<DhcpMessage, 512> request_queue{};

        // Watchdog calls process_background_tasks; cap work per tick for fast return.
        static constexpr unsigned kBackgroundTaskBudget = 32;

        static constexpr size_t MAX_POOL_SIZE = 253;
        std::array<DhcpLease, MAX_POOL_SIZE> leases{};
        size_t       pool_count    = 0;
        Net::IPv4Net        router_ip{};
        std::chrono::seconds lease_duration{86400};

        std::expected<void, std::string> init_pool(Net::IPv4Net start_ip, Net::IPv4Net end_ip);
        void handle_dhcp_request(DhcpMessage& msg, int lan_fd);
        Net::IPv4Net find_or_assign_lease(std::span<const uint8_t, 6> mac);
        void commit_lease(Net::IPv4Net ip);

    public:
        explicit DhcpEngine(const std::string& lan_ip, DhcpPoolConfig cfg);
        std::expected<void, std::string> reconfigure(DhcpPoolConfig cfg);
        void intercept_request(const Net::ParsedPacket& pkt);
        void process_background_tasks(int lan_fd);
    };
}
