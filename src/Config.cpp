#include "Config.hpp"
#include <print>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace Scalpel::Config {

// ── private helpers ──────────────────────────────────────────────────────────

static unsigned long atoul(const char* s) {
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + static_cast<unsigned long>(*s++ - '0');
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

void load_config(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::println(stderr, "[Config] Warning: cannot open config file {}, using defaults.", path);
        return;
    }

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
        const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
        size_t llen = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);

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
                else if (!strcmp(key, "LARGE_PACKET_THRESHOLD")) LARGE_PACKET_THRESHOLD_BYTES = static_cast<uint32_t>(atoul(val));
                else if (!strcmp(key, "PUNISH_TRIGGER_COUNT"))   PUNISH_TRIGGER_COUNT   = static_cast<uint32_t>(atoul(val));
                else if (!strcmp(key, "CLEANUP_INTERVAL"))       CLEANUP_INTERVAL_PKTS  = static_cast<uint32_t>(atoul(val));
                else if (!strcmp(key, "enable_gui"))        global_state.enable_gui.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_nat"))        global_state.enable_nat.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_dhcp"))       global_state.enable_dhcp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_dns_cache"))  global_state.enable_dns_cache.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_upnp"))       global_state.enable_upnp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_firewall"))   global_state.enable_firewall.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_pppoe"))      global_state.enable_pppoe.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "DHCP_POOL_START"))   DHCP_POOL_START = val;
                else if (!strcmp(key, "DHCP_POOL_END"))     DHCP_POOL_END   = val;
                else if (!strcmp(key, "DHCP_LEASE_SECONDS")) DHCP_LEASE_SECONDS = static_cast<uint32_t>(atoul(val));
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

void save_config(const std::string& path) {
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
    dprintf(fd, "DHCP_LEASE_SECONDS=%u\n", DHCP_LEASE_SECONDS);
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
            IP_LIMIT_TABLE[i].rate_mbps);
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

} // namespace Scalpel::Config
