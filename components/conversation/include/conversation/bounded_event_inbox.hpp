// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "voice_control_policy.hpp"
#include <memory>
#include <new>

namespace stopwatch::candidate {
struct NothrowPacketFactory {
    template<class Packet, class Event, class Permit>
    static Packet* make(const Event& event, Permit&& permit) {
        return new (std::nothrow) Packet(event, std::forward<Permit>(permit));
    }
};

// QueueBackend transfers void* FIFO ownership with nonblocking push/pop.
// Its lifetime and this object's lifetime must exceed every queued Packet.
template<class Event, class QueueBackend, std::uint32_t Capacity,
         std::uint32_t Reserve, class Factory = NothrowPacketFactory>
class BoundedEventInbox {
public:
    using Policy = EventDeliveryPolicy<Capacity, Reserve>;
    struct Packet {
        Event event;
        typename Policy::Permit permit;
        Packet(const Event& value, typename Policy::Permit&& admission)
            : event(value), permit(std::move(admission)) {}
        std::uint32_t generation() const noexcept { return permit.generation(); }
    };
    explicit BoundedEventInbox(QueueBackend& queue) : queue_(queue) {}
    BoundedEventInbox(const BoundedEventInbox&) = delete;
    BoundedEventInbox& operator=(const BoundedEventInbox&) = delete;
    Policy& policy() noexcept { return policy_; }
    const Policy& policy() const noexcept { return policy_; }
    bool push(const Event& event, EventClass kind, std::uint32_t generation,
               std::uint8_t event_type) {
        auto admission = policy_.try_admit(kind, generation, event_type);
        if (!admission) return false;
        std::unique_ptr<Packet> packet(Factory::template make<Packet>(event, std::move(*admission)));
        if (!packet) {
            policy_.note_allocation_failure(generation, event_type);
            return false;
        }
        if (!queue_.push(packet.get())) {
            policy_.note_queue_rejection(generation, event_type);
            return false;
        }
        (void)packet.release(); // Queue now owns the packet and its admission credit.
        return true;
    }
    std::unique_ptr<Packet> pop() {
        return std::unique_ptr<Packet>(static_cast<Packet*>(queue_.pop()));
    }
    // Only AFTER revoke AND producer quiescence. Never wait on an active producer.
    std::uint32_t discard_quiescent() {
        std::uint32_t count = 0;
        while (count < Capacity) {
            auto packet = pop();
            if (!packet) break;
            ++count;
        }
        return count;
    }
private:
    QueueBackend& queue_;
    Policy policy_;
};
} // namespace stopwatch::candidate
