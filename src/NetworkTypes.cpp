#include "NetworkTypes.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>

namespace HPGTP::Net {

IPv4Host IPv4Net::to_host() const noexcept {
    return IPv4Host{::ntohl(v_)};
}

IPv4Net IPv4Host::to_net() const noexcept {
    return IPv4Net{::htonl(v_)};
}

bool try_parse_ipv4(const char* s, IPv4Net& out) noexcept {
    if (!s) return false;
    struct ::in_addr a {};
    if (::inet_pton(AF_INET, s, &a) != 1) return false;
    out = IPv4Net{a.s_addr};
    return true;
}

IPv4Net parse_ipv4(const char* s) noexcept {
    IPv4Net out{};
    (void)try_parse_ipv4(s, out);
    return out;
}

} // namespace HPGTP::Net
