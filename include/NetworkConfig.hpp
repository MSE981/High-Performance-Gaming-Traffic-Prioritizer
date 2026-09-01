#pragma once
#include <expected>
#include <string>
#include "DhcpEngine.hpp"
#include "Types_util.hpp"

namespace HPGTP::NetConfig {

// Aligns the LAN interface inside the DHCP pool subnet and keeps the kernel LAN IP in sync with the DHCP gateway.
class NetworkConfig {
public:
    std::expected<void, std::string> sync(Engine::Dhcp::DhcpEngine& dhcp);
    void refresh_dhcp_router(Engine::Dhcp::DhcpEngine& dhcp) noexcept;

private:
    Utils::Net::IPv4Net effective_lan_gateway_{};
};

} // namespace HPGTP::NetConfig
