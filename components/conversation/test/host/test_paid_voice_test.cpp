// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include "conversation/paid_voice_test.hpp"
#include "conversation/paid_voice_command.hpp"
#include "conversation/single_turn_input.hpp"

using namespace stackchan::conversation;
using Phase = PaidVoiceTest::Phase;
using Reason = PaidVoiceTest::Reason;

static void test_wifi_input_readiness()
{
    PaidVoiceTest p;
    assert(!p.snapshot(0).input_ready);
    assert(!p.update_input_ready(0, 1, true));
    p.set_available(true, 91);
    assert(p.request_start(true, 100, 91));
    auto id = p.snapshot(100).approval_id;
    assert(!p.snapshot(100).input_ready);
    assert(!p.update_input_ready(101, id, true)); // Queue is not an open microphone.
    assert(p.claim(102));
    assert(!p.snapshot(102).input_ready); // Session setup is not enough.
    assert(!p.update_input_ready(103, 0, true));
    assert(!p.update_input_ready(103, 91, true)); // Pre-start approval is stale.
    assert(p.update_input_ready(104, id, true));
    assert(p.snapshot(104).input_ready);
    assert(p.update_input_ready(105, id, false));
    assert(!p.snapshot(105).input_ready); // Transcript/output can close input independently.
    assert(p.update_input_ready(106, id, true));
    assert(p.snapshot(106).remaining_ms == 59994); // Readiness never extends permission.
    assert(!p.update_input_ready(-1, id, true));
    assert(!p.snapshot(60100).input_ready); // Exact deadline clears even before teardown.
    assert(!p.update_input_ready(60100, id, true));
    p.finish();
    assert(!p.snapshot(60101).input_ready);

    for (auto reason : {Reason::Completed, Reason::Cancelled, Reason::Error, Reason::Interrupted}) {
        const auto before = p.snapshot(70000).approval_id;
        assert(p.request_start(true, 70000, before));
        assert(p.claim(70001));
        const auto current = p.snapshot(70001).approval_id;
        assert(p.update_input_ready(70002, current, true));
        assert(!p.update_input_ready(70003, id, false)); // Old callbacks cannot clear a new session.
        assert(!p.update_input_ready(70003, id, true));
        assert(p.snapshot(70003).input_ready);
        p.request_stop(reason);
        assert(!p.snapshot(70004).input_ready);
        assert(!p.update_input_ready(70005, current, true));
        p.finish();
        id = current;
    }
    auto before = p.snapshot(80000).approval_id;
    assert(p.request_start(true, 80000, before));
    assert(p.claim(80001));
    id = p.snapshot(80001).approval_id;
    assert(p.update_input_ready(80002, id, true));
    p.set_available(false);
    assert(!p.snapshot(80003).input_ready);
    assert(!p.update_input_ready(80004, id, true));
    p.finish();

    PaidVoiceTest local;
    local.set_available(true, 8);
    assert(local.request_start(true, 0, 8, PaidVoiceTest::Mode::Local));
    assert(local.claim(1, PaidVoiceTest::Mode::Local));
    assert(!local.update_input_ready(2, local.snapshot(2).approval_id, true));
    assert(!local.snapshot(2).input_ready && !local.permitted(2));

    PaidVoiceTest concurrent;
    concurrent.set_available(true, 11);
    assert(concurrent.request_start(true, 0, 11) && concurrent.claim(1));
    id = concurrent.snapshot(1).approval_id;
    std::thread producer([&] { for (int i = 0; i < 1000; ++i) concurrent.update_input_ready(2, id, true); });
    concurrent.request_stop();
    producer.join();
    assert(!concurrent.snapshot(3).input_ready);
    PaidVoiceTest rebooted;
    assert(!rebooted.snapshot(3).input_ready);
}

int main()
{
    test_wifi_input_readiness();
    SingleTurnInput input;
    ConversationEvent event{};
    // Regression: the short VAD noise event immediately after connection had
    // replaced every subsequent frame with silence before the user could speak.
    for (auto type : {ConversationEventType::SpeechStarted, ConversationEventType::SpeechStopped,
                      ConversationEventType::SpeechStarted, ConversationEventType::SpeechStopped}) {
        event.type = type;
        input.observe(event);
        assert(input.open());
    }
    event.type = ConversationEventType::UserTranscript;
    for (auto text : {"", " \r\n\t"}) { event.text = text; input.observe(event); assert(input.open()); }
    event.text = "Hello, introduce yourself.";
    input.observe(event);
    assert(!input.open());
    event.type = ConversationEventType::SpeechStarted;
    input.observe(event);
    assert(!input.open()); // Subsequent VAD cannot open a second utterance.
    input.reset();
    assert(input.open()); // Only a new explicitly approved test resets input.

    PaidVoiceTest test;
    assert(!test.permitted(0)); // Boot is offline, even with stored credentials.
    assert(!test.request_start(true, 0, 1)); // Coordinator not ready.
    test.set_available(true);
    auto start = [&](bool confirmed, std::int64_t now) { return test.request_start(confirmed, now, test.snapshot(now).approval_id); };
    assert(!start(false, 100));
    assert(!start(true, -1));
    assert(start(true, 100));
    assert(!test.permitted(101)); // Queued permission cannot send yet.
    assert(!start(true, 101)); // Duplicate must not extend deadline.
    assert(test.claim(200));
    const auto first_test_id = test.snapshot(200).approval_id;
    assert(test.permitted(200, first_test_id));
    assert(!test.claim(201));
    assert(test.permitted(60099));
    assert(!test.permitted(60100)); // Exact hard limit, including queue time.
    assert(test.snapshot(60101).reason == Reason::Timeout);
    assert(!start(true, 60101)); // No restart during cleanup.
    test.finish();
    assert(!test.permitted(60102));
    assert(test.snapshot(60102).phase == Phase::Idle);

    for (auto reason : {Reason::Completed, Reason::Cancelled, Reason::Error, Reason::Interrupted}) {
        assert(start(true, 70000));
        assert(test.claim(70001));
        assert(!test.permitted(70001, first_test_id)); // Old transports cannot reuse a fresh approval.
        test.request_stop(reason);
        assert(!test.permitted(70002));
        test.request_stop(Reason::Timeout); // Cleanup must retain original reason.
        assert(test.snapshot(70002).reason == reason);
        test.finish();
        assert(!test.claim(70003)); // Reconnect is never automatic.
    }
    assert(start(true, 100000));
    assert(!test.claim(160000)); // Expired request never opens a connection.
    test.finish();
    assert(start(true, 200000));
    test.request_stop(); // Cancelling before claim cannot start later.
    assert(!test.claim(200001));
    test.finish();
    const auto old_id = test.snapshot(200001).approval_id;
    test.request_stop();
    assert(!test.request_start(true, 200002, old_id)); // Stop beats a delayed start.
    PaidVoiceTest rebooted;
    assert(!rebooted.claim(200002));
    assert(!rebooted.permitted(200002));

    // Simultaneous HTTP starts cannot admit multiple paid sessions.
    std::atomic<unsigned> accepted{0};
    std::array<std::thread, 16> threads;
    for (auto& thread : threads) thread = std::thread([&] { if (start(true, 300000)) ++accepted; });
    for (auto& thread : threads) thread.join();
    assert(accepted == 1);
    test.set_available(false);
    assert(!test.permitted(300001));

    // Local microphone permission must never open the paid network gate.
    PaidVoiceTest local;
    local.set_available(true, 456);
    assert(local.request_start(true, 0, 456, PaidVoiceTest::Mode::Local));
    assert(!local.permitted(1));
    assert(!local.claim(1)); // Paid worker cannot claim local work.
    assert(local.claim(1, PaidVoiceTest::Mode::Local));
    assert(local.local_permitted(1));
    assert(!local.permitted(1));
    assert(!local.permitted(1, local.snapshot(1).approval_id));
    assert(!local.request_start(true, 2, local.snapshot(2).approval_id));
    assert(local.local_permitted(14999));
    assert(!local.local_permitted(15000));
    assert(local.snapshot(15000).reason == Reason::Timeout);
    local.finish();
    assert(local.request_start(true, 20000, local.snapshot(20000).approval_id, PaidVoiceTest::Mode::Local));
    local.request_stop();
    assert(!local.claim(20001, PaidVoiceTest::Mode::Local));
    assert(!local.permitted(20001));

    auto command = [](const char* json) {
        auto* root = cJSON_Parse(json);
        auto result = parse_paid_voice_command(root);
        cJSON_Delete(root);
        return result;
    };
    assert(command(R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1})") == PaidVoiceCommand::Start);
    assert(command(R"({"op":"stop"})") == PaidVoiceCommand::Stop);
    assert(command(R"({"op":"record_local","confirm_local_recording":true,"record_seconds":5,"approval_id":1})") == PaidVoiceCommand::LocalRecord);
    for (auto json : {"null", "[]", "{}",
        R"({"op":"record_local","confirm_local_recording":false,"record_seconds":5,"approval_id":1})",
        R"({"op":"record_local","confirm_local_recording":true,"record_seconds":8,"approval_id":1})",
        R"({"op":"record_local","confirm_local_recording":true,"record_seconds":5,"approval_id":0})",
        R"({"op":"record_local","confirm_local_recording":true,"record_seconds":5,"approval_id":1,"confirm_paid":true})",
        R"({"op":"record_local","op":"start","confirm_local_recording":true,"record_seconds":5,"approval_id":1})",
        R"({"op":"start"})", R"({"op":"start","confirm_paid":"true","max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":false,"max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":2,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":600,"approval_id":1})",
        R"({"op":"start","op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1,"auto_retry":true})"})
        assert(command(json) == PaidVoiceCommand::Invalid);
}
