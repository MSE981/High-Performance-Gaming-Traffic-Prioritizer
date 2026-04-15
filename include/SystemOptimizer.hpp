#pragma once

namespace HPGTP::System::Optimizer {
    void lock_cpu_frequency();
    void set_current_thread_affinity(int core_id);
    void set_realtime_priority();
}
