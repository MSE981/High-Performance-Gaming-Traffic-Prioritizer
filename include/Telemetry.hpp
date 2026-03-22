#pragma once
#include <atomic>
#include <cstdint>
#include <array>

namespace Scalpel {

    // 核心指标槽位 (L1 Cache Line 对齐)
    // 强制对齐到 64 字节，确保各 CPU 核心在更新各自统计数据时不会触发 Cache Line Bouncing。
    struct alignas(64) CoreMetrics {
        std::atomic<uint64_t> pkts{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<uint64_t> prio_pkts[3]{ 0, 0, 0 };
        std::atomic<uint64_t> prio_bytes[3]{ 0, 0, 0 };
        std::atomic<uint64_t> dropped[3]{ 0, 0, 0 };
        std::atomic<uint64_t> last_heartbeat{ 0 };
    };

    struct Telemetry {
        // 为每个 CPU 核心分配独立的 64 字节缓存块
        std::array<CoreMetrics, 4> core_metrics{};

        // 诊断与控制数据 (低频读写，无需分流)
        std::atomic<double> internal_limit_mbps{ 0.0 };
        std::atomic<double> isp_down_limit_mbps{ 0.0 };
        std::atomic<double> isp_up_limit_mbps{ 0.0 };
        std::atomic<double> internal_pps{ 0.0 };
        std::atomic<double> isp_pps{ 0.0 };
        std::atomic<bool> is_probing{ false };
        std::atomic<bool> bridge_mode{ false };

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

        /**
         * @brief 零竞争批量提交
         * @param s 批量统计结果
         * @param core_id 当前 CPU 核心 ID
         * @details 写入端完全隔离：核心 2 仅写入槽位 2，核心 3 仅写入槽位 3。
         */
        void commit_batch(const BatchStats& s, int core_id) {
            if (core_id < 0 || core_id >= 4) return;
            auto& m = core_metrics[core_id];
            
            m.pkts.fetch_add(s.pkts, std::memory_order_relaxed);
            m.bytes.fetch_add(s.bytes, std::memory_order_relaxed);
            
            for (int i = 0; i < 3; ++i) {
                m.prio_pkts[i].fetch_add(s.prio_pkts[i], std::memory_order_relaxed);
                m.prio_bytes[i].fetch_add(s.prio_bytes[i], std::memory_order_relaxed);
            }
            // 移除了此处的 time(nullptr) 系统调用，心跳滴答交由空闲循环自增
        }
    };
}

