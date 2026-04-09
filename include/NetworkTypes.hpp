#pragma once
// Network byte-order type system — no implicit uint32_t conversions.
// POSIX headers confined to this one place; callers include only this file.
#include <cstdint>
#include <netinet/in.h>   // htonl / ntohl / INADDR_NONE
#include <arpa/inet.h>    // inet_addr

namespace Scalpel::Net {

struct IPv4Host;   // forward declaration

// ── Network-byte-order IPv4 address (wire representation) ────────────────────
// Constructed only from explicit NBO source (inet_addr, packet header read).
// Use to_host() for arithmetic; use raw() only at POSIX / wire boundaries.
struct IPv4Net {
    explicit constexpr IPv4Net()              noexcept : v_(0)   {}
    explicit constexpr IPv4Net(uint32_t nbo)  noexcept : v_(nbo) {}

    [[nodiscard]] IPv4Host to_host() const noexcept;
    [[nodiscard]] uint32_t raw()     const noexcept { return v_; }

    constexpr bool operator==(IPv4Net o) const noexcept { return v_ == o.v_; }
    constexpr bool operator!=(IPv4Net o) const noexcept { return v_ != o.v_; }

private:
    uint32_t v_;
};

// ── Host-byte-order IPv4 address (for arithmetic / comparisons) ───────────────
struct IPv4Host {
    explicit constexpr IPv4Host()             noexcept : v_(0)   {}
    explicit constexpr IPv4Host(uint32_t hbo) noexcept : v_(hbo) {}

    [[nodiscard]] IPv4Net  to_net()  const noexcept;
    [[nodiscard]] uint32_t raw()     const noexcept { return v_; }

    constexpr bool operator==(IPv4Host o) const noexcept { return v_ == o.v_; }
    constexpr bool operator!=(IPv4Host o) const noexcept { return v_ != o.v_; }

private:
    uint32_t v_;
};

// Inline definitions — the only place ::htonl / ::ntohl appear in this project
inline IPv4Host IPv4Net::to_host()  const noexcept { return IPv4Host{::ntohl(v_)}; }
inline IPv4Net  IPv4Host::to_net()  const noexcept { return IPv4Net {::htonl(v_)}; }

// ── Helpers ───────────────────────────────────────────────────────────────────

// Sole string-to-NBO entry point (replaces bare ::inet_addr call-sites).
inline IPv4Net parse_ipv4(const char* s) noexcept {
    uint32_t r = ::inet_addr(s);
    return (r == INADDR_NONE) ? IPv4Net{} : IPv4Net{r};
}

// Sequential address pool: Nth IP after base (used by DhcpEngine pool init).
inline IPv4Net pool_advance(IPv4Net base_net, uint32_t offset) noexcept {
    return IPv4Host{base_net.to_host().raw() + offset}.to_net();
}

} // namespace Scalpel::Net
