#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <array>
#include <print>
#include <format>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

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

    // atoul helper (avoids std::stoul heap overhead at call site)
    inline unsigned long atoul(const char* s) {
        unsigned long v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + static_cast<unsigned long>(*s++ - '0');
        return v;
    }

    // Parse a single key=value line into key/val buffers. Returns false if malformed.
    inline bool parse_kv(const char* line, size_t len,
                         char* key, size_t key_cap,
                         char* val, size_t val_cap) {
        const char* eq = static_cast<const char*>(memchr(line, '=', len));
        if (!eq) return false;
        size_t klen = static_cast<size_t>(eq - line);
        size_t vlen = len - klen - 1;
        if (klen == 0 || klen >= key_cap || vlen >= val_cap) return false;
        memcpy(key, line, klen); key[klen] = '\0';
        memcpy(val, eq + 1, vlen); val[vlen] = '\0';
        return true;
    }

    // Load system dynamic config from config.txt (raw fd, no ifstream)
    inline void load_config(const std::string& path = "config.txt") {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::println(stderr, "[Config] Warning: cannot open config file {}, using defaults.", path);
            return;
        }

        // Read entire file into a local buffer (config files are small, < 8 KB)
        char buf[8192]{};
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) return;
        buf[n] = '\0';

        bool bridge_iface_loaded = false;
        char key[64]{}, val[256]{};

        const char* p   = buf;
        const char* end = buf + n;
        while (p < end) {
            // Find end of line
            const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
            size_t llen = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);

            // Strip trailing \r
            if (llen > 0 && p[llen - 1] == '\r') --llen;

            if (llen > 0 && p[0] != '#' && parse_kv(p, llen, key, sizeof(key), val, sizeof(val))) {
                try {
                    if      (!strcmp(key, "IFACE_WAN"))  IFACE_WAN  = val;
                    else if (!strcmp(key, "IFACE_LAN"))  IFACE_LAN  = val;
                    else if (!strcmp(key, "ROUTER_IP"))  ROUTER_IP  = val;
                    else if (!strcmp(key, "ENABLE_ACCELERATION")) ENABLE_ACCELERATION = (!strcmp(val, "true") || !strcmp(val, "1"));
                    else if (!strcmp(key, "ENABLE_STP"))          ENABLE_STP.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "ENABLE_IGMP_SNOOPING")) ENABLE_IGMP_SNOOPING.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "BRIDGE_IFACE")) {
                        if (!bridge_iface_loaded) { clear_bridged(); bridge_iface_loaded = true; }
                        add_bridged(val);
                    }
                    else if (!strcmp(key, "LARGE_PACKET_THRESHOLD")) LARGE_PACKET_THRESHOLD = static_cast<uint32_t>(atoul(val));
                    else if (!strcmp(key, "PUNISH_TRIGGER_COUNT"))   PUNISH_TRIGGER_COUNT   = static_cast<uint32_t>(atoul(val));
                    else if (!strcmp(key, "CLEANUP_INTERVAL"))       CLEANUP_INTERVAL       = static_cast<uint32_t>(atoul(val));
                    else if (!strcmp(key, "enable_gui"))        global_state.enable_gui.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_nat"))        global_state.enable_nat.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_dhcp"))       global_state.enable_dhcp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_dns_cache"))  global_state.enable_dns_cache.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_upnp"))       global_state.enable_upnp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_firewall"))   global_state.enable_firewall.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "enable_pppoe"))      global_state.enable_pppoe.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                    else if (!strcmp(key, "IFACE_GATEWAY")) IFACE_GATEWAY = val;
                    else if (!strcmp(key, "IFACE_ROLE")) {
                        char* colon = strchr(val, ':');
                        if (colon) {
                            *colon = '\0';
                            const char* role_str = colon + 1;
                            IfaceRole role = IfaceRole::DISABLED;
                            if      (!strcmp(role_str, "wan"))     role = IfaceRole::WAN;
                            else if (!strcmp(role_str, "lan"))     role = IfaceRole::LAN;
                            else if (!strcmp(role_str, "gateway")) role = IfaceRole::GATEWAY;
                            set_role(val, role);
                        }
                    }
                    else if (!strcmp(key, "IP_LIMIT")) {
                        char* colon = strchr(val, ':');
                        if (colon) {
                            *colon = '\0';
                            uint32_t ip = parse_ip_str(val);
                            double limit = atof(colon + 1);
                            add_ip_limit(ip, limit);
                        }
                    }
                } catch (...) {
                    std::println(stderr, "[Config] Error parsing line: {}={}", key, val);
                }
            }

            p = nl ? nl + 1 : end;
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

    // Save all runtime state to config.txt (raw fd, no ofstream)
    inline void save_config(const std::string& path = "config.txt") {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::println(stderr, "[Config] Error: cannot write config file {}", path);
            return;
        }
        auto b = [](bool v) -> const char* { return v ? "true" : "false"; };

        dprintf(fd, "# Auto-saved on shutdown\n");
        dprintf(fd, "IFACE_WAN=%s\n",            IFACE_WAN.c_str());
        dprintf(fd, "IFACE_LAN=%s\n",            IFACE_LAN.c_str());
        dprintf(fd, "ROUTER_IP=%s\n",            ROUTER_IP.c_str());
        dprintf(fd, "ENABLE_ACCELERATION=%s\n",  b(ENABLE_ACCELERATION.load(std::memory_order_relaxed)));
        dprintf(fd, "ENABLE_STP=%s\n",           b(ENABLE_STP.load(std::memory_order_relaxed)));
        dprintf(fd, "ENABLE_IGMP_SNOOPING=%s\n", b(ENABLE_IGMP_SNOOPING.load(std::memory_order_relaxed)));
        for (size_t i = 0; i < BRIDGED_IFACES_COUNT; ++i)
            dprintf(fd, "BRIDGE_IFACE=%s\n", BRIDGED_INTERFACES[i].name.data());
        dprintf(fd, "LARGE_PACKET_THRESHOLD=%u\n", LARGE_PACKET_THRESHOLD);
        dprintf(fd, "PUNISH_TRIGGER_COUNT=%u\n",   PUNISH_TRIGGER_COUNT);
        dprintf(fd, "CLEANUP_INTERVAL=%u\n",       CLEANUP_INTERVAL);
        dprintf(fd, "enable_gui=%s\n",        b(global_state.enable_gui.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_nat=%s\n",        b(global_state.enable_nat.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_dhcp=%s\n",       b(global_state.enable_dhcp.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_dns_cache=%s\n",  b(global_state.enable_dns_cache.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_upnp=%s\n",       b(global_state.enable_upnp.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_firewall=%s\n",   b(global_state.enable_firewall.load(std::memory_order_relaxed)));
        dprintf(fd, "enable_pppoe=%s\n",      b(global_state.enable_pppoe.load(std::memory_order_relaxed)));
        for (size_t i = 0; i < IP_LIMIT_COUNT; ++i) {
            uint32_t ip = IP_LIMIT_TABLE[i].ip;
            dprintf(fd, "IP_LIMIT=%u.%u.%u.%u:%.6g\n",
                ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
                IP_LIMIT_TABLE[i].rate);
        }
        dprintf(fd, "IFACE_GATEWAY=%s\n", IFACE_GATEWAY.c_str());
        for (size_t i = 0; i < IFACE_ROLES_COUNT; ++i) {
            const char* rs;
            switch (IFACE_ROLES[i].role) {
                case IfaceRole::GATEWAY:  rs = "gateway";  break;
                case IfaceRole::LAN:      rs = "lan";      break;
                case IfaceRole::WAN:      rs = "wan";      break;
                default:                  rs = "disabled"; break;
            }
            dprintf(fd, "IFACE_ROLE=%s:%s\n", IFACE_ROLES[i].name.data(), rs);
        }
        ::close(fd);
        std::println("[Config] Config saved: {}", path);
    }
}
