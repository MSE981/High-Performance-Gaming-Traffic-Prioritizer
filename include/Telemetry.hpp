#pragma once
#include <atomic>
#include <cstdint>

namespace Scalpel {
    // 单例模式：线程安全的全局统计
    struct Telemetry {
        // 流量统计 (Relaxed ordering is sufficient)
        std::atomic<uint64_t> pkts_forwarded{ 0 };
        std::atomic<uint64_t> bytes_forwarded{ 0 };

        // 诊断数据
        std::atomic<double> internal_limit_mbps{ 0.0 };
        std::atomic<double> isp_limit_mbps{ 0.0 };
        std::atomic<bool> is_probing{ false };

        // 线程心跳 (用于 Watchdog)
        std::atomic<uint64_t> last_heartbeat_core2{ 0 };
        std::atomic<uint64_t> last_heartbeat_core3{ 0 };

        static Telemetry& instance() {
            static Telemetry inst;
            return inst;
        }
    };
}