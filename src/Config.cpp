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

// interface names
static std::string g_iface_wan{"eth0"};
static std::string g_iface_lan{"eth1"};

const std::string& iface_wan() { return g_iface_wan; }
const std::string& iface_lan() { return g_iface_lan; }

// private

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

// public

std::expected<void, std::string> load_config(const std::string& path) {
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

    char key[64]{}, val[256]{};

    const char* p   = buf;
    const char* end = buf + n;
    while (p < end) {
        const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
        size_t llen = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);

        if (llen > 0 && p[llen - 1] == '\r') --llen;

        if (llen > 0 && p[0] != '#' && parse_kv(p, llen, key, sizeof(key), val, sizeof(val))) {
            try {
                if      (!strcmp(key, "IFACE_WAN"))  g_iface_wan  = val;
                else if (!strcmp(key, "IFACE_LAN"))  g_iface_lan  = val;
                else if (!strcmp(key, "ROUTER_IP"))  ROUTER_IP  = val;
                else if (!strcmp(key, "APPLY_ROUTER_IP_TO_LAN"))
                    APPLY_ROUTER_IP_TO_LAN = (!strcmp(val, "true") || !strcmp(val, "1"));
                else if (!strcmp(key, "LAN_PREFIX_LEN")) LAN_PREFIX_LEN = static_cast<int>(parse_u32(val));
                else if (!strcmp(key, "ENABLE_ACCELERATION"))
                    ENABLE_ACCELERATION.store(!strcmp(val, "true") || !strcmp(val, "1"),
                        std::memory_order_relaxed);
                else if (!strcmp(key, "LARGE_PACKET_THRESHOLD")) LARGE_PACKET_THRESHOLD_BYTES = parse_u32(val);
                else if (!strcmp(key, "enable_nat"))        global_state.enable_nat.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "enable_dhcp"))       global_state.enable_dhcp.store(!strcmp(val, "true") || !strcmp(val, "1"), std::memory_order_relaxed);
                else if (!strcmp(key, "DHCP_POOL_START"))   DHCP_POOL_START = val;
                else if (!strcmp(key, "DHCP_POOL_END"))     DHCP_POOL_END   = val;
                else if (!strcmp(key, "DHCP_LEASE_SECONDS")) DHCP_LEASE_DURATION = std::chrono::seconds{parse_u32(val)};
            } catch (...) {
                std::println(stderr, "[Config] Error parsing line: {}={}", key, val);
            }
        }

        p = nl ? nl + 1 : end;
    }

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
    dprintf(fd, "IFACE_WAN=%s\n",            g_iface_wan.c_str());
    dprintf(fd, "IFACE_LAN=%s\n",            g_iface_lan.c_str());
    dprintf(fd, "ROUTER_IP=%s\n",            ROUTER_IP.c_str());
    dprintf(fd, "APPLY_ROUTER_IP_TO_LAN=%s\n", b(APPLY_ROUTER_IP_TO_LAN));
    dprintf(fd, "LAN_PREFIX_LEN=%d\n",       LAN_PREFIX_LEN);
    dprintf(fd, "ENABLE_ACCELERATION=%s\n",  b(ENABLE_ACCELERATION.load(std::memory_order_relaxed)));
    dprintf(fd, "LARGE_PACKET_THRESHOLD=%u\n", LARGE_PACKET_THRESHOLD_BYTES);
    dprintf(fd, "enable_nat=%s\n",        b(global_state.enable_nat.load(std::memory_order_relaxed)));
    dprintf(fd, "enable_dhcp=%s\n",       b(global_state.enable_dhcp.load(std::memory_order_relaxed)));
    dprintf(fd, "DHCP_POOL_START=%s\n",   DHCP_POOL_START.c_str());
    dprintf(fd, "DHCP_POOL_END=%s\n",     DHCP_POOL_END.c_str());
    dprintf(fd, "DHCP_LEASE_SECONDS=%u\n", static_cast<uint32_t>(DHCP_LEASE_DURATION.count()));
    ::close(fd);
    std::println("[Config] Config saved: {}", path);
    return {};
}

} // namespace HPGTP::Config
