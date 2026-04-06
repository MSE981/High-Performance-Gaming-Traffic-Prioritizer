#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <array>
#include <map>
#include <vector>
#include <print>
#include <format>
#include <ranges>
#include <atomic>

namespace Scalpel::Config {
    // Per-interface role assignment
    enum class IfaceRole : uint8_t { WAN = 0, LAN = 1, GATEWAY = 2, DISABLED = 3 };
    inline std::map<std::string, IfaceRole> IFACE_ROLES;  // populated by GUI or loaded from config
    inline std::string IFACE_GATEWAY = "eth0";            // primary uplink (default gateway interface)

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
            return (a << 24) | (b << 16) | (c << 8) | d;
        }
        return 0;
    }

    // Helper: convert network-order uint32 to dotted-decimal string
    inline std::string ip_to_str(uint32_t ip) {
        return std::format("{}.{}.{}.{}",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    // Load system dynamic config from config.txt
    inline void load_config(const std::string& path = "config.txt") {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::println(stderr, "[Config] Warning: cannot open config file {}, using defaults.", path);
            return;
        }

        IFACE_ROLES.clear();
IP_LIMIT_MAP.clear();
BRIDGED_INTERFACES = {"eth1", "eth2", "eth3"}
        bool bridge_iface_loaded = false;
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
                else if (key == "ROUTER_IP") ROUTER_IP = val;
                else if (key == "ENABLE_ACCELERATION") ENABLE_ACCELERATION = (val == "true" || val == "1");
                else if (key == "ENABLE_STP") ENABLE_STP.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "ENABLE_IGMP_SNOOPING") ENABLE_IGMP_SNOOPING.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "BRIDGE_IFACE") {
                    if (!bridge_iface_loaded) { BRIDGED_INTERFACES.clear(); bridge_iface_loaded = true; }
                    BRIDGED_INTERFACES.push_back(val);
                }
                else if (key == "LARGE_PACKET_THRESHOLD") LARGE_PACKET_THRESHOLD = std::stoul(val);
                else if (key == "PUNISH_TRIGGER_COUNT") PUNISH_TRIGGER_COUNT = std::stoul(val);
                else if (key == "CLEANUP_INTERVAL") CLEANUP_INTERVAL = std::stoul(val);
                else if (key == "enable_gui") global_state.enable_gui.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_nat") global_state.enable_nat.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_dhcp") global_state.enable_dhcp.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_dns_cache") global_state.enable_dns_cache.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_upnp") global_state.enable_upnp.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_firewall") global_state.enable_firewall.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "enable_pppoe") global_state.enable_pppoe.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "IFACE_GATEWAY") IFACE_GATEWAY = val;
                else if (key == "IFACE_ROLE") {
                    auto colon = val.find(':');
                    if (colon != std::string::npos) {
                        std::string iface_name = val.substr(0, colon);
                        std::string role_str   = val.substr(colon + 1);
                        IfaceRole role = IfaceRole::DISABLED;
                        if      (role_str == "wan")     role = IfaceRole::WAN;
                        else if (role_str == "lan")     role = IfaceRole::LAN;
                        else if (role_str == "gateway") role = IfaceRole::GATEWAY;
                        IFACE_ROLES[iface_name] = role;
                    }
                }
                else if (key == "IP_LIMIT") {
                    auto dash = val.find(':');
                    if (dash != std::string::npos) {
                        uint32_t ip = parse_ip_str(val.substr(0, dash));
                double limit = std::stod(val.substr(dash + 1));
                if (ip == 0) {
                std::println(stderr, "[Config] Invalid IP in line: {}", line);
                continue;
                }
                if (limit <= 0.0) {
                std::println(stderr, "[Config] Invalid limit in line: {}", line);
                continue;
                }
IP_LIMIT_MAP[ip] = limit;
                    }
                }
           } catch (const std::exception& e) {
    std::println(stderr, "[Config] Error parsing line '{}': {}", line, e.what());
}
        }
        // If role map was loaded, derive legacy variables from it (overrides direct IFACE_WAN/LAN keys)
        if (!IFACE_ROLES.empty()) {
            bool bridge_from_roles = false;
            for (const auto& [name, role] : IFACE_ROLES) {
                if (role == IfaceRole::GATEWAY) {
                    IFACE_GATEWAY = name;
                    IFACE_WAN = name;
                } else if (role == IfaceRole::LAN) {
                    if (!bridge_from_roles) { BRIDGED_INTERFACES.clear(); bridge_from_roles = true; }
                    BRIDGED_INTERFACES.push_back(name);
                }
            }
            if (!BRIDGED_INTERFACES.empty()) IFACE_LAN = BRIDGED_INTERFACES[0];
        }
        std::println("[Config] Config loaded: {}", path);
    }

    // Save all runtime state to config.txt (called on program shutdown)
    inline void save_config(const std::string& path = "config.txt") {
        std::ofstream file(path);
        if (!file.is_open()) {
            std::println(stderr, "[Config] Error: cannot write config file {}", path);
            return;
        }
        auto b = [](bool v) { return v ? "true" : "false"; };

        file << "# Auto-saved on shutdown\n";
        file << "IFACE_WAN=" << IFACE_WAN << "\n";
        file << "IFACE_LAN=" << IFACE_LAN << "\n";
        file << "ROUTER_IP=" << ROUTER_IP << "\n";
        file << "ENABLE_ACCELERATION=" << b(ENABLE_ACCELERATION.load()) << "\n";
        file << "ENABLE_STP=" << b(ENABLE_STP.load()) << "\n";
        file << "ENABLE_IGMP_SNOOPING=" << b(ENABLE_IGMP_SNOOPING.load()) << "\n";
        for (const auto& iface : BRIDGED_INTERFACES)
            file << "BRIDGE_IFACE=" << iface << "\n";
        file << "LARGE_PACKET_THRESHOLD=" << LARGE_PACKET_THRESHOLD << "\n";
        file << "PUNISH_TRIGGER_COUNT=" << PUNISH_TRIGGER_COUNT << "\n";
        file << "CLEANUP_INTERVAL=" << CLEANUP_INTERVAL << "\n";
        file << "enable_gui=" << b(global_state.enable_gui.load()) << "\n";
        file << "enable_nat=" << b(global_state.enable_nat.load()) << "\n";
        file << "enable_dhcp=" << b(global_state.enable_dhcp.load()) << "\n";
        file << "enable_dns_cache=" << b(global_state.enable_dns_cache.load()) << "\n";
        file << "enable_upnp=" << b(global_state.enable_upnp.load()) << "\n";
        file << "enable_firewall=" << b(global_state.enable_firewall.load()) << "\n";
        file << "enable_pppoe=" << b(global_state.enable_pppoe.load()) << "\n";
        for (const auto& [ip, rate] : IP_LIMIT_MAP)
            file << "IP_LIMIT=" << ip_to_str(ip) << ":" << rate << "\n";
        file << "IFACE_GATEWAY=" << IFACE_GATEWAY << "\n";
        for (const auto& [name, role] : IFACE_ROLES) {
            std::string_view rs;
            switch (role) {
                case IfaceRole::GATEWAY:  rs = "gateway"; break;
                case IfaceRole::LAN:      rs = "lan";     break;
                case IfaceRole::WAN:      rs = "wan";     break;
                default:                  rs = "disabled"; break;
            }
            file << "IFACE_ROLE=" << name << ":" << rs << "\n";
        }

        std::println("[Config] Config saved: {}", path);
    }
}


