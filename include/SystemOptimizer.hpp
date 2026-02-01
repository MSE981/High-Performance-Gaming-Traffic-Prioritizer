#pragma once
#include <fstream>
#include <print>   // C++23: std::println
#include <format>  // C++23: std::format
#include <string>
#include <pthread.h> // 必须有：pthread_setaffinity_np
#include <sched.h>   // 必须有：cpu_set_t, CPU_SET

namespace Scalpel::System {
    // 锁定 CPU 频率为 Performance 模式
    inline void lock_cpu_frequency() {
        for (int i = 0; i < 4; ++i) {
            std::string path = std::format("/sys/devices/system/cpu/cpu{}/cpufreq/scaling_governor", i);
            std::ofstream f(path);
            if (f.is_open()) f << "performance";
        }
        std::println("[System] CPU governor set to PERFORMANCE.");
    }

    // 设置当前线程的亲和性
    inline void set_thread_affinity(int core_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
            std::println(stderr, "[System] Warning: Failed to bind thread to Core {}", core_id);
        }
    }

    // 设置当前线程为实时调度 (SCHED_FIFO)
    inline void set_realtime_priority() {
        sched_param param{ .sched_priority = 50 }; // 中等实时优先级
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            std::println(stderr, "[System] Warning: Failed to set SCHED_FIFO. Run with sudo/setcap?");
        }
    }
}