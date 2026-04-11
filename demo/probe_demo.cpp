// probe_demo: run the internal CPU stress benchmark and print result
//
// Build: make probe_demo
// Run:   ./probe_demo   (no root required — pure in-memory benchmark)
//
// Expected output: a Mbps figure reflecting the Pi 5 HeuristicProcessor throughput.
// Typical range on Cortex-A76 @ 2.4 GHz: 8000–15000 Mbps.
#include "ProbeManager.hpp"
#include "Telemetry.hpp"
#include <print>
#include <atomic>

int main() {
    std::println("=== Probe Manager Demo ===");
    std::println("[INFO] Running 5-second CPU capacity benchmark…");

    std::atomic<double> result{0.0};

    Scalpel::Probe::Manager::run_internal_stress([&result](double mbps) {
        result.store(mbps, std::memory_order_relaxed);
    });

    std::println("[RESULT] Measured capacity: {:.1f} Mbps",
        result.load(std::memory_order_relaxed));
    std::println("=== Done ===");
    return 0;
}
