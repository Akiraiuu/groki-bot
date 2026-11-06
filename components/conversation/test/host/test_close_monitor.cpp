// SPDX-License-Identifier: BSL-1.0
// Verbatim production methods; transport/audio/RTOS/time are synthetic.
#include "conversation/paid_voice_test.hpp"
#include "conversation/voice_control_policy.hpp"
#include "conversation/session_operation_gate.hpp"
#include "conversation/conversation_error.hpp"
#include <tl/expected.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>
namespace conv=stackchan::conversation;
namespace control=stopwatch::candidate;
namespace config { enum class Provider { Grok, Other }; }
static std::atomic<std::int64_t> clock_us{1000};
static std::int64_t now_us() {return clock_us.load();}
static std::int64_t esp_timer_get_time() {return now_us();}
static std::atomic<bool> escape_monitor{false};
struct TestEscape {};
static void vTaskDelay(unsigned) {
    if(escape_monitor.load()) throw TestEscape{}; // Harness only.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
#define pdMS_TO_TICKS(ms) (ms)
using TaskHandle_t=void*;
static constexpr int pdPASS=1;
static constexpr unsigned MALLOC_CAP_SPIRAM=1;
static std::vector<std::thread> workers;
static std::mutex workers_mutex;
static std::string fail_worker;
static bool inline_audio_worker=false;
static int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char* name,unsigned,void* arg,
                                         unsigned,TaskHandle_t* task,int,unsigned) {
    if(fail_worker==name) return 0;
    if(inline_audio_worker && std::string(name)=="voice_audio_close") {fn(arg);return pdPASS;}
    std::lock_guard lock{workers_mutex};
    *task=reinterpret_cast<void*>(1);
    workers.emplace_back([=] {try {fn(arg);} catch(const TestEscape&) {}});
    return pdPASS;
}
static void vTaskDeleteWithCaps(void*) {}
struct Block {
    std::mutex mutex;std::condition_variable cv;
    std::atomic<bool> entered{false},hold{false};
    void wait() {entered=true;std::unique_lock lock{mutex};cv.wait(lock,[&] {return !hold.load();});}
    void release() {{std::lock_guard lock{mutex};hold=false;}cv.notify_all();}
    void reset() {entered=false;hold=false;}
};
struct FakeDriver {
    Block block;
    std::atomic<bool> running{false},active{false};
    std::atomic<unsigned> ends{0};
    void end() {++ends;block.wait();running=false;active=false;}
    bool isRunning() const {return running.load();}
    unsigned isRecording() const {return active?1u:0u;}
    bool isPlaying() const {return active.load();}
    void reset(bool on) {block.reset();running=on;active=on;ends=0;}
};
static struct {FakeDriver Mic,Speaker;} M5;
struct FakeClient {
    std::atomic<unsigned> begins{0},flags{60};
    std::atomic<conv::TransportCloseStage> stage{conv::TransportCloseStage::Finished};
    std::int64_t begin_elapsed_us{0};
    void begin_controlled_close() {++begins;clock_us.fetch_add(begin_elapsed_us);}
    conv::ControlledCloseSnapshot controlled_close_snapshot() const {
        const auto bits=flags.load();return {true,begins!=0,bool(bits&4),bool(bits&8),bool(bits&16),bool(bits&32),conv::CloseFailure::None,stage.load()};
    }
};
enum class ConvStatus {Error};
struct FakeState {
    struct {std::atomic<ConvStatus> status{};} conv;
    std::atomic<bool> audio_stream_active{false};
    std::atomic<unsigned> failures_shown{0};
    void set_balloon_text(const char*,int) {++failures_shown;}
};
struct FakeInboxPolicy {
    std::atomic<unsigned> count{1};std::atomic<bool> revoked{false};
    unsigned pending() const {return count.load();}
    void revoke(std::uint32_t) {revoked=true;}
};
struct FakeInbox {FakeInboxPolicy p;FakeInboxPolicy& policy() {return p;}};
static std::atomic<bool> g_exclusive_voice_audio{false};
static FakeState speech_state;
static FakeState* g_state=&speech_state;
static unsigned speech_admitted=0;
#define ESP_LOGW(...) ((void)0)
static void local_speech_attempt() {
#include "generated_local_speech_guard.inc"
    ++speech_admitted; // Allocation/synthesis/task creation have not yet happened.
}
struct Coordinator {
    config::Provider provider_{config::Provider::Grok};
    FakeClient transport;FakeClient* client_=&transport;FakeInbox paid_inbox_;FakeState state_;
    std::uint32_t paid_session_id_{2};std::uint64_t paid_deadline_us_{60'000'000};
    std::atomic<unsigned> audio_close_facts_{0},audio_close_workers_{0},flushes{0};
    std::atomic<bool> audio_close_failed_{false};
    control::SessionOperationGate session_operations_;
    std::optional<control::SessionOperationGate::Lease> failed_wake_lease_;
    std::atomic<bool> paid_supervision_active_{true},supervisor_finished_{true},supervisor_late_{false};
    std::atomic<std::uint32_t> supervisor_facts_{0};
    std::atomic<conv::CloseFailure> supervisor_failure_{conv::CloseFailure::None};
    void flush_events() {++flushes;paid_inbox_.p.count=0;}
#include "generated_controlled_close.inc"
};
static unsigned checks=0;
static void check(bool value,const char* why) {++checks;if(!value) {std::cerr<<why<<'\n';std::abort();}}
template<class Predicate> static void until(Predicate predicate) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>=end) {std::cerr<<"observation timeout\n";std::abort();}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void join_workers(bool escape=false) {
    if(escape) escape_monitor=true;
    for(;;) {
        std::vector<std::thread> current;
        {std::lock_guard lock{workers_mutex};current.swap(workers);}
        if(current.empty()) break;
        for(auto& t:current) t.join();
    }
}
static void reset() {
    check(workers.empty(),"previous tasks joined");
    clock_us=1000;escape_monitor=false;fail_worker.clear();inline_audio_worker=false;M5.Mic.reset(true);M5.Speaker.reset(false);
    std::destroy_at(&conv::paid_voice_test);std::construct_at(&conv::paid_voice_test);
    conv::paid_voice_test.set_available(true,1);
    check(conv::paid_voice_test.request_start(true,0,1),"synthetic grant only");
    const auto claim=conv::paid_voice_test.claim_supervised(1);
    check(claim && claim->generation==2 && claim->deadline_ms==60000,"atomic immutable supervised scope");
}
static auto bootstrap(Coordinator& c) {
    check(c.session_operations_.begin(2,1000,c.paid_deadline_us_),"operation gate starts");
    auto b=c.session_operations_.adopt_bootstrap(2);check(bool(b),"wake and coordinator owners adopted");return b;
}
static void cancel() {conv::paid_voice_test.request_stop(conv::PaidVoiceTest::Reason::Cancelled);}
static void verify_on_time(Coordinator& c) {
    until([&] {return c.supervisor_finished_.load();});join_workers();
    check(conv::paid_voice_test.snapshot(0).close_facts==255 && !conv::paid_voice_test.snapshot(0).close_verified,
          "supervisor final ack not inferred from resources alone");
    c.await_supervision();
    check(conv::paid_voice_test.snapshot(0).close_facts==511 && conv::paid_voice_test.snapshot(0).close_verified,
          "all nine real facts verified on time");
    conv::paid_voice_test.finish();check(conv::paid_voice_test.snapshot(0).phase==conv::PaidVoiceTest::Phase::Idle,"verified close permits idle");
}
static void test_coordinator_retained() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();check(c.start_supervision(),"supervisor starts");cancel();
    until([&] {return c.transport.begins==1 && c.audio_close_workers_==3;});
    check(c.flushes==0 && !c.supervisor_finished_,"blocked coordinator retains buffers and queue owner");
    b->coordinator.reset();verify_on_time(c);
    check(c.transport.begins==1 && c.flushes==1,"one close and one safe drain");
}
static void test_late_ticket() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();check(c.start_supervision(),"supervisor starts before ticket");
    Block ticket;ticket.hold=true;std::optional<int> result;
    std::thread caller([&] {result=c.preparation_operation([&] {ticket.wait();return 42;});b->coordinator.reset();});
    until([&] {return ticket.entered.load();});clock_us=55'000'000;
    until([&] {return c.transport.begins==1 && conv::paid_voice_test.snapshot(0).supervision.preparation==1;});
    check(!conv::paid_voice_test.permitted(55000,2) && c.flushes==0,"independent reserve deadline revokes while ticket blocks");
    check(!c.operation_lease(control::ClientOperation),"late ticket cannot acquire new client publication lease");
    ticket.release();caller.join();check(!result,"late ticket discarded before client publication");verify_on_time(c);
}
static void test_client_and_audio_lease_stalls() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();check(c.start_supervision(),"supervisor starts");
    Block send,audio;send.hold=true;audio.hold=true;
    std::thread caller([&] {c.client_operation([&] {send.wait();});});
    std::thread player([&] {c.audio_operation([&] {audio.wait();return true;});});
    until([&] {return send.entered && audio.entered;});cancel();b->coordinator.reset();
    until([] {return conv::paid_voice_test.snapshot(0).supervision.client==1;});
    check(c.transport.begins==0 && M5.Mic.ends==0 && M5.Speaker.ends==0,"no concurrent close against active application operations");
    send.release();caller.join();until([&] {return c.transport.begins==1;});
    check(M5.Mic.ends==0,"transport close independent of audio operation");
    audio.release();player.join();verify_on_time(c);
}
static void test_driver_stall_and_late_completion() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();M5.Mic.block.hold=true;
    check(c.start_supervision(),"supervisor starts");cancel();b->coordinator.reset();
    until([] {return M5.Mic.block.entered.load();});
    check(c.transport.begins==1 && M5.Speaker.ends==0,"shared audio serialized; network independent");
    clock_us=60'000'001;until([] {return conv::paid_voice_test.snapshot(0).close_deadline_exceeded;});
    check(!conv::paid_voice_test.snapshot(0).available && !c.supervisor_finished_,"missed deadline retains stopping owner");
    M5.Mic.block.release();until([&] {return c.supervisor_finished_.load();});join_workers();
    std::thread waiter([&] {try {c.await_supervision();} catch(const TestEscape&) {}});
    until([] {return conv::paid_voice_test.snapshot(0).close_facts==511;});
    conv::paid_voice_test.finish();check(conv::paid_voice_test.snapshot(0).phase==conv::PaidVoiceTest::Phase::Stopping,
        "late real acknowledgements cannot turn deadline failure into idle");
    escape_monitor=true;waiter.join();check(c.transport.begins==1,"no implicit stop retry");
}
static void test_supervisor_creation_failure() {
    reset();Coordinator c;auto b=bootstrap(c);fail_worker="voice_supervisor";
    check(!c.start_supervision(),"task creation failure reported");
    bool ran=false;c.client_operation([&] {ran=true;});
    check(!ran && c.transport.begins==0 && M5.Mic.ends==0,"no new work or driver operation after missing supervisor");
    conv::paid_voice_test.finish();const auto s=conv::paid_voice_test.snapshot(0);
    check(s.phase==conv::PaidVoiceTest::Phase::Stopping && s.close_failure==conv::CloseFailure::WorkerStart && !s.available,
        "task allocation failure preserves finite failed stopping");
}
static void test_audio_creation_failure() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();fail_worker="voice_audio_close";
    check(c.start_supervision(),"supervisor starts");cancel();b->coordinator.reset();
    until([] {return conv::paid_voice_test.snapshot(0).close_failure==conv::CloseFailure::AudioFailed;});
    check(c.transport.begins==1 && M5.Mic.ends==0,"failed audio task not retried; transport still closed once");join_workers(true);
}
static void test_sdk_stage_visible_while_blocked() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();c.transport.flags=4;c.transport.stage=conv::TransportCloseStage::Stop;
    check(c.start_supervision(),"supervisor starts");cancel();b->coordinator.reset();
    until([] {return conv::paid_voice_test.snapshot(0).supervision.transport_stage==conv::TransportCloseStage::Stop;});
    check(c.flushes==0,"SDK wait stage published without treating invocation as close");
    c.transport.stage=conv::TransportCloseStage::Destroy;
    until([] {return conv::paid_voice_test.snapshot(0).supervision.transport_stage==conv::TransportCloseStage::Destroy;});
    c.transport.flags=60;c.transport.stage=conv::TransportCloseStage::Finished;verify_on_time(c);
}
static void test_retained_wake_and_external_audio() {
    reset();Coordinator c;auto b=bootstrap(c);check(c.start_supervision(),"supervisor before wake join");cancel();
    until([&] {return c.transport.begins==1 && conv::paid_voice_test.snapshot(0).supervision.audio==1;});
    check(M5.Mic.ends==0 && c.flushes==0,"unjoined local wake owner blocks audio close and queue release");
    c.state_.audio_stream_active=true;b->audio.reset();b->coordinator.reset();
    until([] {return conv::paid_voice_test.snapshot(0).supervision.audio==0;});
    check(M5.Mic.ends==0,"external audio flag prevents touching another owner");
    c.state_.audio_stream_active=false;verify_on_time(c);
}
static void test_local_speech_owner_gate() {
    g_exclusive_voice_audio=true;local_speech_attempt();
    check(speech_admitted==0,"settings and MCP local speech cannot bypass scoped voice audio owner");
    g_exclusive_voice_audio=false;local_speech_attempt();
    check(speech_admitted==1,"other modes keep their local speech path");
}
static void test_close_uses_fresh_time() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();b->coordinator.reset();
    c.transport.begin_elapsed_us=60'000'000;inline_audio_worker=true;cancel();
    check(c.start_supervision(),"supervisor starts before synthetic expensive close entry");
    until([&] {return c.supervisor_finished_.load();});join_workers();
    check(conv::paid_voice_test.snapshot(0).close_deadline_exceeded && c.supervisor_late_,
        "fresh time after close entry catches same-iteration late acknowledgements");
}
static void test_transferred_preparation_lease() {
    reset();Coordinator c;auto b=bootstrap(c);b->audio.reset();check(c.start_supervision(),"supervisor starts before transferable preparation");
    std::shared_ptr<void> retained;
    const auto result=c.preparation_operation([&](std::shared_ptr<void> hold={}) {retained=std::move(hold);return 17;});
    check(result && *result==17 && retained,"real preparation wrapper can transfer its move-only lease into an owned context");
    cancel();b->coordinator.reset();
    until([] {return conv::paid_voice_test.snapshot(0).supervision.preparation==1;});
    check(!c.supervisor_finished_ && c.flushes==0,"returned caller cannot fabricate quiescence while retained HTTP context still owns preparation");
    retained.reset();verify_on_time(c);
}
int main() {
    test_coordinator_retained();test_late_ticket();test_client_and_audio_lease_stalls();
    test_driver_stall_and_late_completion();test_supervisor_creation_failure();test_audio_creation_failure();
    test_sdk_stage_visible_while_blocked();test_retained_wake_and_external_audio();test_local_speech_owner_gate();test_close_uses_fresh_time();
    test_transferred_preparation_lease();
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"actual_coordinator_methods\":true,\"sdk_and_audio_stubbed\":true,\"device_accessed\":false}\n";
}
