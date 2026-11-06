// SPDX-License-Identifier: BSL-1.0
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <mutex>
#endif

namespace stopwatch::candidate {
#if defined(ESP_PLATFORM)
namespace detail {
// Internal static storage: the Coordinator itself may be allocated in PSRAM.
// Shared only by the small event-policy state; never held during queue/heap I/O.
inline DRAM_ATTR portMUX_TYPE event_policy_mux = portMUX_INITIALIZER_UNLOCKED;
}
#endif

// Task-context state synchronization, NOT a lock-free or timed-close guarantee.
// ESP-IDF critical sections may spin while the other core holds the mux.
class EventPolicyLock {
public:
    EventPolicyLock() = default;
    EventPolicyLock(const EventPolicyLock&) = delete;
    EventPolicyLock& operator=(const EventPolicyLock&) = delete;
    void lock() noexcept {
#if defined(ESP_PLATFORM)
        taskENTER_CRITICAL(&detail::event_policy_mux);
#else
        mutex_.lock();
#endif
    }
    void unlock() noexcept {
#if defined(ESP_PLATFORM)
        taskEXIT_CRITICAL(&detail::event_policy_mux);
#else
        mutex_.unlock();
#endif
    }
private:
#if !defined(ESP_PLATFORM)
    std::mutex mutex_;
#endif
};

class EventPolicyGuard {
public:
    explicit EventPolicyGuard(EventPolicyLock& lock) noexcept : lock_(lock) { lock_.lock(); }
    ~EventPolicyGuard() { lock_.unlock(); }
    EventPolicyGuard(const EventPolicyGuard&) = delete;
    EventPolicyGuard& operator=(const EventPolicyGuard&) = delete;
private:
    EventPolicyLock& lock_;
};
} // namespace stopwatch::candidate
