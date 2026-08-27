#pragma once
// Minimal header: Mbps unit wrapper shared by Config and Scheduler.

namespace HPGTP::Utils::Units {

struct Mbps {
    double value;
    explicit constexpr Mbps(double v) noexcept : value(v) {}
};

} // namespace HPGTP::Utils::Units
