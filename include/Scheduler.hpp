#pragma once
#include <chrono>
#include <vector>
#include <span>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include "Headers.hpp"
#include "Telemetry.hpp" // 用于记录主动丢包(AQM)的统计

namespace Scalpel::Traffic {

    // 1. 令牌桶算法：精确控制普通流量的最高带宽
    class TokenBucket {
        uint64_t tokens;
        uint64_t capacity;
        uint64_t rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

    public:
        explicit TokenBucket(double limit_mbps) {
            // 将 Mbps 转换为 每秒字节数
            rate_bytes_per_sec = static_cast<uint64_t>((limit_mbps * 1e6) / 8.0);

            // 桶容量设定：允许的最大突发量（约 20ms 的数据量，且至少容纳 10 个满载 MTU）
            capacity = std::max<uint64_t>(15000, rate_bytes_per_sec * 0.02);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }

        void refill() {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> dt = now - last_refill;
            uint64_t new_tokens = static_cast<uint64_t>(dt.count() * rate_bytes_per_sec);

            if (new_tokens > 0) {
                tokens = std::min(capacity, tokens + new_tokens);
                last_refill = now;
            }
        }

        bool try_consume(uint32_t bytes) {
            refill();
            if (tokens >= bytes) {
                tokens -= bytes;
                return true;
            }
            return false;
        }
    };

    // 2. 零动态分配环形缓冲区 (4MB 完美对齐版)
    class ZeroAllocRingBuffer {
        // 核心黑科技：alignas(4096) 
        // 强制编译器将每个槽位对齐到 4KB (树莓派系统页大小)。
        // 彻底消除 TLB Miss，大幅提升 CPU 寻址效率。
        struct alignas(4096) PacketSlot {
            uint16_t size = 0;
            uint8_t payload; // 足够装下标准的 1514 字节 MTU
        };

        // 运行时永不扩容，启动时一次性在堆上划拨 4MB 连续内存
        std::vector<PacketSlot> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;
        const size_t capacity_limit = 1024;

    public:
        ZeroAllocRingBuffer() : pool(1024) {}

        bool push(std::span<const uint8_t> pkt) {
            // AQM 机制：队列满时实施 Tail Drop，主动丢弃下载包触发 TCP 降速
            if (count == capacity_limit || pkt.size() > 2048) {
                return false;
            }

            auto& slot = pool;
            slot.size = static_cast<uint16_t>(pkt.size());
            // 极速内存拷贝
            std::memcpy(slot.payload, pkt.data(), pkt.size());

            tail = (tail + 1) % capacity_limit;
            count++;
            return true;
        }

        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool;
            return { slot.payload, slot.size };
        }

        void pop() {
            if (count > 0) {
                head = (head + 1) % capacity_limit;
                count--;
            }
        }

        bool empty() const { return count == 0; }
    };

    // 3. 流量整形器
    class Shaper {
        ZeroAllocRingBuffer normal_queue;
        TokenBucket bucket;

    public:
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        // 将普通包送入缓存排队
        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                // 如果 push 失败（队列已满），记录一次主动丢包
                Telemetry::instance().dropped_pkts.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 抽空队列：由 worker 线程在每一轮循环中调用
        void process_queue(int tx_fd) {
            while (!normal_queue.empty()) {
                auto pkt_span = normal_queue.front();

                // 检查令牌是否足够发这个包
                if (bucket.try_consume(pkt_span.size())) {
                    send(tx_fd, pkt_span.data(), pkt_span.size(), MSG_DONTWAIT);
                    normal_queue.pop();
                }
                else {
                    break; // 令牌耗尽，退出循环让包继续排队
                }
            }
        }
    };
}