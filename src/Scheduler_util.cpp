#include "Scheduler_util.hpp"
#include "TxFrameOutput_util.hpp"
#include <array>

namespace HPGTP::Engine::Scheduler {

// Attempt to send a packet to the hardware, returning a TxResult indicating success, congestion, or fatal error.
static TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
    using Utils::TxFrame::TxFrameOutput_util;
    switch (TxFrameOutput_util::try_send_packet_nonblocking(fd, pkt)) {
    case TxFrameOutput_util::PacketTxTry::Complete: return TxResult::Success;
    case TxFrameOutput_util::PacketTxTry::Busy: return TxResult::Congested;
    case TxFrameOutput_util::PacketTxTry::Error: return TxResult::Fatal;
    }
    return TxResult::Fatal;
}

// Shaper implementation
void Shaper::set_rate_limit(Utils::Units::Mbps limit) {
    bucket.set_rate(limit);
}
// Set the callback to be invoked after each packet is sent .
void Shaper::set_tx_result_callback(TxResultCallback cb) {
    tx_callback_ = std::move(cb);
}
// Set the callback to be invoked after each packet is sent, with the result and size.
void Shaper::enqueue_normal(std::span<const uint8_t> pkt) {
    lock_spin();
    if (!normal_queue.push(pkt)) {
        auto& tel = Telemetry::instance();
        if (pkt.size() > 2048)
            tel.shaper_oversized_drops.fetch_add(1, std::memory_order_relaxed);
        else
            tel.shaper_queue_overflow_drops.fetch_add(1, std::memory_order_relaxed);
    }
    unlock_spin();
}

// Process the normal queue, sending packets to the hardware if the bucket allows it.
void Shaper::process_queue(int tx_fd) {
    std::array<uint8_t, 2048> pkt_copy{};
    while (true) {
        uint16_t sz = 0;
        {
            lock_spin();
            if (normal_queue.empty()) {
                unlock_spin();
                break;
            }
            auto pkt_span = normal_queue.front();
            if (!bucket.try_consume(pkt_span.size())) {
                unlock_spin();
                break;
            }
            sz = static_cast<uint16_t>(pkt_span.size());
            std::memcpy(pkt_copy.data(), pkt_span.data(), sz);
            unlock_spin();
        }
        TxResult res = try_hardware_send(tx_fd, std::span(pkt_copy.data(), sz));
        lock_spin();
        result_handlers_[static_cast<size_t>(res)](sz);
        unlock_spin();
        if (tx_callback_) tx_callback_(res, sz);
        if (res == TxResult::Congested) break;
    }
}

} // namespace HPGTP::Engine::Scheduler
