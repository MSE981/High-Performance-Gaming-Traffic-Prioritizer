#pragma once
#include <atomic>
#include <cstdint>
#include <array>
#include <cstring>

namespace Scalpel {

    // Core metrics slot (L1 cache line aligned)
    // Forced alignment to 64 bytes ensures each CPU core's stats updates don't trigger cache line bouncing
    struct alignas(64) CoreMetrics {
        std::atomic<uint64_t> pkts{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<uint64_t> prio_pkts[3]{ 0, 0, 0 };
        std::atomic<uint64_t> prio_bytes[3]{ 0, 0, 0 };
        std::atomic<uint64_t> dropped[3]{ 0, 0, 0 };
        std::atomic<uint64_t> last_heartbeat{ 0 };
    };

    struct Telemetry {
        // Allocate independent 64-byte cache blocks for each CPU core
        std::array<CoreMetrics, 4> core_metrics{};

        // Diagnostics and control data (low-frequency read/write, no need for separation)
        std::atomic<double> internal_limit_mbps{ 0.0 };
        std::atomic<double> isp_down_limit_mbps{ 0.0 };
        std::atomic<double> isp_up_limit_mbps{ 0.0 };
        std::atomic<double> internal_pps{ 0.0 };
        std::atomic<double> isp_pps{ 0.0 };
        std::atomic<bool> is_probing{ false };
        std::atomic<bool> bridge_mode{ false };
        std::atomic<double> cpu_temp_celsius{ 0.0 };  // updated by Core 1 watchdog via timerfd, read by Qt UI

        // System info: updated by Core 1 watchdog every 5 seconds, read by UI thread on-demand.
        // char arrays are plain (not atomic) — display-only data, torn reads are acceptable.
        struct SystemInfo {
            std::array<char, 64>  hostname{};
            std::array<char, 128> kernel_short{};
            std::atomic<uint64_t> uptime_seconds{0};
            std::atomic<uint64_t> mem_total_kb{0};
            std::atomic<uint64_t> mem_avail_kb{0};
        };
        alignas(64) SystemInfo sys_info{};

        static Telemetry& instance() {
            static Telemetry inst;
            return inst;
        }

        struct BatchStats {
            uint64_t pkts = 0, bytes = 0;
            uint64_t prio_pkts[3] = { 0, 0, 0 };
            uint64_t prio_bytes[3] = { 0, 0, 0 };
            void reset() { *this = BatchStats{}; }
        };

        // Zero-contention batch commit: core N writes only to slot N.
        void commit_batch(const BatchStats& s, int core_id) {
            if (core_id < 0 || core_id >= 4) return;
            auto& m = core_metrics[core_id];
            
            m.pkts.fetch_add(s.pkts, std::memory_order_relaxed);
            m.bytes.fetch_add(s.bytes, std::memory_order_relaxed);
            
            for (int i = 0; i < 3; ++i) {
                m.prio_pkts[i].fetch_add(s.prio_pkts[i], std::memory_order_relaxed);
                m.prio_bytes[i].fetch_add(s.prio_bytes[i], std::memory_order_relaxed);
            }
            // Removed time(nullptr) syscall here, heartbeat tick incremented by idle loop
        }
    };
}

