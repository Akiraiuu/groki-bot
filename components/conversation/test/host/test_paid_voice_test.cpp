// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include "conversation/paid_voice_test.hpp"
#include "conversation/paid_voice_command.hpp"
#include "conversation/single_turn_input.hpp"
#include "conversation/farewell.hpp"

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


static void test_bounded_dialogue()
{
    using Profile = PaidVoiceTest::Profile;
    auto start_dialogue = [](PaidVoiceTest& p, std::int64_t now) {
        return p.request_start(true, now, p.snapshot(now).approval_id, PaidVoiceTest::Mode::Paid, Profile::Dialogue);
    };
    PaidVoiceTest idle;
    idle.set_available(true, 200);
    assert(start_dialogue(idle, 0) && idle.claim(1));
    auto id = idle.snapshot(1).approval_id;
    assert(idle.permitted(20000)); // Setup itself does not consume the idle wait.
    assert(idle.update_input_ready(20000, id, true));
    assert(idle.snapshot(20000).max_rounds == 3 && idle.snapshot(20000).idle_remaining_ms == 10000);
    assert(idle.update_input_ready(25000, id, true)); // Repeated audio updates cannot extend silence.
    assert(idle.snapshot(29999).idle_remaining_ms == 1);
    assert(!idle.permitted(30000));
    assert(idle.snapshot(30000).reason == Reason::IdleTimeout && !idle.snapshot(30000).input_ready);
    assert(!idle.note_user_activity(30000, id));
    idle.request_stop(Reason::Timeout); // Cleanup retains the specific end reason.
    assert(idle.snapshot(30001).reason == Reason::IdleTimeout);
    idle.finish();

    PaidVoiceTest turns;
    turns.set_available(true, 300);
    assert(start_dialogue(turns, 0) && turns.claim(1));
    id = turns.snapshot(1).approval_id;
    for (int round = 1; round <= 3; ++round) {
        const auto ready = round * 1000;
        assert(turns.update_input_ready(ready, id, true));
        assert(turns.note_user_activity(ready + 1, id));
        assert(turns.update_input_ready(ready + 2, id, false));
        assert(turns.snapshot(ready + 2).idle_remaining_ms == 0);
        assert(turns.reply_completed(ready + 500, id) == (round < 3));
        const auto snap = turns.snapshot(ready + 500);
        assert(snap.completed_rounds == static_cast<unsigned>(round));
        assert(snap.remaining_ms == 60000 - ready - 500); // Same original deadline.
        assert(!snap.input_ready);
    }
    assert(turns.snapshot(4000).reason == Reason::RoundLimit && !turns.permitted(4000));
    assert(!turns.reply_completed(4001, id));
    turns.finish();

    PaidVoiceTest busy;
    busy.set_available(true, 400);
    assert(start_dialogue(busy, 0) && busy.claim(1));
    id = busy.snapshot(1).approval_id;
    assert(busy.update_input_ready(10, id, true) && busy.note_user_activity(20, id));
    assert(busy.permitted(50000)); // User speech and assistant work are not idle waiting.
    assert(busy.reply_completed(50001, id));
    assert(busy.update_input_ready(50002, id, true));
    assert(busy.snapshot(59999).idle_remaining_ms == 3);
    assert(!busy.permitted(60000)); // Hard cap always wins; no per-turn extension.
    assert(busy.snapshot(60000).reason == Reason::Timeout);
    busy.finish();

    PaidVoiceTest ending;
    ending.set_available(true, 500);
    assert(start_dialogue(ending, 0) && ending.claim(1));
    id = ending.snapshot(1).approval_id;
    assert(ending.update_input_ready(2, id, true));
    ending.request_stop(Reason::Farewell);
    assert(!ending.permitted(3) && !ending.snapshot(3).input_ready);
    assert(ending.snapshot(3).reason == Reason::Farewell);
    ending.finish();
    assert(start_dialogue(ending, 100) && ending.claim(101));
    auto fresh = ending.snapshot(101).approval_id;
    assert(ending.update_input_ready(102, fresh, true));
    assert(!ending.note_user_activity(103, id)); // Old speech cannot cancel a new idle deadline.
    assert(!ending.reply_completed(103, id));
    assert(ending.snapshot(103).completed_rounds == 0);
    assert(!ending.reply_completed(104, fresh)); // No unsolicited/duplicate reply may reopen input.
    assert(ending.snapshot(104).reason == Reason::Error);

    PaidVoiceTest local;
    local.set_available(true, 1);
    assert(!local.request_start(true, 0, 1, PaidVoiceTest::Mode::Local, Profile::Dialogue));
    assert(!local.request_start(false, 0, 1, PaidVoiceTest::Mode::Paid, Profile::Dialogue));

    for (const auto text : {"再见", "拜拜！", "退下吧。", "不用了，谢谢", " 再 见 ", "“再见”", "结束对话", "Bye bye!"})
        assert(is_farewell(text));
    for (const auto text : {"", "继续", "再见的意思是什么", "不用了但是请继续", "不是再见", "介绍一下拜拜这个词", "不用", "退下吧的英文怎么说"})
        assert(!is_farewell(text));
    assert(!is_farewell(std::string(129, 'x')));
}


// Diagnostic metadata must survive teardown without keeping any paid permission.
// Detail is a bounded enum, never arbitrary server text.
static void test_failure_diagnostics()
{
    using Fault = PaidVoiceTest::Fault;
    PaidVoiceTest p;
    p.set_available(true);
    assert(p.snapshot(0).fault == Fault::None);
    assert(p.snapshot(0).fault_detail == -1);
    const std::array faults{Fault::Unspecified, Fault::Unavailable, Fault::UnexpectedReply,
        Fault::LocalCapture, Fault::LocalPlayback, Fault::WakeRelease, Fault::Connect,
        Fault::Recovery, Fault::AudioUpload, Fault::Microphone, Fault::InputTurnReset,
        Fault::ConversationEvent};
    for (const auto fault : faults) {
        assert(p.request_start(true, 100, p.snapshot(100).approval_id));
        assert(p.claim(101));
        const auto id = p.snapshot(101).approval_id;
        p.request_stop(Reason::Error, fault, 4);
        assert(!p.permitted(102, id));
        assert(p.snapshot(102).fault == fault);
        assert(p.snapshot(102).fault_detail == (fault == Fault::ConversationEvent ? 4 : -1));
        p.request_stop(Reason::Error, Fault::Connect);
        p.request_stop(Reason::Cancelled);
        p.finish();
        assert(p.snapshot(103).phase == Phase::Idle);
        assert(p.snapshot(103).reason == Reason::Error);
        assert(p.snapshot(103).fault == fault); // First cause survives STOP/finish.
        assert(p.snapshot(103).fault_detail == (fault == Fault::ConversationEvent ? 4 : -1));
    }
    assert(p.request_start(true, 200, p.snapshot(200).approval_id));
    assert(p.snapshot(201).fault == Fault::None); // Only a new explicit grant clears it.
    assert(p.snapshot(201).fault_detail == -1);
    p.request_stop(Reason::Cancelled, Fault::ConversationEvent, 4);
    p.finish();
    assert(p.snapshot(202).fault == Fault::None); // Non-error does not invent a failure.
    for (const auto detail : {-100, -1, 0, 7, 8, 100}) {
        assert(p.request_start(true, 300, p.snapshot(300).approval_id));
        assert(p.claim(301));
        p.request_stop(Reason::Error, Fault::ConversationEvent, detail);
        assert(p.snapshot(302).fault_detail == (detail >= 0 && detail <= 7 ? detail : -1));
        p.finish();
    }
    assert(p.request_start(true, 400, p.snapshot(400).approval_id,
        PaidVoiceTest::Mode::Paid, PaidVoiceTest::Profile::Dialogue));
    assert(p.claim(401));
    assert(!p.reply_completed(402, p.snapshot(402).approval_id));
    assert(p.snapshot(403).fault == Fault::UnexpectedReply);
    p.finish();
    assert(p.request_start(true, 500, p.snapshot(500).approval_id));
    p.set_available(false);
    assert(p.snapshot(501).fault == Fault::Unavailable);
}

int main()
{
    test_failure_diagnostics();
    test_bounded_dialogue();
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

    assert(command(R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":3,"max_seconds":60,"idle_timeout_seconds":10,"approval_id":1})") == PaidVoiceCommand::StartDialogue);
    for (const auto bad : {
        R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":3,"max_seconds":60,"approval_id":1})",
        R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":1,"max_seconds":60,"idle_timeout_seconds":10,"approval_id":1})",
        R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":3,"max_seconds":600,"idle_timeout_seconds":10,"approval_id":1})",
        R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":3,"max_seconds":60,"idle_timeout_seconds":11,"approval_id":1})",
        R"({"op":"start_dialogue","confirm_paid":true,"max_rounds":3,"max_seconds":60,"idle_timeout_seconds":10,"idle_timeout_seconds":10,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"idle_timeout_seconds":10,"approval_id":1})",
        R"({"op":"start_dialogue","confirm_paid":false,"max_rounds":3,"max_seconds":60,"idle_timeout_seconds":10,"approval_id":1})"})
        assert(command(bad) == PaidVoiceCommand::Invalid);

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
