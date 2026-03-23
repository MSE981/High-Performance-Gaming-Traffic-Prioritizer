#pragma once
#include <chrono>
#include <array>
#include <span>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <cerrno>
#include <thread>
#include <print>
#include "Headers.hpp"
#include "Telemetry.hpp"

namespace Scalpel::Traffic {

    // 令牌桶速率限制器
    class TokenBucket {
        double tokens;
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

        // Phase 2.4 RCU: 无锁配置桥接
        alignas(64) std::atomic<double> requested_limit{-1.0};

    public:
        explicit TokenBucket(double limit_mbps) {
            apply_new_rate(limit_mbps);
        }

    private:
        void apply_new_rate(double limit_mbps) {
            rate_bytes_per_sec = (limit_mbps * 1e6) / 8.0;
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }

    public:
        // 控制面被动接口：无锁推入更新指令
        void set_rate(double limit_mbps) {
            requested_limit.store(limit_mbps, std::memory_order_release);
        }

        // 数据面实时泵：原子嗅探变更，100% 极低开销
        void refill() {
            double req_limit = requested_limit.load(std::memory_order_acquire);
            if (req_limit >= 0.0) {
                apply_new_rate(req_limit); // 吸收更新
                requested_limit.store(-1.0, std::memory_order_relaxed);
            }

            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> dt = now - last_refill;
            double new_tokens = dt.count() * rate_bytes_per_sec;

            if (new_tokens > 0) {
                tokens = std::min(capacity, tokens + new_tokens);
                last_refill = now;
            }
        }

        // 尝试消耗令牌
        bool try_consume(uint32_t bytes) {
            refill();
            if (tokens >= bytes) {
                tokens -= bytes;
                return true;
            }
            return false;
        }

        // 硬件发送失败时归还令牌
        void refund(uint32_t bytes) {
            tokens = std::min(capacity, tokens + static_cast<double>(bytes));
        }

        // 此函数改由 set_rate 控制热刷新，故移除原逻辑

    };

    // 零动态分配环形缓冲区 (针对特定 Capacity 优化)
    // 使用 std::array 替代 vector，PacketSlot 采用缓存行对齐防止伪共享。
    template<size_t Capacity = 8192>
    class ZeroAllocRingBuffer {
        struct alignas(64) PacketSlot {
            uint16_t size = 0;
            uint8_t payload[2048];
        };

        std::array<PacketSlot, Capacity> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;

    public:
        ZeroAllocRingBuffer() = default;

        // 压入包数据
        bool push(std::span<const uint8_t> pkt) {
            if (count == Capacity || pkt.size() > 2048) {
                return false;
            }
            auto& slot = pool[tail];
            slot.size = static_cast<uint16_t>(pkt.size());
            std::memcpy(slot.payload, pkt.data(), pkt.size());
            tail = (tail + 1) % Capacity;
            count++;
            return true;
        }

        // 获取首部数据块
        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool[head];
            return { slot.payload, slot.size };
        }

        // 弹出首部项
        void pop() {
            if (count > 0) {
                head = (head + 1) % Capacity;
                count--;
            }
        }

        bool empty() const { return count == 0; }
        size_t size() const { return count; }
    };

    // 底层硬件发送结果定义
    enum class TxResult : size_t { Success = 0, Congested = 1, Fatal = 2 };

    // 硬件发送重试抽象 (零阻塞拦截)
    inline TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
        if (send(fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) return TxResult::Success;
        if (errno == ENOBUFS || errno == EAGAIN) return TxResult::Congested;
        return TxResult::Fatal;
    }

    // 流量整形器
    // 处理普通流量的入队、限速控制与硬件分发。
    class Shaper {
        ZeroAllocRingBuffer<8192> normal_queue;
        TokenBucket bucket;
        uint64_t trace_counter = 0;

        // 发送结果处理器表 (Functor Table)
        using ResultHandler = void (*)(Shaper*, size_t);
        static constexpr std::array<ResultHandler, 3> result_handlers = {
            [](Shaper* s, size_t) { // Success
                s->normal_queue.pop();
                if (++s->trace_counter % 5000 == 0) {
                    std::println("[Shaper] 已稳定转发 5000 个普通包。");
                }
            },
            [](Shaper* s, size_t bytes) { // Congested (硬件忙，归还令牌但不弹出，待下次重试)
                s->bucket.refund(bytes);
            },
            [](Shaper* s, size_t bytes) { // Fatal (严重错误，丢弃包)
                s->bucket.refund(bytes);
                s->normal_queue.pop();
            }
        };

    public:
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        void set_rate_limit(double limit_mbps) {
            bucket.set_rate(limit_mbps);
        }

        // 普通流量入队逻辑
        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                Telemetry::instance().dropped_normal.fetch_add(1, std::memory_order_relaxed);
                
                static time_t last_log = 0;
                time_t now = time(nullptr);
                if (now != last_log) {
                    if (pkt.size() > 2048) {
                        std::println(stderr, "\n[Alert] 丢包: 包过大 ({} bytes)，硬件卸载可能未完全关闭！", pkt.size());
                    } else {
                        std::println(stderr, "\n[Alert] 丢包: 队列溢出 (Cap: 8192)，并发过载。");
                    }
                    last_log = now;
                }
            }
        }

        // 队列消费主流程 (被 App 驱动层周期性调用)
        void process_queue(int tx_fd) {
            while (!normal_queue.empty()) {
                auto pkt_span = normal_queue.front();

                if (!bucket.try_consume(pkt_span.size())) break;

                TxResult res = try_hardware_send(tx_fd, pkt_span);
                result_handlers[static_cast<size_t>(res)](this, pkt_span.size());

                if (res == TxResult::Congested) break;
            }
        }
    };
}



