// Host-only ESP task critical-section double; never included by firmware.
#pragma once
#include <atomic>
#include <cstdlib>
#include <mutex>
struct portMUX_TYPE { std::mutex mutex; };
#define portMUX_INITIALIZER_UNLOCKED {}
namespace policy_esp_stub {
inline thread_local unsigned depth = 0;
inline std::atomic<unsigned> entries{0}, exits{0};
inline void enter(portMUX_TYPE* mux) {
    if (!mux || depth != 0) std::abort(); // Forbid recursive policy locking.
    mux->mutex.lock();
    depth = 1;
    entries.fetch_add(1, std::memory_order_relaxed);
}
inline void leave(portMUX_TYPE* mux) {
    if (!mux || depth != 1) std::abort();
    depth = 0;
    exits.fetch_add(1, std::memory_order_relaxed);
    mux->mutex.unlock();
}
inline bool outside() { return depth == 0; }
}
