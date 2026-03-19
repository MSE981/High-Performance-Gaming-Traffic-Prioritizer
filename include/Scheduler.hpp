#pragma once
#include <chrono>
#include <vector>
#include <span>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <cerrno>
#include <thread>
#include <print>
#include <array>
#include "Headers.hpp"
#include "Telemetry.hpp"

namespace Scalpel::Traffic {

    // 1. 令牌桶算法
    class TokenBucket {
        double tokens;
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

    public:
        explicit TokenBucket(double limit_mbps) {
            rate_bytes_per_sec = (limit_mbps * 1e6) / 8.0;
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }

        void refill() {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> dt = now - last_refill;
            double new_tokens = dt.count() * rate_bytes_per_sec;

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

        void refund(uint32_t bytes) {
            tokens = std::min(capacity, tokens + static_cast<double>(bytes));
        }

        void set_rate(double limit_mbps) {
            rate_bytes_per_sec = (limit_mbps * 1e6) / 8.0;
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }
    };

    // 2. 零动态分配环形缓冲区
    class ZeroAllocRingBuffer {
        struct alignas(4096) PacketSlot {
            uint16_t size = 0;
            uint8_t payload[2048];
        };

        std::vector<PacketSlot> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;
        const size_t capacity_limit = 8192;

    public:
        ZeroAllocRingBuffer() : pool(8192) {}

        bool push(std::span<const uint8_t> pkt) {
            if (count == capacity_limit || pkt.size() > 2048) {
                return false;
            }
            auto& slot = pool[tail];
            slot.size = static_cast<uint16_t>(pkt.size());
            std::memcpy(slot.payload, pkt.data(), pkt.size());
            tail = (tail + 1) % capacity_limit;
            count++;
            return true;
        }

        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool[head];
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

    // 封装底层硬件发送结果
    enum class TxResult : size_t { Success = 0, Congested = 1, Fatal = 2 };

    inline TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
        for (int retries = 3; retries > 0; --retries, std::this_thread::yield()) {
            if (send(fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) return TxResult::Success;
            if (errno != ENOBUFS && errno != EAGAIN) return TxResult::Fatal;
        }
        return TxResult::Congested;
    }

    // 3. 流量整形器
    class Shaper {
        ZeroAllocRingBuffer normal_queue;
        TokenBucket bucket;
        uint64_t trace_counter = 0;

        using ResultHandler = void (*)(Shaper*, size_t);
        static constexpr std::array<ResultHandler, 3> result_handlers = { [](Shaper* s, size_t) {
                s->normal_queue.pop();
                if (++s->trace_counter % 5000 == 0) {
                    std::println("[Proof-of-Work] Successfully shaped and forwarded 5000 normal packets.");
                }
            },[](Shaper* s, size_t bytes) {
                s->bucket.refund(bytes);
            },[](Shaper* s, size_t bytes) {
                s->bucket.refund(bytes);
                s->normal_queue.pop();
            }
        };

    public:
        // 核心修复：找回丢失的构造函数！
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        void set_rate_limit(double limit_mbps) {
            bucket.set_rate(limit_mbps);
        }

        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                Telemetry::instance().dropped_normal.fetch_add(1, std::memory_order_relaxed);

                static time_t last_log = 0;
                time_t now = time(nullptr);
                if (now != last_log) {
                    static constexpr std::array<const char*, 2> diag_msgs = {
                        "\n[Diag] Drop N: 8192 Queue is FULL! Shaper is throttling.",
                        "\n[Diag] Drop N: Packet too large. GRO NOT turned off!"
                    };
                    std::println(stderr, "{}", diag_msgs[pkt.size() > 2048]);
                    last_log = now;
                }
            }
        }

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