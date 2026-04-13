#pragma once
#include <atomic>
#include <chrono>
#include <array>
#include <span>
#include <cstring>
#include <algorithm>
#include <thread>
#include "Headers.hpp"
#include "Telemetry.hpp"
#include "Units.hpp"

namespace Scalpel::Traffic {

    // Token bucket rate limiter — kept inline (hot path, called every packet)
    class TokenBucket {
        double tokens;
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

        // Internal pending-rate slot; -1.0 is the sentinel "no pending change".
        alignas(64) std::atomic<double> requested_limit{-1.0};

    public:
        explicit TokenBucket(Mbps limit) { apply_new_rate(limit); }

    private:
        void apply_new_rate(Mbps limit) {
            rate_bytes_per_sec = (limit.value * 1e6) / 8.0;
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }

    public:
        void set_rate(Mbps limit) {
            requested_limit.store(limit.value, std::memory_order_release);
        }

        void refill() {
            double req_limit =
                requested_limit.exchange(-1.0, std::memory_order_acq_rel);
            if (req_limit >= 0.0)
                apply_new_rate(Mbps{req_limit});
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
            if (tokens >= bytes) { tokens -= bytes; return true; }
            return false;
        }

        void refund(uint32_t bytes) {
            tokens = std::min(capacity, tokens + static_cast<double>(bytes));
        }
    };

    // Zero dynamic allocation ring buffer — kept inline (template + hot path)
    template<size_t Capacity = 8192>
    class ZeroAllocRingBuffer {
        struct alignas(64) PacketSlot {
            uint16_t size = 0;
            uint8_t  payload[2048];
        };

        std::array<PacketSlot, Capacity> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;

    public:
        ZeroAllocRingBuffer() = default;

        bool push(std::span<const uint8_t> pkt) {
            if (count == Capacity || pkt.size() > 2048) return false;
            auto& slot = pool[tail];
            slot.size = static_cast<uint16_t>(pkt.size());
            std::memcpy(slot.payload, pkt.data(), pkt.size());
            tail = (tail + 1) % Capacity;
            count++;
            return true;
        }

        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool[head];
            return { slot.payload, slot.size };
        }

        void pop() {
            if (count > 0) { head = (head + 1) % Capacity; count--; }
        }

        bool   empty() const { return count == 0; }
        size_t size()  const { return count; }
    };

    // Low-level hardware send result
    enum class TxResult : size_t { Success = 0, Congested = 1, Fatal = 2 };

    // Traffic shaper — non-trivial methods defined in Scheduler.cpp
    class Shaper {
        ZeroAllocRingBuffer<8192> normal_queue;
        TokenBucket               bucket;

        using ResultHandler = void (*)(Shaper*, size_t);
        static constexpr std::array<ResultHandler, 3> result_handlers = {
            [](Shaper* s, size_t) {
                s->normal_queue.pop();
                Telemetry::instance().shaper_normal_tx_complete.fetch_add(
                    1, std::memory_order_relaxed);
            },
            [](Shaper* s, size_t bytes) {
                s->bucket.refund(bytes);
            },
            [](Shaper* s, size_t bytes) {
                s->bucket.refund(bytes);
                s->normal_queue.pop();
            }
        };

    public:
        explicit Shaper(Mbps limit) : bucket(limit) {}

        void set_rate_limit(Mbps limit);
        void enqueue_normal(std::span<const uint8_t> pkt);
        void process_queue(int tx_fd);
    };
}
