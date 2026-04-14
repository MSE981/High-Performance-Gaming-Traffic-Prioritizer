#pragma once
// Strong unit types for physical quantities.
// Kept in a minimal header so both Config and Scheduler can depend on it
// without creating a circular inclusion chain.

namespace HPGTP::Traffic {

// Prevents bare double values from crossing API boundaries for network rates.
struct Mbps {
    double value;
    explicit constexpr Mbps(double v) noexcept : value(v) {}
};

} // namespace HPGTP::Traffic
