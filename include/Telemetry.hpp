#pragma once
#include <atomic>
#include <cstdint>
#include <array>
#include <cstring>
#include <expected>
#include <string>
#include <mutex>
#include "Types_util.hpp"

namespace HPGTP {

    // Core metrics slot
    // Forced alignment to 64 bytes keeps each CPU core's stats updates from triggering cache line bouncing
    struct alignas(64) CoreMetrics {
        std::atomic<uint64_t> pkts{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<uint64_t> prio_pkts[2]{ 0, 0 };
        std::atomic<uint64_t> prio_bytes[2]{ 0, 0 };
        std::atomic<uint64_t> dropped[2]{ 0, 0 };
        std::atomic<int>      cpu_load_pct{ 0 };
    };

    struct Telemetry {
        // Allocate independent 64-byte cache blocks for each CPU core
        std::array<CoreMetrics, 4> core_metrics{};

        // Diagnostics and control data
        std::atomic<bool> effective_bridge_mode{ false };
        std::atomic<bool> effective_acceleration{ true };
        std::atomic<double> cpu_temp_celsius{ 0.0 };
        // Global WAN shaper caps (Mb): GUI writes pending + dirty; QoS service applies to base_dl/ul + shapers.
        std::atomic<bool> qos_global_bw_dirty{ false };
        std::atomic<double> qos_global_dl_mbps_pending{ 500.0 };
        std::atomic<double> qos_global_ul_mbps_pending{ 50.0 };
        std::atomic<double> effective_qos_global_dl_mbps{ 500.0 };
        std::atomic<double> effective_qos_global_ul_mbps{ 50.0 };
        std::atomic<bool> mode_config_dirty{ false };
        std::atomic<bool> acceleration_pending{ true };

        // Traffic shaper: data plane fetch_add only; GUI reads the counters.
        std::atomic<uint64_t> shaper_normal_tx_complete{0};
        std::atomic<uint64_t> shaper_queue_overflow_drops{0};
        std::atomic<uint64_t> shaper_oversized_drops{0};

        // Raw AF_PACKET RX: bit0 = RawSocketManager::do_poll fatal; bit1 = App worker RX poll fatal.
        std::atomic<uint8_t> raw_socket_poll_errors{0};

        // Device table: scanned from /proc/net/arp by the telemetry service every 5s.
        static constexpr uint8_t MAX_TRACKED_DEVICES = 64;
        struct DeviceEntry {
            Utils::Net::IPv4Net ip{};
            std::array<char, 18> mac{};  // "xx:xx:xx:xx:xx:xx\0"
        };
        std::array<DeviceEntry, MAX_TRACKED_DEVICES> device_table{};
        std::atomic<uint8_t> device_count{0}; 
        std::atomic<uint64_t> device_table_revision{0};

        // System info: updated by the telemetry service every 5 seconds, read by UI thread on-demand.
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
            uint64_t prio_pkts[2] = { 0, 0 };
            uint64_t prio_bytes[2] = { 0, 0 };
            void reset() { *this = BatchStats{}; }
        };

        // Zero-contention batch commit: core N writes only to slot N.
        void commit_batch(const BatchStats& s, int core_id) {
            if (core_id < 0 || core_id >= 4) return;
            auto& m = core_metrics[core_id];
            
            m.pkts.fetch_add(s.pkts, std::memory_order_relaxed);
            m.bytes.fetch_add(s.bytes, std::memory_order_relaxed);
            
            for (int i = 0; i < 2; ++i) {
                m.prio_pkts[i].fetch_add(s.prio_pkts[i], std::memory_order_relaxed);
                m.prio_bytes[i].fetch_add(s.prio_bytes[i], std::memory_order_relaxed);
            }
        }
    };
}

