#pragma once
#include <span>
#include <cstddef>
#include <cstdint>

namespace Scalpel::DataPlane {

// Single egress path for raw Ethernet frames from the data plane.
struct TxFrameOutput {
    enum class PacketTxTry : std::uint8_t { Complete = 0, Busy = 1, Error = 2 };

    // One non-blocking send(2) attempt (MSG_DONTWAIT). Busy = EAGAIN / ENOBUFS / EWOULDBLOCK.
    [[nodiscard]] static PacketTxTry try_send_packet_nonblocking(int tx_fd,
                                                                  std::span<const uint8_t> pkt) noexcept;

    // Non-blocking send; increments drop telemetry on any failure (including would-block).
    static void send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                  int core_id, size_t prio_idx);

    static void send_stream_blocking(int fd, std::span<const uint8_t> data);
};

} // namespace Scalpel::DataPlane
