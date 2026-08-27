#pragma once
#include <string>
#include <string_view>
#include <expected>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <format>
#include <cstring>
#include <arpa/inet.h>
#include "Types_util.hpp"
#include "Units_util.hpp"

namespace HPGTP::Config {
    // Kernel interface names ，private
    const std::string& iface_wan();
    const std::string& iface_lan();

    // Set to false by GUI shutdown dialog ，skip save
    inline std::atomic<bool> SAVE_ON_EXIT{true};

    // Dynamic runtime switch states
    struct DynamicState {
        std::atomic<bool> enable_nat{true};
        std::atomic<bool> enable_dhcp{true};
    };
    inline DynamicState global_state;

    // Interface configuration
    inline std::string ROUTER_IP = "192.168.12.1";
    inline bool APPLY_ROUTER_IP_TO_LAN = true;
    inline int  LAN_PREFIX_LEN         = 24;

    // DHCP pool configuration
    inline std::string DHCP_POOL_START = "192.168.12.50";
    inline std::string DHCP_POOL_END   = "192.168.12.255";
    inline std::chrono::seconds DHCP_LEASE_DURATION{86400};

    inline std::atomic<bool> ENABLE_ACCELERATION{true};

    // Traffic classification， High and Normal lanes.
    inline uint32_t LARGE_PACKET_THRESHOLD_BYTES = 1000;

    // Dotted-decimal IPv4 only; rejects invalid octets and non-canonical forms per inet_pton(3).
    inline std::expected<Net::IPv4Net, std::string> parse_ip_str(std::string_view ip_sv) {
        if (ip_sv.empty()) return std::unexpected(std::string("empty IPv4 string"));
        if (ip_sv.size() > 15u)
            return std::unexpected(std::string("IPv4 string too long"));
        char buf[16]{};
        std::memcpy(buf, ip_sv.data(), ip_sv.size());
        buf[ip_sv.size()] = '\0';
        struct ::in_addr a {};
        if (::inet_pton(AF_INET, buf, &a) != 1)
            return std::unexpected(std::string("invalid IPv4 address"));
        return Net::IPv4Net{a.s_addr};
    }

    // convert NBO IPv4Net to dotted-decimal string
    inline std::string ip_to_str(Net::IPv4Net ip) {
        uint32_t v = ip.raw();
        return std::format("{}.{}.{}.{}",
            v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    }

    // Load / save - implementations in Config.cpp
    std::expected<void, std::string> load_config(const std::string& path = "config/config.txt");
    std::expected<void, std::string> save_config(const std::string& path = "config/config.txt");
}
