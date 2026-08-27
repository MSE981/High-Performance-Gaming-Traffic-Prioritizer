#pragma once
// Minimal header: Mbps unit wrapper shared by Config and Scheduler.

namespace HPGTP::Traffic {

struct Mbps {
    double value;
    explicit constexpr Mbps(double v) noexcept : value(v) {}
};

} // namespace HPGTP::Traffic
