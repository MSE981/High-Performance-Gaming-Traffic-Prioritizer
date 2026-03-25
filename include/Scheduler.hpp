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

    // Token bucket rate limiter
    class TokenBucket {
        double tokens;
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

        // Phase 2.4 RCU: lock-free config bridge
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
        // Control plane passive interface: lock-free update enqueue
        void set_rate(double limit_mbps) {
            requested_limit.store(limit_mbps, std::memory_order_release);
        }

        // Data plane real-time pump: atomic sniff for changes, ultra-low overhead
        void refill() {
            double req_limit = requested_limit.load(std::memory_order_acquire);
            if (req_limit >= 0.0) {
                apply_new_rate(req_limit); // Absorb update
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

        // Try to consume tokens
        bool try_consume(uint32_t bytes) {
            refill();
            if (tokens >= bytes) {
                tokens -= bytes;
                return true;
            }
            return false;
        }

        // Refund tokens on hardware send failure
        void refund(uint32_t bytes) {
            tokens = std::min(capacity, tokens + static_cast<double>(bytes));
        }

        // Hot refresh now controlled by set_rate(), original logic removed

    };

    // Zero dynamic allocation ring buffer (optimized for specific capacity)
    // Use std::array instead of vector, PacketSlot aligned to cache line to prevent false sharing
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

        // Push packet data
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

        // Get front data block
        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool[head];
            return { slot.payload, slot.size };
        }

        // Pop front item
        void pop() {
            if (count > 0) {
                head = (head + 1) % Capacity;
                count--;
            }
        }

        bool empty() const { return count == 0; }
        size_t size() const { return count; }
    };

    // Low-level hardware send result definition
    enum class TxResult : size_t { Success = 0, Congested = 1, Fatal = 2 };

    // Hardware send retry abstraction (zero-blocking intercept)
    inline TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
        if (send(fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) return TxResult::Success;
        if (errno == ENOBUFS || errno == EAGAIN) return TxResult::Congested;
        return TxResult::Fatal;
    }

    // Traffic shaper
    // Handles normal traffic queue, rate limiting, and hardware dispatch
    class Shaper {
        ZeroAllocRingBuffer<8192> normal_queue;
        TokenBucket bucket;
        uint64_t trace_counter = 0;

        // Send result handler table (Functor table)
        using ResultHandler = void (*)(Shaper*, size_t);
        static constexpr std::array<ResultHandler, 3> result_handlers = {
            [](Shaper* s, size_t) { // Success
                s->normal_queue.pop();
                if (++s->trace_counter % 5000 == 0) {
                    std::println("[Shaper] Stably forwarded 5000 normal packets.");
                }
            },
            [](Shaper* s, size_t bytes) { // Congested: hardware busy, refund tokens but don't pop, retry next time
                s->bucket.refund(bytes);
            },
            [](Shaper* s, size_t bytes) { // Fatal: serious error, discard packet
                s->bucket.refund(bytes);
                s->normal_queue.pop();
            }
        };

    public:
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        void set_rate_limit(double limit_mbps) {
            bucket.set_rate(limit_mbps);
        }

        // Normal traffic enqueue logic
        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                static std::atomic<uint64_t> drop_count{0};
                uint64_t c = drop_count.fetch_add(1, std::memory_order_relaxed);
                if (c % 1000 == 0) {
                    if (pkt.size() > 2048)
                        std::println(stderr, "[Alert] Drop: oversized packet ({} bytes), hw offload may be active.", pkt.size());
                    else
                        std::println(stderr, "[Alert] Drop: queue overflow (capacity 8192), total drops: {}.", c);
                }
            }
        }

        // Queue consumption main loop (called periodically by App drive layer)
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



