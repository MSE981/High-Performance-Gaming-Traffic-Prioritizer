#pragma once
#include <atomic>
#include <cstdint>

namespace Scalpel {
    struct BatchStats {
        uint64_t pkts = 0, bytes = 0;
        uint64_t pkts_crit = 0, bytes_crit = 0;
        uint64_t pkts_high = 0, bytes_high = 0;
        uint64_t pkts_norm = 0, bytes_norm = 0;
        void reset() { *this = BatchStats{}; }
    };

    // 在 Telemetry 类中增加一个封装好的提交方法
    void commit_batch(const BatchStats& s, bool is_download) {
        if (is_download) {
            pkts_down.fetch_add(s.pkts, std::memory_order_relaxed);
            bytes_down.fetch_add(s.bytes, std::memory_order_relaxed);
        }
        else {
            pkts_up.fetch_add(s.pkts, std::memory_order_relaxed);
            bytes_up.fetch_add(s.bytes, std::memory_order_relaxed);
        }
        pkts_critical.fetch_add(s.pkts_crit, std::memory_order_relaxed);
        bytes_critical.fetch_add(s.bytes_crit, std::memory_order_relaxed);
        pkts_high.fetch_add(s.pkts_high, std::memory_order_relaxed);
        bytes_high.fetch_add(s.bytes_high, std::memory_order_relaxed);
        pkts_normal.fetch_add(s.pkts_norm, std::memory_order_relaxed);
        bytes_normal.fetch_add(s.bytes_norm, std::memory_order_relaxed);
    }
    // 单例模式：线程安全的全局统计
    struct Telemetry {
        // 流量统计 (Relaxed ordering is sufficient)
        std::atomic<uint64_t> pkts_down{ 0 };
        std::atomic<uint64_t> bytes_down{ 0 };
        std::atomic<uint64_t> pkts_up{ 0 };
        std::atomic<uint64_t> bytes_up{ 0 };

        // 分级 PPS 统计
        std::atomic<uint64_t> pkts_critical{ 0 };
        std::atomic<uint64_t> pkts_high{ 0 };
        std::atomic<uint64_t> pkts_normal{ 0 };

        // 分级带宽统计 (Bytes)
        std::atomic<uint64_t> bytes_critical{ 0 };
        std::atomic<uint64_t> bytes_high{ 0 };
        std::atomic<uint64_t> bytes_normal{ 0 };

        // 诊断数据
        std::atomic<double> internal_limit_mbps{ 0.0 };
        std::atomic<double> isp_limit_mbps{ 0.0 };
        std::atomic<double> isp_down_limit_mbps{ 0.0 };
        std::atomic<double> isp_up_limit_mbps{ 0.0 };
        std::atomic<double> internal_pps{ 0.0 };
        std::atomic<double> isp_pps{ 0.0 };
        std::atomic<bool> is_probing{ false };

        // 线程心跳 (用于 Watchdog)
        alignas(64) std::atomic<uint64_t> last_heartbeat_core2{ 0 };
        alignas(64) std::atomic<uint64_t> last_heartbeat_core3{ 0 };

        // 主动丢包计数 (用于监控 AQM 效果)
        // 分级丢包计数 (涵盖 AQM 主动丢弃与网卡物理丢弃)
        alignas(64) std::atomic<uint64_t> dropped_critical{ 0 };
        alignas(64) std::atomic<uint64_t> dropped_high{ 0 };
        alignas(64) std::atomic<uint64_t> dropped_normal{ 0 };
        static Telemetry& instance() {
            static Telemetry inst;
            return inst;
        }
    };
}