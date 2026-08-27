#pragma once
#include <atomic>
#include <chrono>
#include <array>
#include <span>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include "Headers_util.hpp"
#include "Telemetry.hpp"
#include "Units_util.hpp"

namespace HPGTP::Traffic {

    // Token bucket rate limiter - kept inline (hot path, called every packet)
    class TokenBucket {
        double tokens;
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

        // Internal pending-rate slot
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

    // Zero dynamic allocation ring buffer - need for speed~！~！
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

    using TxResultCallback = std::function<void(TxResult, size_t)>;

    // Traffic shaper
    class Shaper {
        ZeroAllocRingBuffer<8192> normal_queue;
        TokenBucket               bucket;
        std::atomic_flag          spin_{};

        void lock_spin() {
            while (spin_.test_and_set(std::memory_order_acquire)) { }
        }
        void unlock_spin() { spin_.clear(std::memory_order_release); }

        std::array<std::function<void(size_t)>, 3> result_handlers_;

    public:
        explicit Shaper(Mbps limit) : bucket(limit) {
            result_handlers_[0] = [this](size_t) {
                normal_queue.pop();
                Telemetry::instance().shaper_normal_tx_complete.fetch_add(
                    1, std::memory_order_relaxed);
            };
            result_handlers_[1] = [this](size_t bytes) {
                bucket.refund(bytes);
            };
            result_handlers_[2] = [this](size_t bytes) {
                bucket.refund(bytes);
                normal_queue.pop();
            };
        }

        void set_rate_limit(Mbps limit);
        // Register a subscriber for every drain attempt (startup only).
        void set_tx_result_callback(TxResultCallback cb);
        void enqueue_normal(std::span<const uint8_t> pkt);
        void process_queue(int tx_fd);

    private:
        TxResultCallback tx_callback_{};
    };
}
