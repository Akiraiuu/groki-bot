// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
#include <mutex>

namespace stackchan::conversation {

// RAM-only, one-reply permission. Neither credentials nor reboot grant it.
class PaidVoiceTest {
public:
    static constexpr std::int64_t limit_ms = 60'000;
    enum class Phase { Idle, Queued, Running, Stopping };
    enum class Reason { None, Completed, Cancelled, Timeout, Error, Interrupted };
    struct Snapshot {
        Phase phase;
        Reason reason;
        bool available;
        std::int64_t remaining_ms;
        std::uint32_t approval_id;
    };
    void set_available(bool value, std::uint32_t seed = 1) {
        std::lock_guard lock{mutex_};
        available_ = value;
        if (value) approval_id_ = seed ? seed : 1;
        if (!value && phase_ != Phase::Idle) stop_locked(Reason::Error);
    }
    bool request_start(bool confirmed, std::int64_t now, std::uint32_t approval_id) {
        std::lock_guard lock{mutex_};
        if (!confirmed || !available_ || phase_ != Phase::Idle || now < 0 || approval_id != approval_id_) return false;
        next_approval_locked();
        phase_ = Phase::Queued;
        reason_ = Reason::None;
        deadline_ = now + limit_ms;
        return true;
    }
    bool claim(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (phase_ != Phase::Queued) return false;
        phase_ = Phase::Running;
        return true;
    }
    bool permitted(std::int64_t now, std::uint32_t expected_approval = 0) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return phase_ == Phase::Running && (expected_approval == 0 || expected_approval == approval_id_);
    }
    void request_stop(Reason why = Reason::Cancelled) {
        std::lock_guard lock{mutex_};
        // Even an idle stop invalidates a delayed/in-flight old start request.
        next_approval_locked();
        stop_locked(why);
    }
    void finish() {
        std::lock_guard lock{mutex_};
        // Retain the last reason for the local status page, not permission.
        phase_ = Phase::Idle;
        deadline_ = 0;
    }
    Snapshot snapshot(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return {phase_, reason_, available_, deadline_ > now ? deadline_ - now : 0, approval_id_};
    }
private:
    void next_approval_locked() { if (++approval_id_ == 0) ++approval_id_; }
    void stop_locked(Reason why) {
        if (phase_ == Phase::Queued || phase_ == Phase::Running) {
            phase_ = Phase::Stopping;
            reason_ = why;
        }
    }
    void expire_locked(std::int64_t now) {
        if (deadline_ != 0 && now >= deadline_) stop_locked(Reason::Timeout);
    }
    std::mutex mutex_;
    Phase phase_{Phase::Idle};
    Reason reason_{Reason::None};
    bool available_{false};
    std::int64_t deadline_{0};
    std::uint32_t approval_id_{1};
};

inline PaidVoiceTest paid_voice_test;

} // namespace stackchan::conversation
