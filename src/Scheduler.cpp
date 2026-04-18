#include "Scheduler.hpp"
#include "DataPlane.hpp"
#include <array>

namespace HPGTP::Traffic {

static TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
    using DataPlane::TxFrameOutput;
    switch (TxFrameOutput::try_send_packet_nonblocking(fd, pkt)) {
    case TxFrameOutput::PacketTxTry::Complete: return TxResult::Success;
    case TxFrameOutput::PacketTxTry::Busy: return TxResult::Congested;
    case TxFrameOutput::PacketTxTry::Error: return TxResult::Fatal;
    }
    return TxResult::Fatal;
}

void Shaper::set_rate_limit(Mbps limit) {
    bucket.set_rate(limit);
}

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
        result_handlers[static_cast<size_t>(res)](this, sz);
        unlock_spin();
        if (res == TxResult::Congested) break;
    }
}

} // namespace HPGTP::Traffic
