#pragma once
#include <span>
#include <cstddef>
#include <cstdint>

namespace HPGTP::TxFrame_util {

// Single egress path for raw Ethernet frames
struct TxFrameOutput_util {
    enum class PacketTxTry : std::uint8_t { Complete = 0, Busy = 1, Error = 2 };

    [[nodiscard]] static PacketTxTry try_send_packet_nonblocking(int tx_fd,std::span<const uint8_t> pkt) noexcept;

    // Non-blocking send with best-effort
    static void send_best_effort(int tx_fd, std::span<const uint8_t> pkt,int core_id, size_t prio_idx);

    static void send_stream_blocking(int fd, std::span<const uint8_t> data);
};

} // namespace HPGTP::TxFrame_util
