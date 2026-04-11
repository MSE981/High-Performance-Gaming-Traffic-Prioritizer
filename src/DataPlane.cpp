#include "DataPlane.hpp"
#include "Telemetry.hpp"
#include <sys/socket.h>

namespace Scalpel::DataPlane {

void TxFrameOutput::send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                     int core_id, size_t prio_idx) {
    if (send(tx_fd, pkt.data(), pkt.size(), MSG_DONTWAIT) < 0)
        Telemetry::instance().core_metrics[core_id].dropped[prio_idx]
            .fetch_add(1, std::memory_order_relaxed);
}

} // namespace Scalpel::DataPlane
