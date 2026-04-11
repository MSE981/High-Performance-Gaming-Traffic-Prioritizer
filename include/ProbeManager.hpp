#pragma once
#include <functional>
#include <atomic>
#include "Telemetry.hpp"
#include "Processor.hpp"

namespace Scalpel::Probe {
    class Manager {
    public:
        // Mode A: internal compute stress test — benchmarks HeuristicProcessor throughput.
        // cancel_requested: when non-null and set true, the benchmark loop exits early (joinable thread shutdown).
        static void run_internal_stress(std::function<void(double)> on_complete = nullptr,
                                        std::atomic<bool>* cancel_requested = nullptr);

        // Mode B: ISP PPS probing via timerfd-gated packet burst
        static void run_isp_probe(int socket_fd);
    };
}
