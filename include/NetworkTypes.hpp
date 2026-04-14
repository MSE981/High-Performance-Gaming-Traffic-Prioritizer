#pragma once
// Network byte-order type system; no implicit uint32_t conversions.
// POSIX declarations used for conversions live in NetworkTypes.cpp only.
#include <cstdint>

namespace Scalpel::Net {

struct IPv4Host; // forward declaration

// Network-byte-order IPv4 (wire representation).
struct IPv4Net {
    explicit constexpr IPv4Net()              noexcept : v_(0) {}
    explicit constexpr IPv4Net(uint32_t nbo)  noexcept : v_(nbo) {}

    [[nodiscard]] IPv4Host to_host() const noexcept;
    [[nodiscard]] uint32_t raw() const noexcept { return v_; }

    constexpr bool operator==(IPv4Net o) const noexcept { return v_ == o.v_; }
    constexpr bool operator!=(IPv4Net o) const noexcept { return v_ != o.v_; }

private:
    uint32_t v_;
};

// Host-byte-order IPv4 (arithmetic and comparisons).
struct IPv4Host {
    explicit constexpr IPv4Host()             noexcept : v_(0) {}
    explicit constexpr IPv4Host(uint32_t hbo) noexcept : v_(hbo) {}

    [[nodiscard]] IPv4Net  to_net() const noexcept;
    [[nodiscard]] uint32_t raw() const noexcept { return v_; }

    constexpr bool operator==(IPv4Host o) const noexcept { return v_ == o.v_; }
    constexpr bool operator!=(IPv4Host o) const noexcept { return v_ != o.v_; }

private:
    uint32_t v_;
};

IPv4Net parse_ipv4(const char* s) noexcept;

// Nth address after base (DHCP pool construction).
inline IPv4Net pool_advance(IPv4Net base_net, uint32_t offset) noexcept {
    return IPv4Host{base_net.to_host().raw() + offset}.to_net();
}

} // namespace Scalpel::Net
