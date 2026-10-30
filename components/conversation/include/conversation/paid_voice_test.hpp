// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
#include <mutex>

namespace stackchan::conversation {

// RAM-only, explicitly scoped permission; reboot never restores a session.
class PaidVoiceTest {
public:
    static constexpr std::int64_t limit_ms = 60'000;
    static constexpr std::int64_t local_limit_ms = 15'000;
    static constexpr std::int64_t dialogue_idle_ms = 10'000;
    static constexpr std::uint32_t dialogue_max_rounds = 3;
    enum class Mode { Paid, Local };
    enum class Profile { SingleReply, Dialogue };
    enum class Phase { Idle, Queued, Running, Stopping };
    enum class Reason { None, Completed, Cancelled, Timeout, Error, Interrupted, IdleTimeout, Farewell, RoundLimit };
    enum class Fault { None, Unspecified, Unavailable, UnexpectedReply, LocalCapture, LocalPlayback,
                       WakeRelease, Connect, Recovery, AudioUpload, Microphone, InputTurnReset, ConversationEvent };
    static const char* fault_name(Fault fault) {
        switch (fault) {
        case Fault::None: return "none";
        case Fault::Unspecified: return "unspecified";
        case Fault::Unavailable: return "unavailable";
        case Fault::UnexpectedReply: return "unexpected_reply";
        case Fault::LocalCapture: return "local_capture";
        case Fault::LocalPlayback: return "local_playback";
        case Fault::WakeRelease: return "wake_release";
        case Fault::Connect: return "connect";
        case Fault::Recovery: return "recovery";
        case Fault::AudioUpload: return "audio_upload";
        case Fault::Microphone: return "microphone";
        case Fault::InputTurnReset: return "input_turn_reset";
        case Fault::ConversationEvent: return "conversation_event";
        }
        return "unspecified";
    }
    struct Snapshot {
        Phase phase;
        Reason reason;
        bool available;
        std::int64_t remaining_ms;
        std::uint32_t approval_id;
        Mode mode;
        bool input_ready;
        std::uint32_t max_rounds, completed_rounds;
        std::int64_t idle_timeout_ms, idle_remaining_ms;
        Fault fault;
        int fault_detail; // ConversationError enum 0..7 only, -1 when unavailable.
    };
    void set_available(bool value, std::uint32_t seed = 1) {
        std::lock_guard lock{mutex_};
        available_ = value;
        input_ready_ = false;
        if (value) approval_id_ = seed ? seed : 1;
        if (!value && phase_ != Phase::Idle) stop_locked(Reason::Error, Fault::Unavailable);
    }
    bool request_start(bool confirmed, std::int64_t now, std::uint32_t approval_id,
                       Mode mode = Mode::Paid, Profile profile = Profile::SingleReply) {
        std::lock_guard lock{mutex_};
        if (!confirmed || !available_ || phase_ != Phase::Idle || now < 0 || approval_id != approval_id_ ||
            (profile != Profile::SingleReply && profile != Profile::Dialogue) ||
            (mode != Mode::Paid && mode != Mode::Local) ||
            (mode == Mode::Local && profile != Profile::SingleReply)) return false;
        next_approval_locked();
        phase_ = Phase::Queued;
        reason_ = Reason::None;
        mode_ = mode;
        profile_ = profile;
        completed_rounds_ = 0;
        fault_ = Fault::None;
        fault_detail_ = -1;
        input_ready_ = reply_pending_ = false;
        waiting_since_ = -1;
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
        return mode_ == Mode::Paid && phase_ == Phase::Running &&
               (expected_approval == 0 || expected_approval == approval_id_);
    }
    bool local_permitted(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        return mode_ == Mode::Local && phase_ == Phase::Running;
    }
    bool update_input_ready(std::int64_t now, std::uint32_t expected_approval, bool ready) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (!current_paid_locked(now, expected_approval)) return false;
        input_ready_ = ready;
        // Repeated readiness updates cannot keep extending silence.
        if (ready && profile_ == Profile::Dialogue && !reply_pending_ && waiting_since_ < 0)
            waiting_since_ = now;
        return true;
    }
    bool note_user_activity(std::int64_t now, std::uint32_t expected_approval) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (!current_paid_locked(now, expected_approval)) return false;
        reply_pending_ = true;
        waiting_since_ = -1; // Speaking/transcription/reply is not idle waiting.
        return true;
    }
    // Called once after the response completed AND physical playback drained.
    // true permits only the next input turn in this same, unextended grant.
    bool reply_completed(std::int64_t now, std::uint32_t expected_approval) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        if (!current_paid_locked(now, expected_approval)) return false;
        if (profile_ == Profile::Dialogue && !reply_pending_) {
            stop_locked(Reason::Error, Fault::UnexpectedReply); // Do not silently continue an unsolicited/duplicate reply.
            return false;
        }
        ++completed_rounds_;
        input_ready_ = reply_pending_ = false;
        waiting_since_ = -1;
        if (completed_rounds_ >= max_rounds_locked()) {
            stop_locked(profile_ == Profile::Dialogue ? Reason::RoundLimit : Reason::Completed);
            return false;
        }
        return true;
    }
    void request_stop(Reason why = Reason::Cancelled, Fault fault = Fault::Unspecified, int detail = -1) {
        std::lock_guard lock{mutex_};
        next_approval_locked(); // Also invalidates delayed starts while idle.
        stop_locked(why, fault, detail);
    }
    void finish() {
        std::lock_guard lock{mutex_};
        phase_ = Phase::Idle;
        input_ready_ = reply_pending_ = false;
        waiting_since_ = -1;
        deadline_ = 0;
    }
    Snapshot snapshot(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire_locked(now);
        const bool running = available_ && phase_ == Phase::Running && mode_ == Mode::Paid;
        const auto idle_left = running && waiting_since_ >= 0
            ? dialogue_idle_ms - (now - waiting_since_) : 0;
        return {phase_, reason_, available_, deadline_ > now ? deadline_ - now : 0, approval_id_, mode_,
                input_ready_ && running, max_rounds_locked(), completed_rounds_,
                profile_ == Profile::Dialogue ? dialogue_idle_ms : 0, idle_left > 0 ? idle_left : 0, fault_, fault_detail_};
    }
private:
    bool current_paid_locked(std::int64_t now, std::uint32_t expected) const {
        return expected != 0 && expected == approval_id_ && now >= 0 &&
               mode_ == Mode::Paid && phase_ == Phase::Running && available_;
    }
    std::uint32_t max_rounds_locked() const { return profile_ == Profile::Dialogue ? dialogue_max_rounds : 1; }
    void next_approval_locked() { if (++approval_id_ == 0) ++approval_id_; }
    void stop_locked(Reason why, Fault fault = Fault::Unspecified, int detail = -1) {
        input_ready_ = reply_pending_ = false;
        waiting_since_ = -1;
        if (phase_ == Phase::Queued || phase_ == Phase::Running) {
            if (why == Reason::Error) {
                fault_ = fault == Fault::None ? Fault::Unspecified : fault;
                fault_detail_ = fault == Fault::ConversationEvent && detail >= 0 && detail <= 7 ? detail : -1;
            }
            phase_ = Phase::Stopping;
            reason_ = why;
        }
    }
    void expire_locked(std::int64_t now) {
        if (deadline_ != 0 && now >= deadline_) stop_locked(Reason::Timeout);
        else if (phase_ == Phase::Running && profile_ == Profile::Dialogue &&
                 waiting_since_ >= 0 && now - waiting_since_ >= dialogue_idle_ms)
            stop_locked(Reason::IdleTimeout);
    }
    std::mutex mutex_;
    Phase phase_{Phase::Idle};
    Reason reason_{Reason::None};
    Fault fault_{Fault::None};
    int fault_detail_{-1};
    Mode mode_{Mode::Paid};
    Profile profile_{Profile::SingleReply};
    bool available_{false}, input_ready_{false}, reply_pending_{false};
    std::uint32_t completed_rounds_{0};
    std::int64_t deadline_{0}, waiting_since_{-1};
    std::uint32_t approval_id_{1};
};
inline PaidVoiceTest paid_voice_test;
} // namespace stackchan::conversation
