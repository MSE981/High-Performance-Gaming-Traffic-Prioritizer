#include "NetworkUtils.hpp"
#include "NetworkTypes.hpp"
#include <print>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/sockios.h>

namespace HPGTP::Utils {

std::string Network::get_local_ip(const std::string& iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};

    struct ifreq ifr {};
    ifr.ifr_addr.sa_family = AF_INET;
    auto n = std::string_view{iface}.copy(ifr.ifr_name, IFNAMSIZ - 1);
    ifr.ifr_name[n] = '\0';

    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return {};
    }
    char ip_buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, ip_buf, sizeof(ip_buf));
    close(fd);
    return ip_buf;
}

std::string Network::get_gateway_ip() {
    char buf[2048]{};
    int fd = ::open("/proc/net/route", O_RDONLY);
    if (fd < 0) return "";
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";

    char* line = buf;
    char* end  = buf + n;
    while (line < end && *line != '\n') ++line;
    if (line < end) ++line;

    while (line < end) {
        char iface[32]{}, dest[16]{}, gw[16]{};
        if (sscanf(line, "%31s %15s %15s", iface, dest, gw) == 3) {
            if (strcmp(dest, "00000000") == 0) {
                unsigned int addr = 0;
                if (sscanf(gw, "%x", &addr) == 1) {
                    struct in_addr in{};
                    in.s_addr = addr;
                    char gw_buf[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &in, gw_buf, sizeof(gw_buf));
                    return gw_buf;
                }
            }
        }
        while (line < end && *line != '\n') ++line;
        if (line < end) ++line;
    }
    return "";
}

std::string Network::get_default_gateway_for_iface(const std::string& iface) {
    char buf[2048]{};
    int fd = ::open("/proc/net/route", O_RDONLY);
    if (fd < 0) return "";
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";
    buf[n] = '\0';

    char* line = buf;
    char* end  = buf + n;
    while (line < end && *line != '\n') ++line;
    if (line < end) ++line;

    while (line < end) {
        char ifn[32]{}, dest[16]{}, gw[16]{};
        if (sscanf(line, "%31s %15s %15s", ifn, dest, gw) == 3) {
            if (iface == ifn && strcmp(dest, "00000000") == 0) {
                unsigned int addr = 0;
                if (sscanf(gw, "%x", &addr) == 1 && addr != 0) {
                    struct in_addr in{};
                    in.s_addr = addr;
                    char gw_buf[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &in, gw_buf, sizeof(gw_buf));
                    return gw_buf;
                }
            }
        }
        while (line < end && *line != '\n') ++line;
        if (line < end) ++line;
    }
    return "";
}

bool Network::get_iface_hwaddr(const std::string& iface, uint8_t out[6]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct ifreq ifr{};
    auto n = std::string_view{iface}.copy(ifr.ifr_name, IFNAMSIZ - 1);
    ifr.ifr_name[n] = '\0';

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return false;
    }
    close(fd);
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) return false;
    std::memcpy(out, ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

bool Network::parse_mac_colon(std::string_view s, uint8_t out[6]) {
    if (s.size() < 17) return false;
    char buf[24]{};
    std::memcpy(buf, s.data(), std::min(s.size(), sizeof(buf) - 1u));
    unsigned a{}, b{}, c{}, d{}, e{}, f{};
    if (std::sscanf(buf, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f) != 6)
        return false;
    out[0] = static_cast<uint8_t>(a);
    out[1] = static_cast<uint8_t>(b);
    out[2] = static_cast<uint8_t>(c);
    out[3] = static_cast<uint8_t>(d);
    out[4] = static_cast<uint8_t>(e);
    out[5] = static_cast<uint8_t>(f);
    return true;
}

std::string Network::get_mac_from_arp(const std::string& target_ip) {
    char buf[4096]{};
    int fd = ::open("/proc/net/arp", O_RDONLY);
    if (fd < 0) return "";
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";

    char* line = buf;
    char* end  = buf + n;
    while (line < end && *line != '\n') ++line;
    if (line < end) ++line;

    while (line < end) {
        char ip[32]{}, hw[8]{}, flags[8]{}, mac[20]{};
        if (sscanf(line, "%31s %7s %7s %19s", ip, hw, flags, mac) >= 4) {
            if (target_ip == ip) return mac;
        }
        while (line < end && *line != '\n') ++line;
        if (line < end) ++line;
    }
    return "";
}

size_t Network::read_arp_table(ArpTableRow* out, size_t max_out) {
    if (!out || max_out == 0) return 0;
    char buf[65536]{};
    int fd = ::open("/proc/net/arp", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return 0;

    char* line = buf;
    char* end  = buf + n;
    while (line < end && *line != '\n') ++line;
    if (line < end) ++line;

    size_t count = 0;
    while (line < end && count < max_out) {
        char ip[32]{}, hw[8]{}, flags[8]{}, mac_str[20]{};
        if (sscanf(line, "%31s %7s %7s %19s", ip, hw, flags, mac_str) >= 4) {
            if (strcmp(flags, "0x0") != 0) {
                struct in_addr addr {};
                if (inet_pton(AF_INET, ip, &addr) == 1) {
                    uint8_t mac[6]{};
                    if (parse_mac_colon(mac_str, mac)) {
                        out[count].ip_nbo = addr.s_addr;
                        std::memcpy(out[count].mac, mac, 6);
                        ++count;
                    }
                }
            }
        }
        while (line < end && *line != '\n') ++line;
        if (line < end) ++line;
    }
    return count;
}

void Network::force_arp_resolution(const std::string& target_ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(33434);
    inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

    char dummy = 0;
    sendto(fd, &dummy, 1, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(fd);
}

bool Network::disable_hardware_offloads(const std::string& iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct ifreq ifr {};
    auto n = std::string_view{iface}.copy(ifr.ifr_name, IFNAMSIZ - 1);
    ifr.ifr_name[n] = '\0';

    struct ethtool_value eval {};
    bool success = true;

    auto set_offload = [&](uint32_t cmd) {
        eval.cmd = cmd;
        eval.data = 0;
        ifr.ifr_data = reinterpret_cast<char*>(&eval);
        if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) success = false;
    };

    set_offload(ETHTOOL_SGRO);
    set_offload(ETHTOOL_SGSO);
    set_offload(ETHTOOL_STSO);
    set_offload(ETHTOOL_SSG);

    close(fd);
    return success;
}

bool Network::set_iface_ipv4_and_prefix(const std::string& iface, const std::string& ipv4, int prefix_len) {
    if (prefix_len <= 0 || prefix_len > 32) return false;
    if (iface.size() >= IFNAMSIZ) return false;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    // Names must not be ifr_addr / ifr_netmask — linux/netdevice.h macros break `struct ifreq ifr_*`.
    struct ifreq req_addr {};
    iface.copy(req_addr.ifr_name, IFNAMSIZ - 1);
    req_addr.ifr_name[IFNAMSIZ - 1] = '\0';
    auto* sin_a = reinterpret_cast<struct sockaddr_in*>(&req_addr.ifr_addr);
    sin_a->sin_family = AF_INET;
    if (inet_pton(AF_INET, ipv4.c_str(), &sin_a->sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (ioctl(fd, SIOCSIFADDR, &req_addr) < 0) {
        close(fd);
        return false;
    }

    struct ifreq req_nm {};
    iface.copy(req_nm.ifr_name, IFNAMSIZ - 1);
    req_nm.ifr_name[IFNAMSIZ - 1] = '\0';
    auto* sin_m = reinterpret_cast<struct sockaddr_in*>(&req_nm.ifr_netmask);
    sin_m->sin_family = AF_INET;
    const uint32_t pl = static_cast<uint32_t>(prefix_len);
    const uint32_t mask_h = 0xFFFFFFFFu << (32u - pl);
    sin_m->sin_addr.s_addr = htonl(mask_h);
    if (ioctl(fd, SIOCSIFNETMASK, &req_nm) < 0) {
        close(fd);
        return false;
    }

    struct ifreq req_up {};
    iface.copy(req_up.ifr_name, IFNAMSIZ - 1);
    req_up.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(fd, SIOCGIFFLAGS, &req_up) == 0) {
        req_up.ifr_flags = static_cast<short>(req_up.ifr_flags | IFF_UP);
        ioctl(fd, SIOCSIFFLAGS, &req_up);
    }
    close(fd);
    return true;
}

int Network::infer_prefix_covering_pool(Net::IPv4Net a, Net::IPv4Net b) noexcept {
    uint32_t ha = ntohl(a.raw());
    uint32_t hb = ntohl(b.raw());
    if (ha > hb) std::swap(ha, hb);
    if (ha == hb) return 32;
    int p = 0;
    for (; p < 32; ++p) {
        if (((ha >> (31 - p)) & 1u) != ((hb >> (31 - p)) & 1u)) break;
    }
    return p;
}

bool Network::ipv4_in_subnet(Net::IPv4Net ip, int prefix_len, Net::IPv4Net anchor) noexcept {
    if (prefix_len < 0 || prefix_len > 32) return false;
    const uint32_t hi = ntohl(ip.raw());
    const uint32_t ha = ntohl(anchor.raw());
    const uint32_t mask =
        prefix_len == 0 ? 0u : (0xFFFFFFFFu << (32u - static_cast<unsigned>(prefix_len)));
    return (hi & mask) == (ha & mask);
}

} // namespace HPGTP::Utils
