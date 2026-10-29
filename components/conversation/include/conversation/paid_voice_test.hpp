// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
#include <mutex>

namespace stackchan::conversation {

// RAM-only, one-reply permission. Neither credentials nor reboot grant it.
class PaidVoiceTest {
public:
    static constexpr std::int64_t limit_ms = 60'000;
    static constexpr std::int64_t local_limit_ms = 15'000;
    enum class Mode { Paid, Local };
    enum class Phase { Idle, Queued, Running, Stopping };
    enum class Reason { None, Completed, Cancelled, Timeout, Error, Interrupted };
    struct Snapshot {
        Phase phase;
        Reason reason;
        bool available;
        std::int64_t remaining_ms;
        std::uint32_t approval_id;
        Mode mode;
        bool input_ready; // Current paid session: transport + warmed microphone input.
    };
    void set_available(bool value, std::uint32_t seed = 1) {
        std::lock_guard lock{mutex_};
        available_ = value;
        input_ready_ = false;
        if (value) approval_id_ = seed ? seed : 1;
        if (!value && phase_ != Phase::Idle) stop_locked(Reason::Error);
    }
    bool request_start(bool confirmed, std::int64_t now, std::uint32_t approval_id, Mode mode = Mode::Paid) {
        std::lock_guard lock{mutex_};
        if (!confirmed || !available_ || phase_ != Phase::Idle || now < 0 || approval_id != approval_id_) return false;
        next_approval_locked();
        phase_ = Phase::Queued;
        reason_ = Reason::None;
        mode_ = mode;
        input_ready_ = false;
        deadline_ = now + (mode == Mode::Paid ? limit_ms : local_limit_ms);
        return true;
    }
    bool claim(std::int64_t now, Mode mode = Mode::Paid) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (phase_ != Phase::Queued || mode_ != mode) return false;
        phase_ = Phase::Running;
        return true;
    }
    bool permitted(std::int64_t now, std::uint32_t expected_approval = 0) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return mode_ == Mode::Paid && phase_ == Phase::Running && (expected_approval == 0 || expected_approval == approval_id_);
    }
    bool local_permitted(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return mode_ == Mode::Local && phase_ == Phase::Running;
    }
    // A delayed producer from an older session cannot publish or clear readiness.
    // This is observation only; it never grants permission or extends a deadline.
    bool update_input_ready(std::int64_t now, std::uint32_t expected_approval, bool ready) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (expected_approval == 0 || expected_approval != approval_id_ ||
            mode_ != Mode::Paid || phase_ != Phase::Running || !available_ || now < 0) return false;
        input_ready_ = ready;
        return true;
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
        input_ready_ = false;
        deadline_ = 0;
    }
    Snapshot snapshot(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return {phase_, reason_, available_, deadline_ > now ? deadline_ - now : 0, approval_id_, mode_,
                input_ready_ && available_ && phase_ == Phase::Running && mode_ == Mode::Paid};
    }
private:
    void next_approval_locked() { if (++approval_id_ == 0) ++approval_id_; }
    void stop_locked(Reason why) {
        input_ready_ = false;
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
    Mode mode_{Mode::Paid};
    bool available_{false};
    bool input_ready_{false};
    std::int64_t deadline_{0};
    std::uint32_t approval_id_{1};
};

inline PaidVoiceTest paid_voice_test;

} // namespace stackchan::conversation
