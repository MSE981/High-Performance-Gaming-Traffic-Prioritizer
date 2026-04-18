#pragma once
#include <atomic>
#include <cstdint>
#include <array>
#include <cstring>
#include <expected>
#include <string>
#include <mutex>
#include "NetworkTypes.hpp"

namespace HPGTP {

    // Core metrics slot (L1 cache line aligned)
    // Forced alignment to 64 bytes ensures each CPU core's stats updates don't trigger cache line bouncing
    struct alignas(64) CoreMetrics {
        std::atomic<uint64_t> pkts{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<uint64_t> prio_pkts[3]{ 0, 0, 0 };
        std::atomic<uint64_t> prio_bytes[3]{ 0, 0, 0 };
        std::atomic<uint64_t> dropped[3]{ 0, 0, 0 };
        std::atomic<uint64_t> last_heartbeat{ 0 };
        std::atomic<int>      cpu_load_pct{ 0 };  // 0-100, updated by watchdog 1Hz via /proc/stat
    };

    struct Telemetry {
        // Allocate independent 64-byte cache blocks for each CPU core
        std::array<CoreMetrics, 4> core_metrics{};

        // Diagnostics and control data (low-frequency read/write, no need for separation)
        std::atomic<bool> bridge_mode{ false }; // legacy mirror of effective_bridge_mode
        std::atomic<bool> effective_bridge_mode{ false };
        std::atomic<bool> effective_acceleration{ true };
        std::atomic<double> cpu_temp_celsius{ 0.0 };  // updated by Core 1 watchdog via timerfd, read by Qt UI
        std::atomic<int> qos_throttle_pct{ 85 };     // 0–100, written by GUI slider (Core 0), applied by Core 1 watchdog
        // Global WAN shaper caps (Mbps): GUI writes pending + dirty; Core 1 watchdog applies to base_dl/ul + shapers.
        std::atomic<bool> qos_global_bw_dirty{ false };
        std::atomic<double> qos_global_dl_mbps_pending{ 500.0 };
        std::atomic<double> qos_global_ul_mbps_pending{ 50.0 };
        std::atomic<double> effective_qos_global_dl_mbps{ 500.0 };
        std::atomic<double> effective_qos_global_ul_mbps{ 50.0 };
        std::atomic<bool> mode_config_dirty{ false };
        std::atomic<bool> acceleration_pending{ true };
        std::atomic<bool> dhcp_config_dirty{ false }; // set by GUI (Core 0), consumed by Core 1 watchdog
        std::atomic<bool> dns_config_dirty{ false };  // set by GUI (Core 0), consumed by Core 1 watchdog
        struct DnsPendingRecord {
            std::array<char, 64> hostname{};
            std::array<char, 16> ip_str{};
        };
        std::mutex dns_pending_mutex;
        std::string dns_upstream_primary_pending = "8.8.8.8";
        std::string dns_upstream_secondary_pending = "8.8.4.4";
        bool dns_redirect_pending = false;
        std::array<DnsPendingRecord, 64> dns_static_pending{};
        size_t dns_static_pending_count = 0;

        // Traffic shaper: data plane fetch_add only; watchdog prints 1 Hz deltas.
        std::atomic<uint64_t> shaper_normal_tx_complete{0};
        std::atomic<uint64_t> shaper_queue_overflow_drops{0};
        std::atomic<uint64_t> shaper_oversized_drops{0};

        // Raw AF_PACKET RX: bit0 = RawSocketManager::do_poll fatal; bit1 = App worker RX poll fatal.
        std::atomic<uint8_t> raw_socket_poll_errors{0};

        // Firewall conntrack: track_outbound could not insert (linear probe exhausted).
        std::atomic<uint64_t> conntrack_track_drops{0};

        // Device table: scanned from /proc/net/arp by Core 1 watchdog every 5s.
        // Plain char arrays — torn reads acceptable for display-only data.
        static constexpr uint8_t MAX_TRACKED_DEVICES = 64;
        struct DeviceEntry {
            Net::IPv4Net ip{};
            std::array<char, 18> mac{};  // "xx:xx:xx:xx:xx:xx\0"
        };
        std::array<DeviceEntry, MAX_TRACKED_DEVICES> device_table{};
        std::atomic<uint8_t> device_count{0}; // release-stored last after all entries are written

        // System info: updated by Core 1 watchdog every 5 seconds, read by UI thread on-demand.
        // char arrays are plain (not atomic) — display-only data, torn reads are acceptable.
        struct SystemInfo {
            std::array<char, 64>  hostname{};
            std::array<char, 128> kernel_short{};
            std::atomic<uint64_t> uptime_seconds{0};
            std::atomic<uint64_t> mem_total_kb{0};
            std::atomic<uint64_t> mem_avail_kb{0};

            // Network interface cache: Core 1 watchdog scans /sys/class/net every 5s.
            // Qt UI reads iface_count + ifaces[] — zero file I/O on Core 0.
            static constexpr uint8_t MAX_IFACES = 8;
            struct IfaceEntry {
                std::array<char, 16> name{};       // e.g. "eth0"
                std::array<char, 8>  operstate{};  // "up", "down", "unknown"
            };
            std::array<IfaceEntry, MAX_IFACES> ifaces{};
            std::atomic<uint8_t> iface_count{0};  // written last (release), read first (acquire)

            // On-demand rescan signalling via eventfd pair.
            // Initialised once before any thread starts; thereafter accessed only through methods.
            std::expected<void, std::string> init_event_fds();

            // UI (Core 0) interface — no raw fd values exposed
            void request_rescan();           // signal watchdog to re-scan immediately
            int  done_notifier_fd() const;   // fd for QSocketNotifier construction (read-once at UI init)
            void consume_done();             // drain done_fd after QSocketNotifier fires

            // Watchdog (Core 1) interface
            int  rescan_poll_fd() const;     // fd to put in poll() pfd array
            void consume_rescan();           // drain rescan_fd after poll() fires
            void signal_done();              // notify UI that rescan is complete

        private:
            // Raw eventfd descriptors — hidden from all callers
            int rescan_fd_ = -1;
            int done_fd_   = -1;
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

