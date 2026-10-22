// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "paid_voice_test.hpp"
#include <mutex>

namespace stackchan::conversation {

// Recognition can only OFFER a local dialog. Only a tap on the currently
// rendered dialog may exchange its nonce for a single bounded paid grant.
class WakeConfirmation {
public:
    static constexpr std::int64_t lifetime_ms = 20'000;
    struct Snapshot { std::uint32_t id; std::int64_t remaining_ms; };
    bool offer(std::int64_t now, PaidVoiceTest& paid) {
        std::lock_guard lock{mutex_};
        expire(now);
        const auto p = paid.snapshot(now);
        if (now < 0 || id_ || !p.available || p.phase != PaidVoiceTest::Phase::Idle) return false;
        if (++serial_ == 0) ++serial_;
        id_ = serial_;
        approval_ = p.approval_id;
        deadline_ = now + lifetime_ms;
        return true;
    }
    Snapshot snapshot(std::int64_t now) {
        std::lock_guard lock{mutex_};
        expire(now);
        return {id_, id_ ? deadline_ - now : 0};
    }
    bool confirm(std::int64_t now, std::uint32_t displayed_id, PaidVoiceTest& paid) {
        std::lock_guard lock{mutex_};
        expire(now);
        if (!id_ || displayed_id != id_) return false;
        id_ = 0; // single-use even when the paid gate rejects a stale approval
        return paid.request_start(true, now, approval_);
    }
    void cancel() { std::lock_guard lock{mutex_}; id_ = 0; }
private:
    void expire(std::int64_t now) { if (id_ && (now < 0 || now >= deadline_)) id_ = 0; }
    std::mutex mutex_;
    std::uint32_t serial_{0}, id_{0}, approval_{0};
    std::int64_t deadline_{0};
};
inline WakeConfirmation wake_confirmation;
} // namespace stackchan::conversation
