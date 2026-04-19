#pragma once
#include <string>
#include <string_view>
#include <expected>
#include <cstdint>
#include <atomic>
#include <array>
#include <mutex>
#include <chrono>
#include <span>
#include <print>
#include <format>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include "NetworkTypes.hpp"
#include "Units.hpp"

namespace HPGTP::Config {
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

    // Kernel interface names (gateway / WAN / LAN). Storage is private to Config.cpp;
    // all updates go through set_iface_names (aggregated multi-field setter).
    struct IfaceNames {
        std::string gateway;
        std::string wan;
        std::string lan;
    };
    void set_iface_names(const IfaceNames& names);
    const std::string& iface_gateway();
    const std::string& iface_wan();
    const std::string& iface_lan();

    // Set to false by GUI shutdown dialog to skip the post-exit config save
    inline std::atomic<bool> SAVE_ON_EXIT{true};

    // Dynamic runtime switch states
    struct DynamicState {
        std::atomic<bool> enable_nat{true};
        std::atomic<bool> enable_dhcp{true};
        // When false: RAM TTL cache is off; upstream redirect and static records still apply.
        std::atomic<bool> enable_dns_cache{true};
        std::atomic<bool> enable_upnp{true};
        std::atomic<bool> enable_firewall{true};
        std::atomic<bool> enable_gui{true};
    };
    inline DynamicState global_state;

    // Interface configuration (ROUTER_IP and below remain inline; IFACE_* live in Config.cpp)
    inline std::string ROUTER_IP = "192.168.12.1";
    // When true, App::init sets IFACE_LAN's kernel IPv4 to ROUTER_IP (userspace DHCP does not do this).
    inline bool APPLY_ROUTER_IP_TO_LAN = true;
    inline int  LAN_PREFIX_LEN         = 24;

    // Optional static IPv4 for NAT (SNAT source / inbound DNAT match). Empty = use WAN iface address from kernel.
    inline std::string WAN_IP{};

    // DHCP pool configuration
    inline std::string DHCP_POOL_START = "192.168.12.50";
    inline std::string DHCP_POOL_END   = "192.168.12.255";
    inline std::chrono::seconds DHCP_LEASE_DURATION{86400};
    // DNS upstream server and redirect configuration
    inline std::string DNS_UPSTREAM_PRIMARY   = "8.8.8.8";
    inline std::string DNS_UPSTREAM_SECONDARY = "8.8.4.4";
    inline std::atomic<bool> DNS_REDIRECT_ENABLED{false};

    // Static DNS records: hostname → IP mappings, bypass cache with no expiry
    static constexpr size_t MAX_STATIC_DNS = 64;
    struct StaticDnsRecord {
        uint32_t      domain_hash = 0;
        Net::IPv4Net  ip{};
        std::array<char, 64> hostname{};
    };
    inline std::mutex static_dns_mutex;
    inline std::array<StaticDnsRecord, MAX_STATIC_DNS> STATIC_DNS_TABLE{};
    inline size_t STATIC_DNS_COUNT = 0;

    // Encode dotted hostname to DNS QNAME wire format and compute FNV-1a hash.
    inline uint32_t dns_hash_hostname(const std::string& hostname) {
        uint8_t qname[256]{};
        size_t qlen = 0;
        const char* p = hostname.c_str();
        while (*p && qlen < 253) {
            size_t lstart = qlen++;
            uint8_t label_len = 0;
            while (*p && *p != '.' && qlen < 255) {
                qname[qlen++] = static_cast<uint8_t>(*p++);
                ++label_len;
            }
            qname[lstart] = label_len;
            if (*p == '.') ++p;
        }
        uint32_t h = 2166136261U;
        for (size_t i = 0; i < qlen; ++i) { h ^= qname[i]; h *= 16777619U; }
        return h;
    }

    inline std::atomic<bool> ENABLE_ACCELERATION{true};

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
    inline uint32_t LARGE_PACKET_THRESHOLD_BYTES = 1000;
    inline uint32_t PUNISH_TRIGGER_COUNT = 30;
    inline uint32_t CLEANUP_INTERVAL_PKTS = 10000;

    // Gaming protocol port whitelist (GUI + config.txt); dataplane reads active buffer only.
    struct PortRange { uint16_t start; uint16_t end; char desc[32]; };
    static constexpr size_t MAX_GAME_PORT_RANGES = 64;
    // Compile-time defaults — overridden by config.txt GAME_PORT lines when present.
    inline std::array<std::array<PortRange, MAX_GAME_PORT_RANGES>, 2> GAME_PORT_TABLE_DOUBLE{{
        {{
            {3478, 3480, "PlayStation Network"},
            {3074, 3074, "Xbox Live"},
            {27015, 27050, "Steam"},
            {5000, 5500, "Riot Games (LoL, Valorant)"},
            {26500, 26510, "Overwatch / Blizzard"},
            {9000, 9100, "Fortnite / Epic"},
            {1024, 1124, "Apex Legends / EA"},
            {3216, 3216, "EA"},
            {9960, 9969, "EA"},
            {18000, 18060, "EA"},
            {3074, 3080, "Call of Duty"},
            {25565, 25565, "Minecraft Java"},
            {19132, 19133, "Minecraft Bedrock"},
            {7000, 9000, "Rocket League"},
            {22101, 22102, "Genshin Impact / HoYoverse"},
            {7086, 7995, "PUBG"},
            {6015, 6015, "Rainbow Six Siege"},
            {17000, 17100, "Escape from Tarkov"},
            {9987, 9987, "TeamSpeak"},
            {50000, 50004, "Discord voice"},
        }},
        {{
            {3478, 3480, "PlayStation Network"},
            {3074, 3074, "Xbox Live"},
            {27015, 27050, "Steam"},
            {5000, 5500, "Riot Games (LoL, Valorant)"},
            {26500, 26510, "Overwatch / Blizzard"},
            {9000, 9100, "Fortnite / Epic"},
            {1024, 1124, "Apex Legends / EA"},
            {3216, 3216, "EA"},
            {9960, 9969, "EA"},
            {18000, 18060, "EA"},
            {3074, 3080, "Call of Duty"},
            {25565, 25565, "Minecraft Java"},
            {19132, 19133, "Minecraft Bedrock"},
            {7000, 9000, "Rocket League"},
            {22101, 22102, "Genshin Impact / HoYoverse"},
            {7086, 7995, "PUBG"},
            {6015, 6015, "Rainbow Six Siege"},
            {17000, 17100, "Escape from Tarkov"},
            {9987, 9987, "TeamSpeak"},
            {50000, 50004, "Discord voice"},
        }},
    }};
    inline std::array<size_t, 2> game_port_table_counts{{20, 20}};
    inline std::atomic<size_t> game_port_active_idx{0};
    inline std::mutex          game_ports_staging_mutex;
    inline std::array<PortRange, MAX_GAME_PORT_RANGES> game_port_staging{};
    inline size_t                game_port_staging_count = 0;
    // GUI sets GAME_PORTS_DIRTY (release). Exactly one consumer — Core 1 watchdog
    // in App — must exchange(acq_rel) false then call apply_pended_game_ports().
    inline std::atomic<bool>     GAME_PORTS_DIRTY{false};

    void request_game_ports_apply(std::span<const PortRange> ranges);
    void apply_pended_game_ports();

    inline size_t copy_active_game_ports_for_display(PortRange* out, size_t max_out) {
        size_t ai = game_port_active_idx.load(std::memory_order_acquire);
        size_t n  = game_port_table_counts[ai];
        size_t c  = n < max_out ? n : max_out;
        std::memcpy(out, GAME_PORT_TABLE_DOUBLE[ai].data(), c * sizeof(PortRange));
        return c;
    }

    // Per-device policy table (populated by GUI DevicePage, consumed by Core 1 watchdog)
    static constexpr size_t MAX_DEVICE_POLICIES = 64;
    struct DevicePolicy {
        Net::IPv4Net ip{};
        bool         blocked      = false;
        bool         rate_limited = false;
        Traffic::Mbps dl{100.0};
        Traffic::Mbps ul{10.0};
    };
    inline std::array<DevicePolicy, MAX_DEVICE_POLICIES> DEVICE_POLICY_TABLE{};
    inline size_t DEVICE_POLICY_COUNT = 0;
    inline std::atomic<bool> DEVICE_POLICY_DIRTY{false};
    inline std::atomic<uint64_t> DEVICE_POLICY_REVISION{0};
    inline std::mutex        device_policy_mutex;

    inline void upsert_device_policy(const DevicePolicy& policy) {
        std::lock_guard<std::mutex> lk(device_policy_mutex);
        for (size_t i = 0; i < DEVICE_POLICY_COUNT; ++i) {
            if (DEVICE_POLICY_TABLE[i].ip == policy.ip) {
                DEVICE_POLICY_TABLE[i] = policy;
                DEVICE_POLICY_REVISION.fetch_add(1, std::memory_order_release);
                return;
            }
        }
        if (DEVICE_POLICY_COUNT < MAX_DEVICE_POLICIES) {
            DEVICE_POLICY_TABLE[DEVICE_POLICY_COUNT++] = policy;
            DEVICE_POLICY_REVISION.fetch_add(1, std::memory_order_release);
        }
    }

    // Static IP rate limit table (max 256 entries, zero heap allocation)
    static constexpr size_t MAX_IP_LIMITS = 256;
    struct IpLimitEntry {
        Net::IPv4Net ip{};
        Traffic::Mbps rate{0.0};
    };
    inline std::array<IpLimitEntry, MAX_IP_LIMITS> IP_LIMIT_TABLE{};
    inline size_t IP_LIMIT_COUNT = 0;
    inline std::atomic<bool> IP_LIMIT_ACTIVE{false};
    inline std::mutex ip_limit_mutex;

    inline void clear_ip_limits() {
        std::lock_guard<std::mutex> lk(ip_limit_mutex);
        IP_LIMIT_COUNT = 0;
    }
    inline void add_ip_limit(Net::IPv4Net ip, Traffic::Mbps rate) {
        std::lock_guard<std::mutex> lk(ip_limit_mutex);
        for (size_t i = 0; i < IP_LIMIT_COUNT; ++i) {
            if (IP_LIMIT_TABLE[i].ip == ip) { IP_LIMIT_TABLE[i].rate = rate; return; }
        }
        if (IP_LIMIT_COUNT < MAX_IP_LIMITS) {
            IP_LIMIT_TABLE[IP_LIMIT_COUNT] = {ip, rate};
            ++IP_LIMIT_COUNT;
        }
    }

    // Helper: check if port is a game port (hot path — read stable active snapshot only)
    inline bool is_game_port(uint16_t port) {
        size_t ai = game_port_active_idx.load(std::memory_order_acquire);
        size_t n  = game_port_table_counts[ai];
        for (size_t i = 0; i < n; ++i) {
            const auto& r = GAME_PORT_TABLE_DOUBLE[ai][i];
            if (port >= r.start && port <= r.end) return true;
        }
        return false;
    }

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

    // Helper: convert NBO IPv4Net to dotted-decimal string
    inline std::string ip_to_str(Net::IPv4Net ip) {
        uint32_t v = ip.raw();
        return std::format("{}.{}.{}.{}",
            v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    }

    // Insert or update a static DNS record (callers must hold static_dns_mutex)
    inline void upsert_static_dns_unlocked(const std::string& hostname, const std::string& ip_str) {
        uint32_t hash = dns_hash_hostname(hostname);
        auto     ip_e = parse_ip_str(ip_str);
        if (!ip_e) return;
        Net::IPv4Net ip = *ip_e;
        for (size_t i = 0; i < STATIC_DNS_COUNT; ++i) {
            if (STATIC_DNS_TABLE[i].domain_hash == hash) {
                STATIC_DNS_TABLE[i].ip = ip;
                strncpy(STATIC_DNS_TABLE[i].hostname.data(), hostname.c_str(), 63);
                STATIC_DNS_TABLE[i].hostname[63] = '\0';
                return;
            }
        }
        if (STATIC_DNS_COUNT < MAX_STATIC_DNS) {
            auto& e = STATIC_DNS_TABLE[STATIC_DNS_COUNT++];
            e.domain_hash = hash;
            e.ip = ip;
            strncpy(e.hostname.data(), hostname.c_str(), 63);
            e.hostname[63] = '\0';
        }
    }

    inline void clear_static_dns_records() {
        std::lock_guard<std::mutex> lock(static_dns_mutex);
        STATIC_DNS_COUNT = 0;
    }

    // Insert or update a static DNS record.
    inline void upsert_static_dns(const std::string& hostname, const std::string& ip_str) {
        std::lock_guard<std::mutex> lock(static_dns_mutex);
        upsert_static_dns_unlocked(hostname, ip_str);
    }

    inline void apply_static_dns_snapshot(const StaticDnsRecord* records, size_t count) {
        std::lock_guard<std::mutex> lock(static_dns_mutex);
        STATIC_DNS_COUNT = 0;
        const size_t n = count < MAX_STATIC_DNS ? count : MAX_STATIC_DNS;
        for (size_t i = 0; i < n; ++i) {
            STATIC_DNS_TABLE[i] = records[i];
        }
        STATIC_DNS_COUNT = n;
    }

    inline size_t copy_static_dns_snapshot(StaticDnsRecord* out, size_t max_out) {
        std::lock_guard<std::mutex> lock(static_dns_mutex);
        const size_t c = STATIC_DNS_COUNT < max_out ? STATIC_DNS_COUNT : max_out;
        for (size_t i = 0; i < c; ++i) out[i] = STATIC_DNS_TABLE[i];
        return c;
    }

    // Load / save — implementations in Config.cpp (hide POSIX fd I/O from clients)
    std::expected<void, std::string> load_config(const std::string& path = "config/config.txt");
    std::expected<void, std::string> save_config(const std::string& path = "config/config.txt");
}
