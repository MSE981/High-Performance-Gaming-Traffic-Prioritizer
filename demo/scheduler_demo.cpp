// scheduler_demo: verify TokenBucket rate limiting and Shaper queue drain
//
// Build: make scheduler_demo
// Run:   ./scheduler_demo   (no root required -- uses fd=-1, send() fails gracefully)
#include "Scheduler.hpp"
#include <print>
#include <cassert>
#include <array>
#include <memory>

int main() {
    std::println("=== Scheduler / TokenBucket Demo ===");

    // 1. TokenBucket: verify token consumption and refund
    {
        HPGTP::Traffic::TokenBucket bucket(HPGTP::Traffic::Mbps{10.0});  // 10 Mbps

        // First consume should succeed (bucket starts full)
        bool first = bucket.try_consume(1000);
        assert(first && "Initial consume should succeed");
        std::println("[PASS] TokenBucket: initial consume succeeded.");

        // Drain the bucket completely then try again
        for (int i = 0; i < 200; ++i) bucket.try_consume(8000);
        bool drained = bucket.try_consume(8000);
        // May or may not succeed depending on refill time -- just print result
        std::println("[INFO] TokenBucket after drain: try_consume(8000) = {}", drained);

        // Refund and re-consume
        bucket.refund(8000);
        bool after_refund = bucket.try_consume(8000);
        assert(after_refund && "After refund, consume should succeed");
        std::println("[PASS] TokenBucket: refund + consume succeeded.");
    }

    // 2. ZeroAllocRingBuffer: push/pop round-trip
    {
        HPGTP::Traffic::ZeroAllocRingBuffer<4> ring;
        assert(ring.empty());

        std::array<uint8_t, 64> pkt{};
        pkt[0] = 0xAA;
        bool pushed = ring.push(std::span<const uint8_t>{pkt});
        assert(pushed && "push should succeed on empty ring");
        assert(!ring.empty());
        assert(ring.size() == 1);

        auto front = ring.front();
        assert(front.size() == 64 && front[0] == 0xAA);
        ring.pop();
        assert(ring.empty());

        std::println("[PASS] ZeroAllocRingBuffer: push/front/pop verified.");
    }

    // 3. Shaper: enqueue then process_queue (tx_fd = -1, sends will fail)
    {
        auto shaper = std::make_unique<HPGTP::Traffic::Shaper>(HPGTP::Traffic::Mbps{1000.0});

        std::array<uint8_t, 128> pkt{};
        shaper->enqueue_normal(std::span<const uint8_t>{pkt});
        shaper->enqueue_normal(std::span<const uint8_t>{pkt});

        // process_queue with invalid fd: try_hardware_send returns Fatal,
        // packet is discarded and popped; queue should drain without hang.
        shaper->process_queue(-1);

        std::println("[PASS] Shaper: enqueue + process_queue with invalid fd completed.");
    }

    std::println("=== Done ===");
    return 0;
}
