#include "NetworkTypes.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>

namespace Scalpel::Net {

IPv4Host IPv4Net::to_host() const noexcept {
    return IPv4Host{::ntohl(v_)};
}

IPv4Net IPv4Host::to_net() const noexcept {
    return IPv4Net{::htonl(v_)};
}

IPv4Net parse_ipv4(const char* s) noexcept {
    uint32_t r = ::inet_addr(s);
    return (r == INADDR_NONE) ? IPv4Net{} : IPv4Net{r};
}

} // namespace Scalpel::Net
