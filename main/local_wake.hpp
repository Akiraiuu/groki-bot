// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>

namespace stackchan::app {
// Called ONLY by the Saved-Agent coordinator, which also owns cloud and
// diagnostic audio. There is no second task competing for the I2S peripheral.
class LocalWake {
public:
    enum class Result { Preparing, Waiting, Detected, Failed };
    LocalWake() = default;
    ~LocalWake();
    LocalWake(const LocalWake&) = delete;
    LocalWake& operator=(const LocalWake&) = delete;
    Result poll();
    void close(); // stop mic, erase PCM and free decoder before any cloud call
private:
    struct Impl;
    Impl* impl_{nullptr};
};
} // namespace stackchan::app
