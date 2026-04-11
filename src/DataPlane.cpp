#include "DataPlane.hpp"
#include "Telemetry.hpp"
#include <sys/socket.h>
#include <cerrno>

namespace Scalpel::DataPlane {

bool TxFrameOutput::send_blocking(int tx_fd, std::span<const uint8_t> pkt) noexcept {
    const uint8_t* p   = pkt.data();
    size_t         left = pkt.size();
    while (left > 0) {
        ssize_t n = ::send(tx_fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += static_cast<size_t>(n);
        left -= static_cast<size_t>(n);
    }
    return true;
}

void TxFrameOutput::send_best_effort(int tx_fd, std::span<const uint8_t> pkt,
                                     int core_id, size_t prio_idx) {
    if (!send_blocking(tx_fd, pkt))
        Telemetry::instance().core_metrics[core_id].dropped[prio_idx]
            .fetch_add(1, std::memory_order_relaxed);
}

} // namespace Scalpel::DataPlane
