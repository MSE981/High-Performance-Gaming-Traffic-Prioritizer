#include "SystemOptimizer.hpp"
#include <print>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>

namespace Scalpel::System::Optimizer {

void lock_cpu_frequency() {
    for (int i = 0; i < 4; ++i) {
        char path[80];
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        int fd = ::open(path, O_WRONLY);
        if (fd >= 0) {
            if (::write(fd, "performance", 11) < 0)
                std::println(stderr, "[System] Warning: failed to set governor for cpu{}", i);
            ::close(fd);
        }
    }
    std::println("[System] CPU governor set to PERFORMANCE.");
}

void set_current_thread_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        std::println(stderr, "[System] Warning: Failed to bind thread to core {}", core_id);
}

void set_realtime_priority() {
    sched_param param{ .sched_priority = 50 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
        std::println(stderr, "[System] Warning: Failed to set SCHED_FIFO. Run with sudo/setcap?");
}

} // namespace Scalpel::System::Optimizer
