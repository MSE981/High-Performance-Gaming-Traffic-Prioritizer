#pragma once

namespace HPGTP::System::Optimizer_util {
    void lock_cpu_frequency();
    void set_current_thread_affinity(int core_id);
    void set_current_thread_affinity_control();
    void set_realtime_priority();
}
