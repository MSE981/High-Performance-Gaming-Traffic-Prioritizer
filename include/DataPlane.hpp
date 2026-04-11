#pragma once
#include <span>
#include <cstddef>

namespace Scalpel::DataPlane {

// Single egress path for raw Ethernet frames (course “setter” / actuator side).
// All best-effort TX from the packet pipeline should go through here.
struct TxFrameOutput {
    static void send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                  int core_id, size_t prio_idx);
};

} // namespace Scalpel::DataPlane
