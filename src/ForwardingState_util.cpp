#include "ForwardingState_util.hpp"
#include "Config.hpp"
#include "Network_util.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <cstring>

namespace HPGTP::ForwardState_util {

bool ForwardingState_util::is_invalid_nat_wan(Net::IPv4Net a) noexcept {
    if (a.raw() == 0) return true;
    const uint32_t h = ntohl(a.raw());
    return (h >> 24) == 127;
}

std::expected<Net::IPv4Net, std::string> ForwardingState_util::resolve_nat_wan_ip() const {
    std::string s = Utils_util::Network_util::get_local_ip(Config::iface_wan());
    if (s.empty())
        return std::unexpected(
            std::string("No IPv4 on WAN interface ") + Config::iface_wan()
            + " (assign the WAN address with NetworkManager so the kernel owns it)");
    auto e = Config::parse_ip_str(s);
    if (!e) return std::unexpected(std::string("WAN address parse failed: ") + e.error());
    if (is_invalid_nat_wan(*e))
        return std::unexpected(std::string("WAN interface has unusable address: ") + s);
    return *e;
}

const ForwardingState_util::L2Snapshot& ForwardingState_util::snapshot() const {
    return snap_[active_.load(std::memory_order_acquire)];
}

bool ForwardingState_util::resolve_mac_onlink(const L2Snapshot& snap,
                                         uint32_t dst_ip_nbo,
                                         uint8_t out_mac[6]) const {
    for (uint32_t i = 0; i < snap.arp_count; ++i) {
        if (snap.arp[i].ip_nbo == dst_ip_nbo) {
            std::memcpy(out_mac, snap.arp[i].mac.data(), 6);
            return true;
        }
    }
    return false;
}

void ForwardingState_util::refresh_l2() {
    const unsigned w = 1u - active_.load(std::memory_order_relaxed);
    L2Snapshot& snap = snap_[w];

    const bool lan_ok = Utils_util::Network_util::get_iface_hwaddr(Config::iface_lan(), snap.lan_hw.data());
    const bool wan_ok = Utils_util::Network_util::get_iface_hwaddr(Config::iface_wan(), snap.wan_hw.data());

    snap.wan_prefix_len = static_cast<int32_t>(
        Utils_util::Network_util::get_iface_ipv4_prefix_len(Config::iface_wan()));
    if (auto wan = resolve_nat_wan_ip()) {
        snap.wan_ip_nbo = wan->raw();
    } else {
        snap.wan_ip_nbo = 0;
    }
    snap.wan_cfg_valid = snap.wan_ip_nbo != 0 && !is_invalid_nat_wan(Net::IPv4Net{snap.wan_ip_nbo})
        && snap.wan_prefix_len >= 0 && snap.wan_prefix_len <= 32;

    std::string gw_ip = Utils_util::Network_util::get_default_gateway_for_iface(Config::iface_wan());
    if (gw_ip.empty())
        gw_ip = Utils_util::Network_util::get_gateway_ip();
    snap.default_gw_ip_configured = !gw_ip.empty();
    if (!gw_ip.empty())
        Utils_util::Network_util::force_arp_resolution(gw_ip);

    Utils_util::ArpTableRow rows[ForwardingState_util::L2Snapshot::MAX_ARP];
    const size_t n = Utils_util::Network_util::read_arp_table(rows, ForwardingState_util::L2Snapshot::MAX_ARP);

    snap.gw_hw_valid = false;
    if (!gw_ip.empty()) {
        struct in_addr gw_addr {};
        if (inet_pton(AF_INET, gw_ip.c_str(), &gw_addr) == 1) {
            const uint32_t gw_nbo = gw_addr.s_addr;
            for (size_t i = 0; i < n; ++i) {
                if (rows[i].ip_nbo == gw_nbo) {
                    std::memcpy(snap.gw_hw.data(), rows[i].mac, 6);
                    snap.gw_hw_valid = true;
                    break;
                }
            }
        }
    }

    const uint32_t ac = static_cast<uint32_t>(
        std::min(n, static_cast<size_t>(ForwardingState_util::L2Snapshot::MAX_ARP)));
    snap.arp_count = ac;
    for (uint32_t i = 0; i < ac; ++i) {
        snap.arp[i].ip_nbo = rows[i].ip_nbo;
        std::memcpy(snap.arp[i].mac.data(), rows[i].mac, 6);
    }

    snap.ready = lan_ok && wan_ok && (snap.gw_hw_valid || snap.wan_cfg_valid);
    active_.store(w, std::memory_order_release);
}

} // namespace HPGTP::ForwardState_util
