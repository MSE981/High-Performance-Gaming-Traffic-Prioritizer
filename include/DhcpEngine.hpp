#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    struct DhcpMessage {
        size_t len = 0;
        std::array<uint8_t, 512> data{};
    };

    struct DhcpLease {
        uint8_t      mac[6]{};
        Net::IPv4Net ip{};
        bool         active = false;
        std::chrono::steady_clock::time_point lease_expiry;
    };

    class DhcpEngine {
        Net::SpscRingBuffer<DhcpMessage, 512> request_queue{};

        static constexpr size_t MAX_POOL_SIZE = 253;
        std::array<DhcpLease, MAX_POOL_SIZE> leases{};
        size_t       pool_count    = 0;
        Net::IPv4Net        router_ip{};
        std::chrono::seconds lease_duration{86400};

        void init_pool(Net::IPv4Net start_ip, Net::IPv4Net end_ip);
        void handle_dhcp_request(DhcpMessage& msg, int lan_fd);
        Net::IPv4Net find_or_assign_lease(const uint8_t* mac);
        void commit_lease(const uint8_t* mac, Net::IPv4Net ip);

    public:
        explicit DhcpEngine(const std::string& lan_ip);
        void reconfigure(Net::IPv4Net start_ip, Net::IPv4Net end_ip, std::chrono::seconds lease_duration);
        void intercept_request(const Net::ParsedPacket& pkt);
        void process_background_tasks(int lan_fd);
    };
}
