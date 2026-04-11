#include "ProbeManager.hpp"
#include <chrono>
#include <cstring>
#include <print>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace Scalpel::Probe {

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
    tel.is_probing.store(true, std::memory_order_relaxed);
    std::println("[Probe B] Probing ISP limits...");

    uint8_t pkt[64]{};
    std::memset(pkt, 0xEE, 64);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd == -1) return;

    struct itimerspec its{};
    its.it_value.tv_nsec    = 555;
    its.it_interval.tv_nsec = 555;
    timerfd_settime(tfd, 0, &its, nullptr);

    auto start = std::chrono::steady_clock::now();
    uint64_t sent = 0;
    uint64_t expirations = 0;

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        if (read(tfd, &expirations, sizeof(expirations)) > 0) {
            for (uint64_t i = 0; i < expirations; ++i) {
                if (send(socket_fd, pkt, 64, MSG_DONTWAIT) >= 0) ++sent;
            }
        }
    }
    close(tfd);

    double pps     = sent / 5.0;
    double hw_mbps = (sent * 64.0 * 8.0) / (5.0 * 1e6);
    std::println("[Probe B] Local Hardware Tx Limit: {:.2f} Mbps ({} PPS)", hw_mbps, pps);
    tel.is_probing.store(false, std::memory_order_relaxed);
}

} // namespace Scalpel::Probe
