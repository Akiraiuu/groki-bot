// SPDX-License-Identifier: BSL-1.0
#include "host_transport.hpp"
#include "conversation/openai_realtime_client.hpp"
#include "conversation/paid_voice_test.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
using namespace stackchan::conversation;
static unsigned checks=0;
static void check(bool ok,const char* message) {
    ++checks; if (!ok) { std::cerr<<message<<'\n';std::abort(); }
}
static ConversationConfig config(bool& permitted) {
    ConversationConfig c;c.session_deadline_us=60000000;
    c.agent_id="agent_test_only";
    c.input_sample_rate_hz=c.output_sample_rate_hz=24000;
    c.session_permitted=[&permitted] { return permitted; };
    return c;
}
static void reset() {
    check(host_pending_tasks.empty(),"all prior fake tasks returned");
    host_stop_calls=host_destroy_calls=host_cancel_calls=host_cleanup_calls=0;host_task_stopped=true;host_cleanup_result=host_cancel_result=ESP_OK;host_clock_us=1000000;
    host_stop_result=host_destroy_result=ESP_OK;
    host_stop_hook=nullptr;host_fail_task_name.clear();
}
static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool stop_entered=false,allow_stop=false;
static void stalled_stop() {
    std::unique_lock lock{gate_mutex};stop_entered=true;gate_cv.notify_all();
    gate_cv.wait(lock,[] { return allow_stop; });
}
static void test_blocked_sdk_stop() {
    reset();bool permitted=true;
    OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);
    unsigned events=0;client.set_event_callback([&](const auto&) { ++events; });
    check(bool(client.start(config(permitted))),"synthetic transport starts");
    permitted=false;client.begin_controlled_close();
    const auto initial=client.controlled_close_snapshot();
    check(initial.stage==TransportCloseStage::AwaitSender,"stage published before sender acknowledgement");
    check(initial.requested && !initial.worker_finished && host_stop_calls==0,
          "request returns without running SDK stop on application task");
    check(host_cancel_calls==1 && tls_scope_observe(host_socket->scope,host_socket->generation,host_clock_us).cancelled,"actual application cancels scope before waiting for sender");
    host_run_one_task(); // Actual sender exit/last-access acknowledgement.
    check(client.controlled_close_snapshot().sender_finished,"sender really acknowledged");
    stop_entered=allow_stop=false;host_stop_hook=&stalled_stop;
    std::thread closer([] { host_run_one_task(); });
    { std::unique_lock lock{gate_mutex};gate_cv.wait(lock,[] { return stop_entered; }); }
    const auto blocked=client.controlled_close_snapshot();
    check(blocked.stage==TransportCloseStage::Stop,"stage published before entering blocking SDK stop");
    check(!blocked.transport_closed && !blocked.worker_finished && host_destroy_calls==0,
          "blocked SDK stop never becomes closed or frees transport");
    client.begin_controlled_close();
    check(host_pending_tasks.empty() && host_stop_calls==1,"duplicate close does not create/retry a worker");
    const auto before=events;
    host_receive("{}");
    check(events==before,"closing callbacks cannot reach application or audio");
    check(!client.start(config(permitted)),"live close owner cannot be reused");
    { std::lock_guard lock{gate_mutex};allow_stop=true; } gate_cv.notify_all();closer.join();
    host_stop_hook=nullptr;
    const auto closed=client.controlled_close_snapshot();
    check(closed.stage==TransportCloseStage::Finished,"finished stage precedes final worker acknowledgement");
    check(closed.sender_finished && closed.transport_closed && closed.callbacks_quiesced &&
          closed.worker_finished && closed.failure==CloseFailure::None,"only actual SDK return completes transport facts");
    check(host_stop_calls==1 && host_destroy_calls==1 && host_socket==nullptr,"exactly one stop and destroy");
    client.stop();host_run_all_tasks();
    check(host_stop_calls==1 && host_destroy_calls==1,"post-close stop remains idempotent");
}
static void test_uncertain_stop_retains_owner() {
    reset();bool permitted=true;
    {
        OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);
        check(bool(client.start(config(permitted))),"stop failure scope starts");
        permitted=false;host_stop_result=ESP_FAIL;
        client.begin_controlled_close();host_run_all_tasks();
        const auto s=client.controlled_close_snapshot();
        check(s.failure==CloseFailure::StopFailed && s.worker_finished && !s.transport_closed,
              "uncertain stop returns finite failure, never closed");
        check(host_destroy_calls==0 && host_last_queue!=nullptr,"uncertain stop retains queue and SDK owner");
        check(!client.start(config(permitted)),"failure cannot recycle transport");
    } // Intrusive quarantine keeps the incomplete owner reachable; no retry.
    check(host_stop_calls==1 && host_destroy_calls==0,"destructor never retries an uncertain stop or frees it");
}
static void test_creation_and_destroy_failures() {
    reset();bool permitted=true;
    {
        OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);
        check(bool(client.start(config(permitted))),"worker allocation failure scope starts");
        permitted=false;host_fail_task_name="rt_close";
        client.begin_controlled_close();host_run_all_tasks();
        const auto s=client.controlled_close_snapshot();
        check(s.failure==CloseFailure::WorkerStart && !s.transport_closed,"worker creation failure cannot claim closed");
    }
    check(host_stop_calls==0 && host_destroy_calls==0,"worker failure has no implicit retry/destructor cleanup");
    reset();permitted=true;
    {
        OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);
        check(bool(client.start(config(permitted))),"destroy failure scope starts");
        permitted=false;host_destroy_result=ESP_FAIL;
        client.begin_controlled_close();host_run_all_tasks();
        const auto s=client.controlled_close_snapshot();
        check(s.failure==CloseFailure::DestroyFailed && s.transport_closed,"failed destroy preserves owner");
    }
    check(host_stop_calls==1 && host_destroy_calls==1,"failed destroy not retried");
}
static void test_already_stopped_and_empty() {
    reset();bool permitted=true;
    {
        OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);
        check(bool(client.start(config(permitted))),"already-stopped scope starts");
        host_socket->connected=false;
        host_socket->callback(host_socket->arg,nullptr,WEBSOCKET_EVENT_FINISH,nullptr);
        permitted=false;host_stop_result=ESP_FAIL;host_task_stopped=false;
        client.begin_controlled_close();host_run_all_tasks();
        const auto s=client.controlled_close_snapshot();
        check(!s.transport_closed && s.failure==CloseFailure::StopFailed && host_destroy_calls==0,
              "FINISH event alone is never a task exit or physical close acknowledgement");
    }
    reset();
    OpenAiRealtimeClient empty("",RealtimeBackend::Xai);
    empty.begin_controlled_close();const auto s=empty.controlled_close_snapshot();
    check(s.transport_closed && s.worker_finished && host_pending_tasks.empty(),"never-started empty client needs no worker or SDK call");
}
static void test_finish_requires_all_real_facts() {
    using Paid=PaidVoiceTest;
    for (unsigned missing=0;missing<9;++missing) {
        Paid p;p.set_available(true,1);
        check(p.request_start(true,0,1) && p.claim(0),"guard scope starts");
        p.require_verified_close();p.request_stop(Paid::Reason::Completed);
        p.observe_close(all_close_facts & ~(1u<<missing),false,CloseFailure::None);p.finish();
        check(p.snapshot(1).phase==Paid::Phase::Stopping,"every missing real ack blocks finish");
        p.observe_close(all_close_facts,false,CloseFailure::None);p.finish();
        check(p.snapshot(2).phase==Paid::Phase::Idle && p.snapshot(2).close_verified,"all real on-time facts permit finish");
    }
    Paid p;p.set_available(true,1);p.request_start(true,0,1);p.claim(0);p.require_verified_close();
    p.request_stop(Paid::Reason::Error,Paid::Fault::ConversationEvent,4);
    p.observe_close(0,true,CloseFailure::None);
    p.observe_close(all_close_facts,false,CloseFailure::None);p.finish();
    const auto s=p.snapshot(1);
    check(s.phase==Paid::Phase::Stopping && !s.available && !s.close_verified && s.close_deadline_exceeded,
          "late completion preserves failed deadline and blocks new grant");
    check(s.fault==Paid::Fault::ConversationEvent && s.fault_detail==4 && s.close_failure==CloseFailure::Deadline,
          "close failure never overwrites original finite voice fault");
    check(!p.request_start(true,2,s.approval_id),"no automatic new grant after late close");
}
static void test_setup_racing_expiry() {
    PaidVoiceTest p;p.set_available(true,1);
    check(p.request_start(true,0,1) && p.claim(59999),"grant claimed just before original expiry");
    check(p.snapshot(60000).phase==PaidVoiceTest::Phase::Stopping && p.scope_deadline_ms()==60000,
          "exact deadline survives expiry instead of being reconstructed from zero remaining");
    p.require_verified_close();p.finish();
    check(p.snapshot(60001).phase==PaidVoiceTest::Phase::Stopping && p.snapshot(60001).close_required,
          "setup racing expiry cannot bypass verified-close gate");
    p.observe_close(all_close_facts,true,CloseFailure::None);p.finish();
    check(!p.snapshot(60001).close_verified && !p.snapshot(60001).available,"expired setup cannot gain new time from late closure");
}
static void test_supervised_admission() {
    PaidVoiceTest p;p.require_supervised_admission();p.set_available(true,1);
    check(!p.snapshot(0).start_ready && !p.request_start(true,0,1),"cannot queue paid scope behind local preparation");
    p.set_supervised_admission(true);
    check(p.snapshot(0).start_ready && p.request_start(true,0,1),"coordinator checkpoint accepts one grant");
    p.set_supervised_admission(false);const auto claimed=p.claim_supervised(10);
    check(claimed && claimed->generation==2 && claimed->deadline_ms==60000,"closing admission does not discard already accepted scope");
    p.request_stop(PaidVoiceTest::Reason::Cancelled);p.finish();
    check(p.snapshot(10).phase==PaidVoiceTest::Phase::Stopping && !p.snapshot(10).start_ready,"supervised atomic claim installs close gate before expiry race");
}
static void test_deadline_and_first_failure() {
    reset();bool permitted=true;
    {OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);auto cfg=config(permitted);cfg.session_deadline_us=host_clock_us;
    check(!client.start(cfg),"already-expired original scope never creates transport");}
    reset();
    {OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);check(bool(client.start(config(permitted))),"pending cleanup test starts");
    permitted=false;client.begin_controlled_close();host_cleanup_result=ESP_WEBSOCKET_CLOSE_PENDING;host_clock_us=60000000;host_run_all_tasks();
    const auto s=client.controlled_close_snapshot();check(s.failure==CloseFailure::Deadline && !s.transport_closed && s.worker_finished,"pending close at original end is not closed and gets no renewed budget");
    check(host_destroy_calls==0 && host_cancel_calls==1 && host_cleanup_calls==1,"no destroy, restart or extra cleanup after deadline");}
    reset();permitted=true;
    {OpenAiRealtimeClient client("synthetic-token",RealtimeBackend::Xai);check(bool(client.start(config(permitted))),"first failure test starts");
    permitted=false;host_cancel_result=ESP_FAIL;client.begin_controlled_close();host_clock_us=60000000;host_cleanup_result=ESP_WEBSOCKET_CLOSE_PENDING;host_run_all_tasks();
    const auto s=client.controlled_close_snapshot();check(s.failure==CloseFailure::StopFailed && s.worker_finished && !s.transport_closed,"later timeout preserves first finite stop failure");}
}
int main() {
    test_blocked_sdk_stop();test_uncertain_stop_retains_owner();test_creation_and_destroy_failures();
    test_already_stopped_and_empty();test_finish_requires_all_real_facts();test_setup_racing_expiry();test_supervised_admission();
    test_deadline_and_first_failure();
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks
             <<",\"actual_client_executed\":true,\"sdk_and_rtos_stubbed\":true,\"device_accessed\":false}\n";
}
