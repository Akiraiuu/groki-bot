// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include "conversation/paid_voice_test.hpp"
#include "conversation/paid_voice_command.hpp"

using namespace stackchan::conversation;
using Phase = PaidVoiceTest::Phase;
using Reason = PaidVoiceTest::Reason;

int main()
{
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

    auto command = [](const char* json) {
        auto* root = cJSON_Parse(json);
        auto result = parse_paid_voice_command(root);
        cJSON_Delete(root);
        return result;
    };
    assert(command(R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1})") == PaidVoiceCommand::Start);
    assert(command(R"({"op":"stop"})") == PaidVoiceCommand::Stop);
    for (auto json : {"null", "[]", "{}",
        R"({"op":"start"})", R"({"op":"start","confirm_paid":"true","max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":false,"max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":2,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":600,"approval_id":1})",
        R"({"op":"start","op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1})",
        R"({"op":"start","confirm_paid":true,"max_rounds":1,"max_seconds":60,"approval_id":1,"auto_retry":true})"})
        assert(command(json) == PaidVoiceCommand::Invalid);
}
