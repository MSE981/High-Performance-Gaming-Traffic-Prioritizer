#include "DataPlane.hpp"
#include "Telemetry.hpp"
#include <sys/socket.h>
#include <cerrno>
#include <cstdint>

namespace HPGTP::DataPlane {

TxFrameOutput::PacketTxTry TxFrameOutput::try_send_packet_nonblocking(
    int tx_fd, std::span<const uint8_t> pkt) noexcept {
    for (;;) {
        ssize_t n = ::send(tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT);
        if (n >= 0) {
            return static_cast<size_t>(n) == pkt.size() ? PacketTxTry::Complete
                                                        : PacketTxTry::Error;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) return PacketTxTry::Busy;
        return PacketTxTry::Error;
    }
}

void TxFrameOutput::send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                     int core_id, size_t prio_idx) {
    if (try_send_packet_nonblocking(tx_fd, pkt) != PacketTxTry::Complete)
        Telemetry::instance().core_metrics[core_id].dropped[prio_idx]
            .fetch_add(1, std::memory_order_relaxed);
}

void TxFrameOutput::send_stream_blocking(int fd, std::span<const uint8_t> data) {
    const uint8_t* p   = data.data();
    size_t         left = data.size();
    while (left > 0) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += static_cast<size_t>(n);
        left -= static_cast<size_t>(n);
    }
}

} // namespace HPGTP::DataPlane
