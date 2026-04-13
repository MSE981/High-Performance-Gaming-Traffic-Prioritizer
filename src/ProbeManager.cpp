#include "ProbeManager.hpp"
#include <chrono>
#include <cerrno>
#include <cstring>
#include <print>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Scalpel::Probe {

namespace {

struct ProbingFlag {
    std::atomic<bool>* flag;
    explicit ProbingFlag(std::atomic<bool>* f) : flag(f) {
        flag->store(true, std::memory_order_relaxed);
    }
    ~ProbingFlag() { flag->store(false, std::memory_order_relaxed); }
};

} // namespace

// Benchmark-only: tight loop to estimate heuristic throughput; not a production timer path.
void Manager::run_internal_stress(std::function<void(double)> on_complete,
                                    std::atomic<bool>* cancel_requested) {
    auto& tel = Telemetry::instance();
    tel.is_probing.store(true, std::memory_order_relaxed);
    std::println("[Probe A] Benchmarking internal logic...");

    Logic::HeuristicProcessor temp_proc;
    uint8_t dummy_data[64]{};
    dummy_data[12] = 0x08; dummy_data[13] = 0x00; // Eth proto
    dummy_data[14] = 0x45;                          // IP Ver/IHL
    auto start = std::chrono::steady_clock::now();
    uint64_t count = 0;

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        if (cancel_requested &&
            cancel_requested->load(std::memory_order_relaxed))
            break;
        auto pkt_ctx = Net::ParsedPacket::parse(std::span<uint8_t>{dummy_data});
        temp_proc.process(pkt_ctx);
        count++;
    }

    double pps  = count / 5.0;
    double mbps = (pps * 64 * 8) / 1e6;
    std::println("[Probe A] CPU capacity: {:.2f} Mbps ({} PPS)", mbps, pps);
    tel.is_probing.store(false, std::memory_order_relaxed);
    if (on_complete) on_complete(mbps);
}

void Manager::run_isp_probe(int socket_fd) {
    auto& tel = Telemetry::instance();

    uint8_t pkt[64]{};
    std::memset(pkt, 0xEE, 64);

    uint64_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    {
        ProbingFlag guard(&tel.is_probing);
        std::println("[Probe B] Probing ISP limits (socket POLLOUT, coarse 5s estimate)...");

        while (std::chrono::steady_clock::now() < deadline) {
            const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remain <= 0) break;

            int timeout_ms = static_cast<int>(std::min<int64_t>(remain, 500));
            struct pollfd pfd{};
            pfd.fd     = socket_fd;
            pfd.events = POLLOUT;
            int pr     = ::poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;

            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;

            while (std::chrono::steady_clock::now() < deadline) {
                ssize_t n = ::send(socket_fd, pkt, sizeof(pkt), MSG_DONTWAIT);
                if (n >= 0) {
                    ++sent;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                break;
            }
        }
    }

    double pps     = sent / 5.0;
    double hw_mbps = (sent * 64.0 * 8.0) / (5.0 * 1e6);
    std::println("[Probe B] Local Hardware Tx Limit: {:.2f} Mbps ({} PPS)", hw_mbps, pps);
}

} // namespace Scalpel::Probe
