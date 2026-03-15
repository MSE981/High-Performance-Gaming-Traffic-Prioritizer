#pragma once
#include <atomic>
#include <cstdint>

namespace Scalpel {
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