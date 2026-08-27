#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace HPGTP::Lifecycle {

class Lifecycle {
public:
    bool begin_stop() {
        return !started_.exchange(true, std::memory_order_acq_rel);
    }

    void mark_complete() {
        {
            std::lock_guard lock(mutex_);
            complete_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this]() {
            return complete_.load(std::memory_order_acquire);
        });
    }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> complete_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace HPGTP::Lifecycle
