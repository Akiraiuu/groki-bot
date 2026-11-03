#include "voice_control_policy.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

using namespace stopwatch::candidate;
using Policy = EventDeliveryPolicy<32, 8>;
static unsigned checks = 0;
static void check(bool good, const char* name) {
    ++checks;
    if (!good) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}

static void test_admission_and_faults() {
    Policy p;
    check(!p.begin(0), "zero generation rejected");
    check(p.begin(11), "first generation starts");
    check(!p.begin(12), "active scope cannot reset state");
    std::vector<Policy::Permit> payload;
    for (unsigned i = 0; i < 24; ++i) {
        auto token = p.try_admit(EventClass::Payload, 11, 6);
        check(token.has_value(), "payload credit available");
        payload.push_back(std::move(*token));
    }
    check(p.pending() == 24 && p.payload_pending() == 24, "payload cap enforced");
    auto audio_done = p.try_admit(EventClass::Control, 11, 7);
    auto response_done = p.try_admit(EventClass::Control, 11, 10);
    check(audio_done && response_done, "completion events have reserved admission");
    check(p.first_fault().kind == DeliveryFault::None, "normal reserved admission has no fault");
    auto overflow = p.try_admit(EventClass::Payload, 11, 6);
    check(!overflow, "additional payload rejected rather than consuming control reserve");
    check(p.first_fault().kind == DeliveryFault::PayloadCapacity, "overflow is explicit failure");
    p.note_source_error(11, 11, 4);
    check(p.first_fault().kind == DeliveryFault::PayloadCapacity, "first failure not overwritten");
    p.revoke(11);
    check(!p.try_admit(EventClass::Control, 11, 7), "no admission after revoke");
    check(!p.begin(12), "outstanding queue ownership blocks reset");
    payload.clear(); audio_done.reset(); response_done.reset();
    check(p.pending() == 0 && p.payload_pending() == 0, "all permits released");
    check(!p.begin(11), "same generation cannot clear first fault");
    check(p.first_fault().kind == DeliveryFault::PayloadCapacity, "failed reset preserves first fault");
    check(p.begin(12), "new quiescent generation resets policy");
    check(p.first_fault().kind == DeliveryFault::None, "new scope clears failure");
    auto stale = p.try_admit(EventClass::Payload, 11, 6);
    check(!stale && p.stale() == 1 && p.pending() == 0, "old callback cannot spend new credits");
    p.revoke(11);
    check(p.current(12), "old revoke cannot affect current scope");
    p.note_source_error(12, 11, 4);
    check(p.first_fault().kind == DeliveryFault::SourceError && p.first_fault().source_error == 4, "source error latched without queue");
    p.note_source_error(12, 11, 6);
    check(p.first_fault().source_error == 4, "source first detail preserved");
    p.revoke(12);

    Policy full;
    check(full.begin(21), "full control test starts");
    std::vector<Policy::Permit> controls;
    for (unsigned i = 0; i < 32; ++i) {
        auto token = full.try_admit(EventClass::Control, 21, 7);
        check(token.has_value(), "control credit available");
        controls.push_back(std::move(*token));
    }
    full.note_source_error(21, 11, 7);
    check(full.first_fault().kind == DeliveryFault::SourceError, "source error survives entirely full control queue");
    check(!full.try_admit(EventClass::Control, 21, 10), "full control admission explicitly fails");
    check(full.first_fault().source_error == 7, "capacity failure cannot replace source first cause");
    full.revoke(21); controls.clear();

    Policy unknown;
    check(unknown.begin(31), "unknown detail test starts");
    unknown.note_source_error(31, 11, 500);
    check(unknown.first_fault().source_error == -1, "unbounded error detail reduced to unknown");
    unknown.revoke(31);
}

static void test_move_rollback_and_order() {
    Policy p; check(p.begin(101), "ownership test starts");
    {
        auto first = p.try_admit(EventClass::Payload, 101, 6);
        check(first && p.pending() == 1, "one permit acquired");
        auto moved = std::move(*first);
        first.reset();
        check(p.pending() == 1, "moved-from destruction does not double release");
        moved.reset(); moved.reset();
        check(p.pending() == 0, "permit reset idempotent");
    }
    {
        auto token = p.try_admit(EventClass::Payload, 101, 6);
        p.note_queue_rejection(101, 6);
        check(token.has_value(), "queue rejection retains temporary owner until cleanup");
    }
    check(p.pending() == 0 && p.first_fault().kind == DeliveryFault::QueueRejected, "failed send rollback returns credit");
    p.revoke(101);
    check(p.begin(102), "FIFO scope starts");
    struct Envelope { int sequence; Policy::Permit permit; };
    std::deque<Envelope> fifo;
    for (int i = 0; i < 26; ++i) {
        auto token = p.try_admit(i < 24 ? EventClass::Payload : EventClass::Control, 102, static_cast<std::uint8_t>(i < 24 ? 6 : 7));
        check(token.has_value(), "FIFO envelope admitted");
        fifo.push_back({i, std::move(*token)});
    }
    for (int i = 0; i < 26; ++i) {
        check(fifo.front().sequence == i, "reserve does not reorder completion ahead of audio");
        fifo.pop_front();
    }
    check(p.pending() == 0, "FIFO drain returns all resources");
    p.revoke(102);
}

static void test_batch_budget() {
    EventBatchBudget budget(4, 2000, 10000);
    check(budget.may_process_next(10000), "batch initially available");
    for (unsigned i = 0; i < 4; ++i) budget.processed_one();
    check(!budget.may_process_next(10001), "event limit returns to playback even with backlog");
    budget.processed_one();
    check(budget.count() == 4, "count cannot wrap through extra acknowledgement");
    EventBatchBudget time(4, 2000, 10000);
    check(time.may_process_next(11999) && !time.may_process_next(12000), "elapsed limit excludes next handler");
    check(!time.may_process_next(9999), "clock reversal fails closed");
    EventBatchBudget disabled(0, 2000, 10000);
    check(!disabled.may_process_next(10000), "zero event budget refuses work");
    EventBatchBudget one_slow_handler(4, 2000, 0);
    check(one_slow_handler.may_process_next(0), "one handler can start");
    one_slow_handler.processed_one();
    check(!one_slow_handler.may_process_next(100000), "budget does not claim to preempt slow handler");
}

static CloseFacts all_closed() { return {true, true, true, true, true, true, true}; }
static void test_close_ownership() {
    CloseOwnershipPolicy p;
    check(p.stage() == CloseStage::Idle, "close initially idle");
    check(!p.begin_close(0, 0, 100) && !p.begin_close(1, 100, 100), "invalid closing scope rejected");
    check(p.begin_close(1, 0, 100), "closing starts");
    check(p.stage() == CloseStage::AwaitPermissionRevoke, "permission revoke must be real");
    check(!p.observe(99, 10, all_closed()), "stale close ack ignored");
    check(!p.destroy_owner_allowed(), "stale ack cannot free impl");
    check(p.observe(1, 10, {true, true, false, false, false, false}), "permission and audio acks accepted");
    check(p.stage() == CloseStage::AwaitSender && !p.sender_resources_release_allowed(), "sender-owned objects remain alive");
    check(!p.observe(1, 9, all_closed()), "reversed close clock cannot accept acks");
    check(p.observe(1, 100, {}), "deadline observation accepted");
    check(p.stage() == CloseStage::DeadlineExceeded && p.deadline_exceeded(), "timeout is explicit failure state");
    check(!p.destroy_owner_allowed() && !p.sender_resources_release_allowed(), "timeout is not resource release permission");
    check(!p.begin_close(2, 101, 201), "new scope cannot reuse live owner");
    check(p.observe(1, 110, all_closed()), "late real completion accepted");
    check(p.stage() == CloseStage::ClosedLate && p.deadline_exceeded(), "late completion cannot erase missed deadline");
    check(p.destroy_owner_allowed(), "actual all-thread and transport acks permit destruction");
    check(!p.begin_close(1, 120, 220), "same generation cannot reset deadline history");
    check(p.begin_close(2, 200, 300), "new closing scope only after previous owner safe");
    check(p.observe(2, 300, all_closed()), "completion exactly at deadline accepted");
    check(p.stage() == CloseStage::ClosedOnTime && !p.deadline_exceeded(), "on-time completion distinguished");
    check(p.observe(2, 500, {}), "duplicate post-close poll harmless");
    check(p.stage() == CloseStage::ClosedOnTime, "later poll cannot reclassify completed shutdown");
    for (unsigned missing = 0; missing < 7; ++missing) {
        CloseOwnershipPolicy incomplete; check(incomplete.begin_close(10 + missing, 0, 100), "partial facts scope starts");
        auto f = all_closed();
        switch (missing) {
            case 0: f.permission_revoked = false; break;
            case 1: f.audio_stopped = false; break;
            case 2: f.sender_finished = false; break;
            case 3: f.transport_closed = false; break;
            case 4: f.callbacks_quiesced = false; break;
            case 5: f.close_worker_finished = false; break;
            case 6: f.event_queue_drained = false; break;
        }
        incomplete.observe(10 + missing, 10, f);
        check(!incomplete.destroy_owner_allowed(), "every independent lifetime acknowledgement required");
    }
}

static void test_concurrent_admission() {
    Policy p; check(p.begin(500), "threaded scope starts");
    std::atomic<bool> failed{false};
    std::array<std::thread, 4> threads;
    for (unsigned n = 0; n < threads.size(); ++n) {
        threads[n] = std::thread([&, n] {
            for (unsigned i = 0; i < 25000; ++i) {
                auto token = p.try_admit((i + n) % 4 ? EventClass::Payload : EventClass::Control, 500, 6);
                if (!token || token->generation() != 500 || p.pending() > 32 || p.payload_pending() > 24)
                    failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(!failed.load() && p.pending() == 0 && p.payload_pending() == 0, "100000 real concurrent leases conserve credits");
    check(p.first_fault().kind == DeliveryFault::None, "under-capacity concurrency causes no false loss");
    p.revoke(500);
}

int main() {
    test_admission_and_faults();
    test_move_rollback_and_order();
    test_batch_budget();
    test_close_ownership();
    test_concurrent_admission();
    std::cout << "{\"status\":\"passed\",\"checks\":" << checks
              << ",\"concurrent_lease_attempts\":100000,\"device_accessed\":false,\"paid_api_called\":false}\n";
}
