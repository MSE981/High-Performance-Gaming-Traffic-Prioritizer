#pragma once
#include <functional>
#include "Telemetry.hpp"
#include "Processor.hpp"

namespace Scalpel::Probe {
    class Manager {
    public:
        // Mode A: internal compute stress test — benchmarks HeuristicProcessor throughput
        static void run_internal_stress(std::function<void(double)> on_complete = nullptr);

        // Mode B: ISP PPS probing via timerfd-gated packet burst
        static void run_isp_probe(int socket_fd);
    };
}
