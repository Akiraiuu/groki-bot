// SPDX-License-Identifier: BSL-1.0
// Portable policy candidate. No network, device, credentials, or implicit grants.
#pragma once
#include "event_policy_lock.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace stopwatch::candidate {

enum class EventClass : std::uint8_t { Payload, Control };
enum class DeliveryFault : std::uint8_t {
    None, TotalCapacity, PayloadCapacity, QueueRejected, AllocationFailed,
    SourceError, InvariantViolation
};
struct FaultSnapshot {
    DeliveryFault kind{DeliveryFault::None};
    std::uint8_t event_type{0};
    int source_error{-1};
};

// RAII admission: a queue envelope owns a Permit, and deletes it exactly once.
// The policy must outlive all permits. begin() requires the prior producer to be
// quiescent, accepting=false, and zero outstanding permits; it grants no API use.
template<std::uint32_t Capacity, std::uint32_t ControlReserve>
class EventDeliveryPolicy {
    static_assert(ControlReserve > 0 && ControlReserve < Capacity);
    static constexpr auto payload_limit = Capacity - ControlReserve;
public:
    EventDeliveryPolicy() = default;
    EventDeliveryPolicy(const EventDeliveryPolicy&) = delete;
    EventDeliveryPolicy& operator=(const EventDeliveryPolicy&) = delete;
    class Permit {
        friend class EventDeliveryPolicy;
        EventDeliveryPolicy* owner_{};
        std::uint32_t generation_{};
        bool payload_{};
        Permit(EventDeliveryPolicy& owner, std::uint32_t generation, bool payload)
            : owner_(&owner), generation_(generation), payload_(payload) {}
    public:
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              generation_(other.generation_), payload_(other.payload_) {}
        Permit& operator=(Permit&& other) noexcept {
            if (this != &other) {
                reset(); owner_ = std::exchange(other.owner_, nullptr);
                generation_ = other.generation_; payload_ = other.payload_;
            }
            return *this;
        }
        ~Permit() { reset(); }
        std::uint32_t generation() const { return generation_; }
        bool is_payload() const { return payload_; }
        void reset() noexcept {
            if (auto* owner = std::exchange(owner_, nullptr)) owner->release(payload_);
        }
    };

    bool begin(std::uint32_t generation) noexcept {
        const EventPolicyGuard guard{lock_};
        if (!generation || generation == generation_ || accepting_ || pending_ != 0) return false;
        // Caller has joined/quiesced the old producer, not merely timed it out.
        generation_ = generation;
        first_fault_ = rejected_ = stale_ = 0;
        accepting_ = true;
        return true;
    }

    void revoke(std::uint32_t generation) noexcept {
        const EventPolicyGuard guard{lock_};
        if (generation_ == generation) accepting_ = false;
    }

    bool current(std::uint32_t generation) const noexcept {
        const EventPolicyGuard guard{lock_};
        return current_locked(generation);
    }

    std::optional<Permit> try_admit(EventClass kind, std::uint32_t generation,
                                    std::uint8_t event_type) noexcept {
        const bool payload = kind == EventClass::Payload;
        {
            const EventPolicyGuard guard{lock_};
            if (!current_locked(generation)) { increment(stale_); return std::nullopt; }
            if (pending_ == Capacity) {
                reject_locked(generation, DeliveryFault::TotalCapacity, event_type);
                return std::nullopt;
            }
            if (payload && payload_pending_ == payload_limit) {
                reject_locked(generation, DeliveryFault::PayloadCapacity, event_type);
                return std::nullopt;
            }
            // Generation, both counters and first cause share one linearization.
            ++pending_;
            if (payload) ++payload_pending_;
        }
        // No Permit destruction, allocation, queue call or client work under lock.
        Permit result{*this, generation, payload};
        return std::optional<Permit>{std::move(result)};
    }

    // A source Error is latched OUTSIDE the lossy queue; only finite enums survive.
    // Consumer maps this to an explicit failed stop. It must not report completion.
    void note_source_error(std::uint32_t generation, std::uint8_t event_type,
                           int detail) noexcept {
        const EventPolicyGuard guard{lock_};
        note_fault_locked(generation, DeliveryFault::SourceError, event_type, detail);
    }
    void note_queue_rejection(std::uint32_t generation, std::uint8_t event_type) noexcept {
        const EventPolicyGuard guard{lock_};
        reject_locked(generation, DeliveryFault::QueueRejected, event_type);
    }
    void note_allocation_failure(std::uint32_t generation, std::uint8_t event_type) noexcept {
        const EventPolicyGuard guard{lock_};
        reject_locked(generation, DeliveryFault::AllocationFailed, event_type);
    }
    FaultSnapshot first_fault() const noexcept {
        const EventPolicyGuard guard{lock_};
        const auto packed = first_fault_;
        return {static_cast<DeliveryFault>(packed & 0xffu),
                static_cast<std::uint8_t>((packed >> 8) & 0xffu),
                static_cast<int>((packed >> 16) & 0xffu) - 1};
    }
    std::uint32_t pending() const noexcept { const EventPolicyGuard guard{lock_}; return pending_; }
    std::uint32_t payload_pending() const noexcept { const EventPolicyGuard guard{lock_}; return payload_pending_; }
    std::uint32_t rejected() const noexcept { const EventPolicyGuard guard{lock_}; return rejected_; }
    std::uint32_t stale() const noexcept { const EventPolicyGuard guard{lock_}; return stale_; }

private:
    bool current_locked(std::uint32_t generation) const noexcept {
        return generation && accepting_ && generation_ == generation;
    }
    static void increment(std::uint32_t& count) noexcept {
        if (count != UINT32_MAX) ++count;
    }
    void release(bool payload) noexcept {
        const EventPolicyGuard guard{lock_};
        if (payload) --payload_pending_;
        --pending_;
    }
    void reject_locked(std::uint32_t generation, DeliveryFault fault, std::uint8_t type) noexcept {
        if (!current_locked(generation)) return;
        increment(rejected_); note_fault_locked(generation, fault, type, -1);
    }
    void note_fault_locked(std::uint32_t generation, DeliveryFault fault,
                    std::uint8_t type, int detail) noexcept {
        if (!current_locked(generation) || fault == DeliveryFault::None || first_fault_ != 0) return;
        const auto code = static_cast<std::uint32_t>(detail >= 0 && detail <= 7 ? detail + 1 : 0);
        const auto packed = static_cast<std::uint32_t>(fault) |
                            (static_cast<std::uint32_t>(type) << 8) | (code << 16);
        first_fault_ = packed;
    }
    mutable EventPolicyLock lock_;
    std::uint32_t generation_{0}, pending_{0}, payload_pending_{0};
    std::uint32_t first_fault_{0}, rejected_{0}, stale_{0};
    bool accepting_{false};
};

// Budget checks BETWEEN handlers. It cannot preempt a blocking single handler.
class EventBatchBudget {
public:
    EventBatchBudget(std::uint32_t max_events, std::uint64_t max_us, std::uint64_t start_us)
        : max_events_(max_events), max_us_(max_us), start_us_(start_us) {}
    bool may_process_next(std::uint64_t now_us) const noexcept {
        return processed_ < max_events_ && now_us >= start_us_ && now_us - start_us_ < max_us_;
    }
    void processed_one() noexcept { if (processed_ < max_events_) ++processed_; }
    std::uint32_t count() const noexcept { return processed_; }
private:
    std::uint32_t max_events_, processed_{0};
    std::uint64_t max_us_, start_us_;
};

enum class CloseStage : std::uint8_t {
    Idle, AwaitPermissionRevoke, AwaitAudioStop, AwaitSender, AwaitTransport,
    AwaitCallbacks, AwaitWorker, AwaitQueueDrain, ClosedOnTime, DeadlineExceeded, ClosedLate
};
struct CloseFacts {
    bool permission_revoked{false}, audio_stopped{false}, sender_finished{false};
    bool transport_closed{false}, callbacks_quiesced{false}, close_worker_finished{false};
    bool event_queue_drained{false};
};

// Single-consumer ownership policy, NOT a WebSocket closer or a hardware timer.
// Facts must come from real acknowledgements. A timeout is never an ack.
class CloseOwnershipPolicy {
public:
    bool begin_close(std::uint32_t generation, std::uint64_t now_us,
                      std::uint64_t deadline_us) noexcept {
        if (!generation || generation == generation_ || deadline_us <= now_us || (owned_ && !destroy_owner_allowed())) return false;
        generation_ = generation; deadline_us_ = deadline_us; last_now_us_ = now_us;
        facts_ = {}; exceeded_ = false; owned_ = true; return true;
    }
    bool observe(std::uint32_t generation, std::uint64_t now_us, CloseFacts facts) noexcept {
        if (!owned_ || generation != generation_ || now_us < last_now_us_) return false;
        if (destroy_owner_allowed()) return true; // Final classification is sticky.
        last_now_us_ = now_us;
        facts_.permission_revoked |= facts.permission_revoked;
        facts_.audio_stopped |= facts.audio_stopped;
        facts_.sender_finished |= facts.sender_finished;
        facts_.transport_closed |= facts.transport_closed;
        facts_.callbacks_quiesced |= facts.callbacks_quiesced;
        facts_.close_worker_finished |= facts.close_worker_finished;
        facts_.event_queue_drained |= facts.event_queue_drained;
        if (now_us > deadline_us_ || (now_us == deadline_us_ && !destroy_owner_allowed())) exceeded_ = true;
        return true;
    }
    bool sender_resources_release_allowed() const noexcept {
        return facts_.permission_revoked && facts_.sender_finished;
    }
    bool destroy_owner_allowed() const noexcept {
        return facts_.permission_revoked && facts_.audio_stopped && facts_.sender_finished &&
               facts_.transport_closed && facts_.callbacks_quiesced && facts_.close_worker_finished && facts_.event_queue_drained;
    }
    bool deadline_exceeded() const noexcept { return exceeded_; }
    CloseStage stage() const noexcept {
        if (!owned_) return CloseStage::Idle;
        if (destroy_owner_allowed()) return exceeded_ ? CloseStage::ClosedLate : CloseStage::ClosedOnTime;
        if (exceeded_) return CloseStage::DeadlineExceeded;
        if (!facts_.permission_revoked) return CloseStage::AwaitPermissionRevoke;
        if (!facts_.audio_stopped) return CloseStage::AwaitAudioStop;
        if (!facts_.sender_finished) return CloseStage::AwaitSender;
        if (!facts_.transport_closed) return CloseStage::AwaitTransport;
        if (!facts_.callbacks_quiesced) return CloseStage::AwaitCallbacks;
        if (!facts_.close_worker_finished) return CloseStage::AwaitWorker;
        return CloseStage::AwaitQueueDrain;
    }
private:
    std::uint32_t generation_{0};
    std::uint64_t deadline_us_{0}, last_now_us_{0};
    CloseFacts facts_{};
    bool owned_{false}, exceeded_{false};
};
} // namespace stopwatch::candidate
