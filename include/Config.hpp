#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <array>
#include <map>
#include <vector>
#include <print>  
#include <ranges> 

namespace Scalpel::Config {
    // Dynamic runtime switch states (follow RCU/lock barrier concept for data plane O(1) feature state access)
    struct DynamicState {
        std::atomic<bool> enable_nat{true};
        std::atomic<bool> enable_dhcp{true};
        std::atomic<bool> enable_dns_cache{true};
        std::atomic<bool> enable_upnp{true};
        std::atomic<bool> enable_firewall{true};
        std::atomic<bool> enable_pppoe{false};
        std::atomic<bool> enable_gui{true};
    };
    inline DynamicState global_state;

    // Interface configuration (runtime override from web or config file)
    inline std::string IFACE_WAN = "eth0";
    inline std::string IFACE_LAN = "eth1"; // LAN / USB NIC interface
    inline std::string ROUTER_IP = "192.168.1.100"; // Router's own LAN IP
    inline std::atomic<bool> ENABLE_ACCELERATION{true}; // Acceleration / transparent bridge toggle

    // Bridge depth configuration
    inline std::atomic<bool> ENABLE_STP{false};
    inline std::atomic<bool> ENABLE_IGMP_SNOOPING{false};
    inline std::vector<std::string> BRIDGED_INTERFACES = {"eth1", "eth2", "eth3"};

    // Heuristic detection algorithm thresholds
    inline uint32_t LARGE_PACKET_THRESHOLD = 1000;
    inline uint32_t PUNISH_TRIGGER_COUNT = 30;
    inline uint32_t CLEANUP_INTERVAL = 10000;

    // Gaming protocol port whitelist (defaults)
    struct PortRange { uint16_t start; uint16_t end; };
    inline std::array<PortRange, 3> GAME_PORTS = {{ {3074, 3074}, {27015, 27015}, {12000, 12999} }};

    // Software router feature: per-device IP rate limit table
    inline std::map<uint32_t, double> IP_LIMIT_MAP;

    // Helper: check if port is a game port
    inline bool is_game_port(uint16_t port) {
        for (const auto& range : GAME_PORTS) {
            if (port >= range.start && port <= range.end) return true;
        }
        return false;
    }

    // Helper: parse string IP (A.B.C.D) to network-order uint32
    inline uint32_t parse_ip_str(const std::string& ip_str) {
        uint32_t a, b, c, d;
        if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
             return (a << 0) | (b << 8) | (c << 16) | (d << 24); // Simple endian conversion depends on architecture
        }
        return 0;
    }

    // Load system dynamic config from config.txt
    inline void load_config(const std::string& path = "config.txt") {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::println(stderr, "[Config] Warning: cannot open config file {}, using defaults.", path);
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line.starts_with('#')) continue;

            auto delim = line.find('=');
            if (delim == std::string::npos) continue;

            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);

            try {
                if (key == "IFACE_WAN") IFACE_WAN = val;
                else if (key == "IFACE_LAN") IFACE_LAN = val;
                else if (key == "ENABLE_ACCELERATION") ENABLE_ACCELERATION = (val == "true" || val == "1");
                else if (key == "LARGE_PACKET_THRESHOLD") LARGE_PACKET_THRESHOLD = std::stoul(val);
                else if (key == "PUNISH_TRIGGER_COUNT") PUNISH_TRIGGER_COUNT = std::stoul(val);
                else if (key == "CLEANUP_INTERVAL") CLEANUP_INTERVAL = std::stoul(val);
                else if (key == "enable_gui") global_state.enable_gui.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "IP_LIMIT") {
                    auto dash = val.find(':');
                    if (dash != std::string::npos) {
                        uint32_t ip = parse_ip_str(val.substr(0, dash));
                        double limit = std::stod(val.substr(dash + 1));
                        IP_LIMIT_MAP[ip] = limit;
                    }
                }
            } catch (...) {
                std::println(stderr, "[Config] Error: cannot parse config line: {}", line);
            }
        }
        std::println("[Config] 配置文件加载完成: {}", path);
    }
}


