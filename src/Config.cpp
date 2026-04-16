#include "Config.hpp"
#include <print>
#include <cstring>
#include <cstdlib>
#include <charconv>
#include <string_view>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace HPGTP::Config {

// ── game port load accumulator (single-threaded load_config) ─────────────────
static std::array<PortRange, MAX_GAME_PORT_RANGES> g_load_game_ports{};
static size_t                                      g_load_game_ports_n = 0;

static void commit_loaded_game_ports_to_runtime() {
    if (g_load_game_ports_n == 0) return;
    size_t n = g_load_game_ports_n;
    for (int b = 0; b < 2; ++b) {
        std::memcpy(GAME_PORT_TABLE_DOUBLE[static_cast<size_t>(b)].data(),
            g_load_game_ports.data(), n * sizeof(PortRange));
        game_port_table_counts[static_cast<size_t>(b)] = n;
    }
    std::memcpy(game_port_staging.data(), g_load_game_ports.data(), n * sizeof(PortRange));
    game_port_staging_count = n;
    game_port_active_idx.store(0, std::memory_order_relaxed);
}

void request_game_ports_apply(std::span<const PortRange> ranges) {
    size_t n = std::min(ranges.size(), MAX_GAME_PORT_RANGES);
    std::lock_guard<std::mutex> lock(game_ports_staging_mutex);
    for (size_t i = 0; i < n; ++i)
        game_port_staging[i] = ranges[i];
    game_port_staging_count = n;
    GAME_PORTS_DIRTY.store(true, std::memory_order_release);
}

void apply_pended_game_ports() {
    std::array<PortRange, MAX_GAME_PORT_RANGES> local{};
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(game_ports_staging_mutex);
        n = game_port_staging_count;
        std::memcpy(local.data(), game_port_staging.data(), n * sizeof(PortRange));
    }
    size_t cur  = game_port_active_idx.load(std::memory_order_relaxed);
    size_t next = 1 - cur;
    std::memcpy(GAME_PORT_TABLE_DOUBLE[next].data(), local.data(), n * sizeof(PortRange));
    game_port_table_counts[next] = n;
    game_port_active_idx.store(next, std::memory_order_release);
    std::println("[QoS] Game port whitelist applied: {} range(s)", n);
}

// ── private helpers ──────────────────────────────────────────────────────────

static uint32_t parse_u32(const char* s) {
    uint32_t v = 0;
    std::string_view sv{s};
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

static bool parse_kv(const char* line, size_t len,
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

// ── public API ────────────────────────────────────────────────────────────────

std::expected<void, std::string> load_config(const std::string& path) {
    g_load_game_ports_n = 0;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            std::println(stderr, "[Config] Warning: cannot open config file {}, using defaults.", path);
            return {};
        }
        return std::unexpected(
            std::string("cannot open ") + path + ": " + std::strerror(errno));
    }

    char buf[8192]{};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return {};
    buf[n] = '\0';

    bool bridge_iface_loaded = false;
    char key[64]{}, val[256]{};

    const char* p   = buf;
    const char* end = buf + n;
    while (p < end) {
        const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
        size_t llen = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);

        if (llen > 0 && p[llen - 1] == '\r') --llen;

        if (llen > 0 && p[0] != '#' && parse_kv(p, llen, key, sizeof(key), val, sizeof(val))) {
            try {
                if      (!strcmp(key, "IFACE_WAN"))  IFACE_WAN  = val;
                else if (!strcmp(key, "IFACE_LAN"))  IFACE_LAN  = val;
                else if (!strcmp(key, "ROUTER_IP"))  ROUTER_IP  = val;
                else if (!strcmp(key, "ENABLE_ACCELERATION"))
                    ENABLE_ACCELERATION.store(!strcmp(val, "true") || !strcmp(val, "1"),
                        std::memory_order_relaxed);
                else if (!strcmp(key, "ENABLE_STP"))          ENABLE_STP.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "ENABLE_IGMP_SNOOPING")) ENABLE_IGMP_SNOOPING.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "BRIDGE_IFACE")) {
                    if (!bridge_iface_loaded) { clear_bridged(); bridge_iface_loaded = true; }
                    add_bridged(val);
                }
                else if (!strcmp(key, "LARGE_PACKET_THRESHOLD")) LARGE_PACKET_THRESHOLD_BYTES = parse_u32(val);
                else if (!strcmp(key, "PUNISH_TRIGGER_COUNT"))   PUNISH_TRIGGER_COUNT   = parse_u32(val);
                else if (!strcmp(key, "CLEANUP_INTERVAL"))       CLEANUP_INTERVAL_PKTS  = parse_u32(val);
                else if (!strcmp(key, "enable_gui"))        global_state.enable_gui.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_nat"))        global_state.enable_nat.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_dhcp"))       global_state.enable_dhcp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_dns_cache"))  global_state.enable_dns_cache.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_upnp"))       global_state.enable_upnp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_firewall"))   global_state.enable_firewall.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_pppoe"))      global_state.enable_pppoe.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "DHCP_POOL_START"))   DHCP_POOL_START = val;
                else if (!strcmp(key, "DHCP_POOL_END"))     DHCP_POOL_END   = val;
                else if (!strcmp(key, "DHCP_LEASE_SECONDS")) DHCP_LEASE_DURATION = std::chrono::seconds{parse_u32(val)};
                else if (!strcmp(key, "DNS_UPSTREAM_PRIMARY"))   DNS_UPSTREAM_PRIMARY   = val;
                else if (!strcmp(key, "DNS_UPSTREAM_SECONDARY")) DNS_UPSTREAM_SECONDARY = val;
                else if (!strcmp(key, "DNS_REDIRECT_ENABLED")) DNS_REDIRECT_ENABLED.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "STATIC_DNS")) {
                    char* colon = strchr(val, ':');
                    if (colon && STATIC_DNS_COUNT < MAX_STATIC_DNS) {
                        *colon = '\0';
                        upsert_static_dns(val, colon + 1);
                    }
                }
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
                        Net::IPv4Net ip = parse_ip_str(val);
                        add_ip_limit(ip, Traffic::Mbps{atof(colon + 1)});
                    }
                }
                else if (!strcmp(key, "GAME_PORT")) {
                    if (g_load_game_ports_n < MAX_GAME_PORT_RANGES) {
                        char* dash = strchr(val, '-');
                        uint32_t a = 0, b = 0;
                        if (dash) {
                            *dash = '\0';
                            a = parse_u32(val);
                            b = parse_u32(dash + 1);
                            *dash = '-';
                        } else {
                            a = b = parse_u32(val);
                        }
                        if (a <= 65535u && b <= 65535u && a <= b)
                            g_load_game_ports[g_load_game_ports_n++] = {
                                static_cast<uint16_t>(a), static_cast<uint16_t>(b)};
                    }
                }
            } catch (...) {
                std::println(stderr, "[Config] Error parsing line: {}={}", key, val);
            }
        }

        p = nl ? nl + 1 : end;
    }

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
    commit_loaded_game_ports_to_runtime();
    std::println("[Config] Config loaded: {}", path);
    return {};
}

std::expected<void, std::string> save_config(const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return std::unexpected(
            std::string("cannot write ") + path + ": " + std::strerror(errno));
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
    dprintf(fd, "LARGE_PACKET_THRESHOLD=%u\n", LARGE_PACKET_THRESHOLD_BYTES);
    dprintf(fd, "PUNISH_TRIGGER_COUNT=%u\n",   PUNISH_TRIGGER_COUNT);
    dprintf(fd, "CLEANUP_INTERVAL=%u\n",       CLEANUP_INTERVAL_PKTS);
    dprintf(fd, "enable_gui=%s\n",        b(global_state.enable_gui.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_nat=%s\n",        b(global_state.enable_nat.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_dhcp=%s\n",       b(global_state.enable_dhcp.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_dns_cache=%s\n",  b(global_state.enable_dns_cache.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_upnp=%s\n",       b(global_state.enable_upnp.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_firewall=%s\n",   b(global_state.enable_firewall.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_pppoe=%s\n",      b(global_state.enable_pppoe.load(std::memory_order_relaxed)));
    dprintf(fd, "DHCP_POOL_START=%s\n",   DHCP_POOL_START.c_str());
    dprintf(fd, "DHCP_POOL_END=%s\n",     DHCP_POOL_END.c_str());
    dprintf(fd, "DHCP_LEASE_SECONDS=%u\n", static_cast<uint32_t>(DHCP_LEASE_DURATION.count()));
    dprintf(fd, "DNS_UPSTREAM_PRIMARY=%s\n",   DNS_UPSTREAM_PRIMARY.c_str());
    dprintf(fd, "DNS_UPSTREAM_SECONDARY=%s\n", DNS_UPSTREAM_SECONDARY.c_str());
    dprintf(fd, "DNS_REDIRECT_ENABLED=%s\n",   b(DNS_REDIRECT_ENABLED.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < STATIC_DNS_COUNT; ++i) {
        uint32_t ip = STATIC_DNS_TABLE[i].ip.raw();
        dprintf(fd, "STATIC_DNS=%s:%u.%u.%u.%u\n",
            STATIC_DNS_TABLE[i].hostname.data(),
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }
    for (size_t i = 0; i < IP_LIMIT_COUNT; ++i) {
        uint32_t ip = IP_LIMIT_TABLE[i].ip.raw();
        dprintf(fd, "IP_LIMIT=%u.%u.%u.%u:%.6g\n",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
            IP_LIMIT_TABLE[i].rate.value);
    }
    {
        size_t ai = game_port_active_idx.load(std::memory_order_acquire);
        size_t n  = game_port_table_counts[ai];
        for (size_t i = 0; i < n; ++i) {
            const auto& r = GAME_PORT_TABLE_DOUBLE[ai][i];
            if (r.start == r.end)
                dprintf(fd, "GAME_PORT=%u\n", static_cast<unsigned>(r.start));
            else
                dprintf(fd, "GAME_PORT=%u-%u\n", static_cast<unsigned>(r.start),
                    static_cast<unsigned>(r.end));
        }
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
    return {};
}

} // namespace HPGTP::Config
