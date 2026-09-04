#include "NetworkConfig.hpp"
#include "Config.hpp"
#include "Network_util.hpp"
#include <optional>
#include <string>
#include <print>

namespace HPGTP::NetConfig {

std::expected<void, std::string> NetworkConfig::sync(Engine::Dhcp::DhcpEngine& dhcp) {
    auto parse_kern = [](const std::string& s) -> std::optional<Utils::Net::IPv4Net> {
        if (s.empty()) return std::nullopt;
        auto e = Config::parse_ip_str(s);
        if (!e) return std::nullopt;
        return *e;
    };

    auto pstart_e = Config::parse_ip_str(Config::DHCP_POOL_START);
    auto pend_e   = Config::parse_ip_str(Config::DHCP_POOL_END);
    if (!pstart_e) return std::unexpected(std::string("DHCP_POOL_START: ") + pstart_e.error());
    if (!pend_e) return std::unexpected(std::string("DHCP_POOL_END: ") + pend_e.error());
    const Utils::Net::IPv4Net pool_start = *pstart_e;
    const Utils::Net::IPv4Net pool_end   = *pend_e;
    if (ntohl(pool_start.raw()) > ntohl(pool_end.raw()))
        return std::unexpected(std::string("DHCP pool start is after pool end"));

    const int prefix = Utils::Network::Network_util::infer_prefix_covering_pool(pool_start, pool_end);
    if (prefix > 32)
        return std::unexpected(std::string("invalid DHCP pool prefix derivation"));

    if (Config::LAN_PREFIX_LEN > 0 && Config::LAN_PREFIX_LEN <= 32
        && Config::LAN_PREFIX_LEN != prefix)
        std::println(stderr,
            "[App] LAN_PREFIX_LEN={} differs from pool-derived prefix {}; using derived for subnet and ioctl.",
            Config::LAN_PREFIX_LEN, prefix);

    auto router_e = Config::parse_ip_str(Config::ROUTER_IP);
    if (!router_e) return std::unexpected(std::string("ROUTER_IP: ") + router_e.error());

    if (Config::APPLY_ROUTER_IP_TO_LAN) {
        if (!Utils::Network::Network_util::set_iface_ipv4_and_prefix(
                Config::iface_lan(), Config::ROUTER_IP, prefix)) {
            std::println(stderr, "[App] set_iface ROUTER_IP on {} failed: {}",
                Config::iface_lan(), std::strerror(errno));
        }
    }

    auto in_pool_subnet = [&](Utils::Net::IPv4Net ip) {
        return Utils::Network::Network_util::ipv4_in_subnet(ip, prefix, pool_start);
    };

    std::string       kern_s = Utils::Network::Network_util::get_local_ip(Config::iface_lan());
    std::optional<Utils::Net::IPv4Net> kern = parse_kern(kern_s);

    bool ok = kern.has_value() && in_pool_subnet(*kern);
    if (!ok) {
        if (in_pool_subnet(*router_e)) {
            if (!Utils::Network::Network_util::set_iface_ipv4_and_prefix(
                    Config::iface_lan(), Config::ROUTER_IP, prefix))
                return std::unexpected(std::string("set_iface ROUTER_IP: ") + std::strerror(errno));
            kern_s = Utils::Network::Network_util::get_local_ip(Config::iface_lan());
            kern   = parse_kern(kern_s);
            ok     = kern.has_value() && in_pool_subnet(*kern);
        } else {
            return std::unexpected(
                std::string("ROUTER_IP is not in the DHCP pool subnet (derived /") + std::to_string(prefix)
                + "); fix ROUTER_IP or DHCP_POOL_*");
        }
    }

    if (!ok || !kern.has_value())
        return std::unexpected(
            std::string("IFACE_LAN has no IPv4 in the DHCP pool subnet; set APPLY_ROUTER_IP_TO_LAN=true "
                        "with ROUTER_IP inside the pool subnet, or assign manually."));

    effective_lan_gateway_ = *kern;
    dhcp.set_router_ip(effective_lan_gateway_);
    std::println("[App] DHCP gateway {} (kernel LAN, pool subnet /{})",
        Config::ip_to_str(effective_lan_gateway_), prefix);
    return {};
}

void NetworkConfig::refresh_dhcp_router(Engine::Dhcp::DhcpEngine& dhcp) noexcept {
    const std::string s = Utils::Network::Network_util::get_local_ip(Config::iface_lan());
    if (s.empty()) return;
    auto e = Config::parse_ip_str(s);
    if (!e) return;
    if (*e != dhcp.router_ip_snapshot()) {
        dhcp.set_router_ip(*e);
    }
}

} // namespace HPGTP::NetConfig
