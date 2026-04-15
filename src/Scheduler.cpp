#include "Scheduler.hpp"
#include "DataPlane.hpp"

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
    if (!normal_queue.push(pkt)) {
        auto& tel = Telemetry::instance();
        if (pkt.size() > 2048)
            tel.shaper_oversized_drops.fetch_add(1, std::memory_order_relaxed);
        else
            tel.shaper_queue_overflow_drops.fetch_add(1, std::memory_order_relaxed);
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

} // namespace HPGTP::Traffic
