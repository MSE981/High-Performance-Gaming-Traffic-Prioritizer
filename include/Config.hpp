#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <array>
#include <print>
#include <format>
#include <cstring>

namespace Scalpel::Config {
    // Per-interface role assignment
    enum class IfaceRole : uint8_t { WAN = 0, LAN = 1, GATEWAY = 2, DISABLED = 3 };

    // Static interface role table (max 8 interfaces, zero heap allocation)
    static constexpr size_t MAX_IFACES = 8;
    struct IfaceRoleEntry {
        std::array<char, 16> name{};
        IfaceRole role = IfaceRole::DISABLED;
    };
    inline std::array<IfaceRoleEntry, MAX_IFACES> IFACE_ROLES{};
    inline size_t IFACE_ROLES_COUNT = 0;

    // Lookup helper: find role for interface name, returns DISABLED if not found
    inline IfaceRole find_role(const std::string& name) {
        for (size_t i = 0; i < IFACE_ROLES_COUNT; ++i)
            if (name == IFACE_ROLES[i].name.data()) return IFACE_ROLES[i].role;
        return IfaceRole::DISABLED;
    }

    // Insert or update helper
    inline void set_role(const std::string& name, IfaceRole role) {
        for (size_t i = 0; i < IFACE_ROLES_COUNT; ++i) {
            if (name == IFACE_ROLES[i].name.data()) {
                IFACE_ROLES[i].role = role;
                return;
            }
        }
        if (IFACE_ROLES_COUNT < MAX_IFACES) {
            strncpy(IFACE_ROLES[IFACE_ROLES_COUNT].name.data(), name.c_str(), 15);
            IFACE_ROLES[IFACE_ROLES_COUNT].name[15] = '\0';
            IFACE_ROLES[IFACE_ROLES_COUNT].role = role;
            ++IFACE_ROLES_COUNT;
        }
    }

    inline void clear_roles() { IFACE_ROLES_COUNT = 0; }

    inline std::string IFACE_GATEWAY = "eth0";

    // Dynamic runtime switch states
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

    // Interface configuration
    inline std::string IFACE_WAN = "eth0";
    inline std::string IFACE_LAN = "eth1";
    inline std::string ROUTER_IP = "192.168.1.100";
    inline std::atomic<bool> ENABLE_ACCELERATION{true};

    // Bridge depth configuration
    inline std::atomic<bool> ENABLE_STP{false};
    inline std::atomic<bool> ENABLE_IGMP_SNOOPING{false};

    // Static bridged interfaces list (max 8 entries, zero heap allocation)
    struct BridgedIfaceEntry { std::array<char, 16> name{}; };
    inline std::array<BridgedIfaceEntry, MAX_IFACES> BRIDGED_INTERFACES{};
    inline size_t BRIDGED_IFACES_COUNT = 0;

    inline void clear_bridged() { BRIDGED_IFACES_COUNT = 0; }
    inline void add_bridged(const std::string& name) {
        if (BRIDGED_IFACES_COUNT < MAX_IFACES) {
            strncpy(BRIDGED_INTERFACES[BRIDGED_IFACES_COUNT].name.data(), name.c_str(), 15);
            BRIDGED_INTERFACES[BRIDGED_IFACES_COUNT].name[15] = '\0';
            ++BRIDGED_IFACES_COUNT;
        }
    }

    // Heuristic detection algorithm thresholds
    inline uint32_t LARGE_PACKET_THRESHOLD = 1000;
    inline uint32_t PUNISH_TRIGGER_COUNT = 30;
    inline uint32_t CLEANUP_INTERVAL = 10000;

    // Gaming protocol port whitelist
    struct PortRange { uint16_t start; uint16_t end; };
    inline std::array<PortRange, 3> GAME_PORTS = {{ {3074, 3074}, {27015, 27015}, {12000, 12999} }};

    // Static IP rate limit table (max 256 entries, zero heap allocation)
    static constexpr size_t MAX_IP_LIMITS = 256;
    struct IpLimitEntry {
        uint32_t ip = 0;
        double   rate = 0.0;
    };
    inline std::array<IpLimitEntry, MAX_IP_LIMITS> IP_LIMIT_TABLE{};
    inline size_t IP_LIMIT_COUNT = 0;
    inline std::atomic<bool> IP_LIMIT_ACTIVE{false};

    inline void clear_ip_limits() { IP_LIMIT_COUNT = 0; }
    inline void add_ip_limit(uint32_t ip, double rate) {
        // Update existing entry if found
        for (size_t i = 0; i < IP_LIMIT_COUNT; ++i) {
            if (IP_LIMIT_TABLE[i].ip == ip) { IP_LIMIT_TABLE[i].rate = rate; return; }
        }
        if (IP_LIMIT_COUNT < MAX_IP_LIMITS) {
            IP_LIMIT_TABLE[IP_LIMIT_COUNT] = {ip, rate};
            ++IP_LIMIT_COUNT;
        }
    }

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
            return (a << 0) | (b << 8) | (c << 16) | (d << 24);
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
                    if (!bridge_iface_loaded) { clear_bridged(); bridge_iface_loaded = true; }
                    add_bridged(val);
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
                        set_role(iface_name, role);
                    }
                }
                else if (key == "IP_LIMIT") {
                    auto dash = val.find(':');
                    if (dash != std::string::npos) {
                        uint32_t ip = parse_ip_str(val.substr(0, dash));
                        double limit = std::stod(val.substr(dash + 1));
                        add_ip_limit(ip, limit);
                    }
                }
            } catch (...) {
                std::println(stderr, "[Config] Error: cannot parse config line: {}", line);
            }
        }
        // If role table was loaded, derive legacy variables
        if (IFACE_ROLES_COUNT > 0) {
            bool bridge_from_roles = false;
            for (size_t i = 0; i < IFACE_ROLES_COUNT; ++i) {
                if (IFACE_ROLES[i].role == IfaceRole::GATEWAY) {
                    IFACE_GATEWAY = IFACE_ROLES[i].name.data();
                    IFACE_WAN = IFACE_ROLES[i].name.data();
                } else if (IFACE_ROLES[i].role == IfaceRole::LAN) {
                    if (!bridge_from_roles) { clear_bridged(); bridge_from_roles = true; }
                    add_bridged(IFACE_ROLES[i].name.data());
                }
            }
            if (BRIDGED_IFACES_COUNT > 0) IFACE_LAN = BRIDGED_INTERFACES[0].name.data();
        }
        IP_LIMIT_ACTIVE.store(IP_LIMIT_COUNT > 0, std::memory_order_release);
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
        file << "ENABLE_ACCELERATION=" << b(ENABLE_ACCELERATION.load(std::memory_order_relaxed)) << "\n";
        file << "ENABLE_STP=" << b(ENABLE_STP.load(std::memory_order_relaxed)) << "\n";
        file << "ENABLE_IGMP_SNOOPING=" << b(ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed)) << "\n";
        for (size_t i = 0; i < BRIDGED_IFACES_COUNT; ++i)
            file << "BRIDGE_IFACE=" << BRIDGED_INTERFACES[i].name.data() << "\n";
        file << "LARGE_PACKET_THRESHOLD=" << LARGE_PACKET_THRESHOLD << "\n";
        file << "PUNISH_TRIGGER_COUNT=" << PUNISH_TRIGGER_COUNT << "\n";
        file << "CLEANUP_INTERVAL=" << CLEANUP_INTERVAL << "\n";
        file << "enable_gui=" << b(global_state.enable_gui.load(std::memory_order_relaxed)) << "\n";
        file << "enable_nat=" << b(global_state.enable_nat.load(std::memory_order_relaxed)) << "\n";
        file << "enable_dhcp=" << b(global_state.enable_dhcp.load(std::memory_order_relaxed)) << "\n";
        file << "enable_dns_cache=" << b(global_state.enable_dns_cache.load(std::memory_order_relaxed)) << "\n";
        file << "enable_upnp=" << b(global_state.enable_upnp.load(std::memory_order_relaxed)) << "\n";
        file << "enable_firewall=" << b(global_state.enable_firewall.load(std::memory_order_relaxed)) << "\n";
        file << "enable_pppoe=" << b(global_state.enable_pppoe.load(std::memory_order_relaxed)) << "\n";
        for (size_t i = 0; i < IP_LIMIT_COUNT; ++i)
            file << "IP_LIMIT=" << ip_to_str(IP_LIMIT_TABLE[i].ip) << ":" << IP_LIMIT_TABLE[i].rate << "\n";
        file << "IFACE_GATEWAY=" << IFACE_GATEWAY << "\n";
        for (size_t i = 0; i < IFACE_ROLES_COUNT; ++i) {
            std::string_view rs;
            switch (IFACE_ROLES[i].role) {
                case IfaceRole::GATEWAY:  rs = "gateway"; break;
                case IfaceRole::LAN:      rs = "lan";     break;
                case IfaceRole::WAN:      rs = "wan";     break;
                default:                  rs = "disabled"; break;
            }
            file << "IFACE_ROLE=" << IFACE_ROLES[i].name.data() << ":" << rs << "\n";
        }

        std::println("[Config] Config saved: {}", path);
    }
}
