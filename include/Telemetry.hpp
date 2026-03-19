#pragma once
#include <atomic>
#include <cstdint>

namespace Scalpel {
    struct Telemetry {
        // 流量统计 (分离上下行)
        std::atomic<uint64_t> pkts_down{ 0 };
        std::atomic<uint64_t> bytes_down{ 0 };
        std::atomic<uint64_t> pkts_up{ 0 };
        std::atomic<uint64_t> bytes_up{ 0 };

        // 分级统计
        std::atomic<uint64_t> pkts_critical{ 0 };
        std::atomic<uint64_t> pkts_high{ 0 };
        std::atomic<uint64_t> pkts_normal{ 0 };
        std::atomic<uint64_t> bytes_critical{ 0 };
        std::atomic<uint64_t> bytes_high{ 0 };
        std::atomic<uint64_t> bytes_normal{ 0 };

        // 诊断数据
        std::atomic<double> internal_limit_mbps{ 0.0 };
        std::atomic<double> isp_down_limit_mbps{ 0.0 };
        std::atomic<double> isp_up_limit_mbps{ 0.0 };
        std::atomic<double> internal_pps{ 0.0 };
        std::atomic<double> isp_pps{ 0.0 };
        std::atomic<bool> is_probing{ false };

        // 线程隔离心跳与丢包 (消除伪共享)
        alignas(64) std::atomic<uint64_t> last_heartbeat_core2{ 0 };
        alignas(64) std::atomic<uint64_t> last_heartbeat_core3{ 0 };
        alignas(64) std::atomic<uint64_t> dropped_critical{ 0 };
        alignas(64) std::atomic<uint64_t> dropped_high{ 0 };
        alignas(64) std::atomic<uint64_t> dropped_normal{ 0 };

        static Telemetry& instance() {
            static Telemetry inst;
            return inst;
        }

        // 数据驱动的批量统计结构体
        struct BatchStats {
            uint64_t pkts = 0, bytes = 0;
            uint64_t prio_pkts[3] = { 0, 0, 0 };
            uint64_t prio_bytes[3] = { 0, 0, 0 };
            void reset() { *this = BatchStats{}; }
        };

        // O(1) 批量提交接口
        void commit_batch(const BatchStats& s, bool is_download) {
            if (is_download) {
                pkts_down.fetch_add(s.pkts, std::memory_order_relaxed);
                bytes_down.fetch_add(s.bytes, std::memory_order_relaxed);
            }
            else {
                pkts_up.fetch_add(s.pkts, std::memory_order_relaxed);
                bytes_up.fetch_add(s.bytes, std::memory_order_relaxed);
            }
            pkts_critical.fetch_add(s.prio_pkts[0], std::memory_order_relaxed);
            bytes_critical.fetch_add(s.prio_bytes[0], std::memory_order_relaxed);
            pkts_high.fetch_add(s.prio_pkts[1], std::memory_order_relaxed);
            bytes_high.fetch_add(s.prio_bytes[1], std::memory_order_relaxed);
            pkts_normal.fetch_add(s.prio_pkts[2], std::memory_order_relaxed);
            bytes_normal.fetch_add(s.prio_bytes[2], std::memory_order_relaxed);
        }
    };
}