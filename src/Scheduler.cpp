#include "Scheduler.hpp"
#include "DataPlane.hpp"
#include <print>

namespace Scalpel::Traffic {

static TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
    return DataPlane::TxFrameOutput::send_blocking(fd, pkt) ? TxResult::Success
                                                             : TxResult::Fatal;
}

void Shaper::set_rate_limit(Mbps limit) {
    bucket.set_rate(limit);
}

void Shaper::enqueue_normal(std::span<const uint8_t> pkt) {
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

void Shaper::process_queue(int tx_fd) {
    while (!normal_queue.empty()) {
        auto pkt_span = normal_queue.front();
        if (!bucket.try_consume(pkt_span.size())) break;

        TxResult res = try_hardware_send(tx_fd, pkt_span);
        result_handlers[static_cast<size_t>(res)](this, pkt_span.size());

        if (res == TxResult::Congested) break;
    }
}

} // namespace Scalpel::Traffic
