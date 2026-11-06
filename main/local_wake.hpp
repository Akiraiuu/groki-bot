// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>

namespace stackchan::app {
// The coordinator polls the worker and must wait for close()==true before
// granting I2S to another owner. Model mmap/create/destroy run on an internal
// RAM task stack; the existing TLS coordinator keeps its PSRAM stack.
class LocalWake {
public:
    enum class Result { Preparing, Waiting, Detected, Checked, Expired, Failed };
    LocalWake() = default;
    ~LocalWake();
    LocalWake(const LocalWake&) = delete;
    LocalWake& operator=(const LocalWake&) = delete;
    Result poll();
    bool close(); // asynchronous stop; true only after mic/model/worker are gone
private:
    struct Impl;
    Impl* impl_{nullptr};
};
} // namespace stackchan::app

// Capture reset/progress diagnostics only; does not initialize any model/mic.
void local_wake_boot_diagnostics();
