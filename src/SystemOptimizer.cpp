#include "SystemOptimizer.hpp"
#include <print>
#include <cstdio>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>

namespace HPGTP::System::Optimizer {

void lock_cpu_frequency() {
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    for (long i = 0; i < n; ++i) {
        char path[80];
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",
            static_cast<int>(i));
        int fd = ::open(path, O_WRONLY);
        if (fd >= 0) {
            if (::write(fd, "performance", 11) < 0)
                std::println(stderr, "[System] Warning: failed to set governor for cpu{}", static_cast<int>(i));
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

} // namespace HPGTP::System::Optimizer
