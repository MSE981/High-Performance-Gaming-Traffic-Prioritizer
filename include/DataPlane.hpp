#pragma once
#include <span>
#include <cstddef>

namespace Scalpel::DataPlane {

// Single egress path for raw Ethernet frames from the data plane.
struct TxFrameOutput {
    [[nodiscard]] static bool send_blocking(int tx_fd, std::span<const uint8_t> pkt) noexcept;

    static void send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                  int core_id, size_t prio_idx);

    static void send_stream_blocking(int fd, std::span<const uint8_t> data);
};

} // namespace Scalpel::DataPlane
