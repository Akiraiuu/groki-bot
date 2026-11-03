#include "bounded_event_inbox.hpp"
#include "inbox_fault_bridge.hpp"
#include "paid_voice_test.hpp"
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>

using namespace stopwatch::candidate;
using Paid = stackchan::conversation::PaidVoiceTest;
static unsigned checks = 0;
static void check(bool good, const char* name) {
    ++checks;
    if (!good) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}
struct Queue {
    std::deque<void*> packets;
    bool reject{false};
    bool push(void* p) {
        if (reject || packets.size() >= 32) return false;
        packets.push_back(p); return true;
    }
    void* pop() {
        if (packets.empty()) return nullptr;
        auto* p = packets.front(); packets.pop_front(); return p;
    }
};
struct Event { int sequence; std::string text; };
using Inbox = BoundedEventInbox<Event, Queue, 32, 8>;
struct RejectAllocation {
    template<class Packet, class E, class Permit>
    static Packet* make(const E&, Permit&&) { return nullptr; }
};

static void test_order_and_backpressure() {
    Queue q; Inbox inbox(q); check(inbox.policy().begin(7), "scope starts");
    for (int i = 0; i < 24; ++i)
        check(inbox.push({i, "synthetic"}, EventClass::Payload, 7, 6), "payload packet accepted");
    check(inbox.push({24, "audio_done"}, EventClass::Control, 7, 7), "audio_done accepted behind audio");
    check(inbox.push({25, "response_done"}, EventClass::Control, 7, 10), "response_done accepted behind audio");
    check(!inbox.push({26, "overflow"}, EventClass::Payload, 7, 6), "payload overload produces failure");
    Paid paid; paid.set_available(true, 100); check(paid.request_start(true, 0, 100), "synthetic paid state starts");
    check(paid.claim(0), "synthetic paid state claimed");
    check(stop_on_inbox_failure(inbox.policy().first_fault(), paid), "overflow bridged into stopped state");
    auto s = paid.snapshot(1);
    check(s.phase == Paid::Phase::Stopping && s.fault == Paid::Fault::EventInbox, "overflow cannot masquerade as completed reply");
    for (int i = 0; i < 26; ++i) {
        auto packet = inbox.pop();
        check(packet && packet->event.sequence == i && packet->generation() == 7, "all admitted events remain FIFO");
    }
    check(!inbox.pop() && inbox.policy().pending() == 0, "consumer returns all queue credits");
    paid.request_stop(); paid.finish();
    s = paid.snapshot(2);
    check(s.fault == Paid::Fault::EventInbox && std::string(Paid::fault_name(s.fault)) == "event_inbox", "finite inbox fault survives stop and finish");
    check(!paid.request_start(true, 3, 100), "old nonce cannot clear inbox fault");
    check(paid.request_start(true, 3, s.approval_id), "new explicit synthetic scope accepted");
    check(paid.snapshot(4).fault == Paid::Fault::None, "new authorized scope clears fault");
    paid.request_stop(); paid.finish(); inbox.policy().revoke(7);
}

static void test_source_error_and_first_cause() {
    Queue q; Inbox inbox(q); check(inbox.policy().begin(8), "source error scope starts");
    for (int i = 0; i < 32; ++i)
        check(inbox.push({i, "control"}, EventClass::Control, 8, 10), "all control capacity filled");
    // No allocation and no queue insertion are needed to preserve the Error.
    inbox.policy().note_source_error(8, 11, 4);
    check(q.packets.size() == 32, "error latch independent from full queue");
    Paid paid; paid.set_available(true, 200); paid.request_start(true, 0, 200); paid.claim(0);
    check(stop_on_inbox_failure(inbox.policy().first_fault(), paid), "source error bridged out of band");
    auto s = paid.snapshot(1);
    check(s.fault == Paid::Fault::ConversationEvent && s.fault_detail == 4, "source category and bounded detail preserved");
    inbox.policy().note_queue_rejection(8, 7);
    check(inbox.policy().first_fault().kind == DeliveryFault::SourceError, "later full-queue error does not overwrite first cause");
    paid.request_stop(); paid.finish();
    inbox.policy().revoke(8);
    check(inbox.discard_quiescent() == 32 && inbox.policy().pending() == 0, "quiescent discard releases all envelopes");
    check(inbox.policy().begin(9), "new queue generation starts after complete drain");
    check(!inbox.push({1, "stale"}, EventClass::Control, 8, 7), "old producer cannot enqueue into new scope");
    check(inbox.policy().first_fault().kind == DeliveryFault::None, "stale event does not poison new first cause");
    inbox.policy().revoke(9);
}

static void test_rollback() {
    Queue q; Inbox inbox(q); check(inbox.policy().begin(40), "rejection scope starts");
    q.reject = true;
    check(!inbox.push({1, "rejected"}, EventClass::Payload, 40, 6), "backend rejection surfaced");
    check(inbox.policy().pending() == 0 && inbox.policy().payload_pending() == 0, "rejected enqueue destroys packet and releases lease");
    check(inbox.policy().first_fault().kind == DeliveryFault::QueueRejected, "backend rejection has fixed fault");
    inbox.policy().revoke(40);
    Queue q2;
    BoundedEventInbox<Event, Queue, 32, 8, RejectAllocation> no_memory(q2);
    check(no_memory.policy().begin(41), "allocation scope starts");
    check(!no_memory.push({1, "not copied"}, EventClass::Payload, 41, 6), "allocation failure surfaced");
    check(no_memory.policy().pending() == 0 && q2.packets.empty(), "allocation failure releases credit without queue entry");
    check(no_memory.policy().first_fault().kind == DeliveryFault::AllocationFailed, "allocation failure bounded category retained");
    no_memory.policy().revoke(41);
}

int main() {
    test_order_and_backpressure(); test_source_error_and_first_cause(); test_rollback();
    std::cout << "{\"status\":\"passed\",\"checks\":" << checks
              << ",\"device_accessed\":false,\"paid_api_called\":false,\"real_paid_voice_header_executed\":true}\n";
}
