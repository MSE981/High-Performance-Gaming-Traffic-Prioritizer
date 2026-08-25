#pragma once

namespace HPGTP::System::Optimizer {
    void lock_cpu_frequency();
    void set_current_thread_affinity(int core_id);
    // Bind to the control cores (0 and 1) as a set; the scheduler may use either.
    void set_current_thread_affinity_control();
    void set_realtime_priority();
}
