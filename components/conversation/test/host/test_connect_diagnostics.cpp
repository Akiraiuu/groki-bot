// SPDX-License-Identifier: BSL-1.0
#include "host_transport.hpp"
#include "esp_http_client.h"
#include "conversation/openai_realtime_client.hpp"
#include "conversation/saved_agent.hpp"
#include "conversation/paid_voice_test.hpp"
#include "conversation/session_operation_gate.hpp"
#include "ticket_http_diagnostics.hpp"
#include "cJSON.h"
#include <atomic>
#include <cassert>
#include <ctime>
#include <iostream>
#include <thread>
#include <type_traits>

namespace conv=stackchan::conversation;
namespace conversation=stackchan::conversation;
namespace control=stopwatch::candidate;
namespace config { enum class Provider { Grok, Gemini, XiaoZhi, OpenAi }; }
using namespace stackchan::conversation;
static unsigned checks=0,cases=0;
struct WakeStatusFixture {
    std::atomic<bool> enabled{},check_only{},microphone_active{},worker_active{};
    std::atomic<int> status{},stage{},error{},previous_stage{},stack_free_bytes{},frames{},samples_per_frame{},
        peak{},mean_abs_peak{},clipped_samples{},timeouts{},rejected{},detect_max_us{},last_command{},
        best_target_per_mille{},best_other_per_mille{};
    std::uint32_t remaining(std::uint32_t)const{return UINT32_MAX;}
};
namespace stackchan::conversation { inline WakeStatusFixture local_wake_status; }
constexpr unsigned MALLOC_CAP_INTERNAL=2;
static std::uint32_t heap_caps_get_free_size(unsigned){return UINT32_MAX;}
static int esp_reset_reason(){return 0;}
struct httpd_req_t {std::string body;};
static bool require_auth(httpd_req_t*){return true;}
static int send_error(httpd_req_t*,const char*,const char*){return ESP_FAIL;}
static int send_json(httpd_req_t* r,std::string body){r->body=std::move(body);return ESP_OK;}
static int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
#include "generated_voice_status.inc"
static void check(bool value,const char* msg){++checks;if(!value){std::cerr<<msg<<'\n';std::abort();}}
constexpr auto kInstructions="synthetic";
#define CONFIG_STACKCHAN_OPENAI_REALTIME_MODEL "synthetic"
#define CONFIG_STACKCHAN_OPENAI_VOICE "synthetic"
class ConnectHarness {
public:
    config::Provider provider_{config::Provider::Grok};
    std::string xai_token_url_,xai_device_token_=std::string(43,'d'),system_prompt_,extra_headers_;
    std::uint32_t paid_session_id_{};
    std::uint64_t paid_deadline_us_{60000000};
    std::uint32_t mic_sample_rate_{24000},speaker_sample_rate_{24000};
    std::unique_ptr<OpenAiRealtimeClient> client_;
    ConversationConfig config_;
    stopwatch::candidate::SessionOperationGate session_operations_;
    static std::int64_t now_us(){return esp_timer_get_time();}
    bool preparation_admitted{true},client_admitted{true},client_call_admitted{true};
    enum class Local { Init };
    void set_local(Local){}
    void enqueue_event(const ConversationEvent&,std::uint32_t){}
    ToolDefinition make_set_expression_tool(){return {};}
    ToolDefinition make_set_head_pose_tool(){return {};}
    ToolDefinition make_speak_katakoto_tool(){return {};}
    template<class Fn> auto preparation_operation(Fn&& fn)->std::optional<std::invoke_result_t<Fn>> {
        if(!preparation_admitted)return std::nullopt;
        return fn(std::make_shared<int>(1)); // SDK operation admission is the modeled boundary.
    }
    enum {ClientOperation=1};
    std::optional<int> operation_lease(std::uint32_t){return client_admitted?std::optional<int>(1):std::nullopt;}
    template<class Fn> auto client_operation(Fn&& fn)->std::invoke_result_t<Fn> {
        using R=std::invoke_result_t<Fn>;
        if constexpr(std::is_same_v<R,tl::expected<void,ConversationError>>) {
            if(!client_call_admitted)return tl::unexpected{ConversationError::InvalidState};
        }
        return fn();
    }
#include "generated_connect.inc"
};
// Production namespace constant used by the extracted connect method.
// The real operation gate itself remains covered by its existing host targets.
static std::string ticket_body(){
    return "{\"version\":1,\"token\":\"synthetic-ephemeral-token\",\"expires_at\":"+
        std::to_string(std::time(nullptr)+300)+",\"agent_id\":\"agent_test_only\",\"websocket_url\":\"wss://api.x.ai/v1/realtime?agent_id=agent_test_only\"}";
}
static void reset(){
    check(host_pending_tasks.empty(),"all prior fake workers finished");
    host_http_init_fail=host_http_setup_fail=false;host_http_perform_result=ESP_OK;
    host_http_cleanup_result=ESP_OK;host_http_status=200;host_http_headers.clear();
    host_last_tls_result=host_last_tls_native=host_last_tls_flags=host_last_tls_reads=0;
    host_http_body=ticket_body();host_http_perform_hook=nullptr;
    host_queue_create_fail=host_ws_init_fail=host_ws_start_fail=false;
    host_fail_heap_alloc_at_least=static_cast<std::size_t>(-1);host_fail_task_name.clear();
    host_stop_result=host_destroy_result=host_cancel_result=host_cleanup_result=ESP_OK;
    host_task_stopped=true;host_clock_us=1000000;
}
static std::uint32_t generation=100;
static void begin(ConnectHarness& h){
    reset();auto& paid=conv::paid_voice_test;
    paid.request_stop();paid.finish();paid.set_available(true,++generation*10);
    const auto id=paid.snapshot(1000).approval_id;
    check(paid.request_start(true,1000,id),"synthetic explicit grant");
    check(paid.claim(1000),"synthetic grant claimed");
    h.paid_session_id_=paid.snapshot(1000).approval_id;
    check(h.session_operations_.begin(h.paid_session_id_,host_clock_us,h.paid_deadline_us_),"synthetic operation gate lifetime");
}
static void ended(ConnectHarness& h){
    if(h.client_)h.client_->stop();host_run_all_tasks();
    conv::paid_voice_test.request_stop(PaidVoiceTest::Reason::Error,PaidVoiceTest::Fault::Connect);
    conv::paid_voice_test.finish();
}
static void result(ConnectHarness& h,ConnectStage stage,ConnectCode code,bool http,bool ws){
    const auto s=conv::paid_voice_test.snapshot(1000).connection;
    check(s.stage==stage,"exact failing stage");check(s.code==code,"bounded failure code");
    check(s.ticket_http_called==http,"HTTP call prefix");check(s.websocket_start_called==ws,"WS start call prefix");
    char text[256]{};check(write_connect_json(text,sizeof(text),s),"bounded production serializer");
    auto* j=cJSON_Parse(text);check(j!=nullptr,"valid additive JSON");
    check(std::string(cJSON_GetObjectItemCaseSensitive(j,"stage")->valuestring)==connect_stage_name(stage),"serialized exact stage");
    cJSON_Delete(j);const auto before=s;ended(h);
    const auto after=conv::paid_voice_test.snapshot(1000).connection;
    check(before.stage==after.stage&&before.code==after.code,"STOP and finish preserve first diagnostic");++cases;
    httpd_req_t request;
    check(handle_paid_voice_status(&request)==ESP_OK,"actual status handler fits its buffer");
    auto* status=cJSON_Parse(request.body.c_str());check(status!=nullptr,"actual whole status JSON parses");
    const auto* connection=cJSON_GetObjectItemCaseSensitive(status,"connection");
    check(connection!=nullptr&&std::string(cJSON_GetObjectItemCaseSensitive(connection,"stage")->valuestring)==connect_stage_name(after.stage),"actual handler publishes exact preserved diagnostic");
    cJSON_Delete(status);
}
int main(int argc,char** argv){
    if(argc>1){
        ConnectHarness h;begin(h);host_http_cleanup_result=ESP_FAIL;
        const bool request_failed=std::string(argv[1])=="cleanup-after-auth";
        if(request_failed)host_http_status=401;
        check(!h.connect(),"uncertain ticket cleanup ends connect");
        result(h,request_failed?ConnectStage::TicketHttpStatus:ConnectStage::TicketHttpCleanup,
            request_failed?ConnectCode::HttpAuth:ConnectCode::Cleanup,true,false);
        std::cout<<"uncertain cleanup retained owner; first cause preserved; no real socket\n";
        return 0;
    }
    {ConnectHarness h;begin(h);h.preparation_admitted=false;const auto calls=host_http_perform_calls;
     check(!h.connect(),"preparation gate refused");check(calls==host_http_perform_calls,"gate refusal made no HTTP call");result(h,ConnectStage::PreparationLease,ConnectCode::InvalidState,false,false);}
    {ConnectHarness h;begin(h);h.xai_device_token_="synthetic-invalid";check(!h.connect(),"ticket configuration refused");result(h,ConnectStage::TicketConfig,ConnectCode::ProtocolError,false,false);}
    {ConnectHarness h;begin(h);host_http_init_fail=true;check(!h.connect(),"ticket client init refused");result(h,ConnectStage::TicketHttpInit,ConnectCode::TransportInit,false,false);}
    {ConnectHarness h;begin(h);host_http_setup_fail=true;check(!h.connect(),"ticket header setup failed");result(h,ConnectStage::TicketHttpSetup,ConnectCode::TransportInit,false,false);}
    {ConnectHarness h;begin(h);host_http_perform_result=ESP_FAIL;check(!h.connect(),"ticket transport failed");result(h,ConnectStage::TicketHttpRequest,ConnectCode::HttpOtherFailure,true,false);}
    for(auto [status,code]:{std::pair{401,ConnectCode::HttpAuth},{403,ConnectCode::HttpAuth},{302,ConnectCode::HttpRedirect},
            {429,ConnectCode::HttpClient},{503,ConnectCode::HttpServer},{204,ConnectCode::HttpOther}}){
        ConnectHarness h;begin(h);host_http_status=status;check(!h.connect(),"HTTP status failed once");result(h,ConnectStage::TicketHttpStatus,code,true,false);
    }
    {ConnectHarness h;begin(h);host_http_body=std::string(9000,'x');check(!h.connect(),"ticket body bounded");result(h,ConnectStage::TicketHttpRequest,ConnectCode::BodyLimit,true,false);}
    {ConnectHarness h;begin(h);host_http_body="{}";check(!h.connect(),"ticket validation failed");result(h,ConnectStage::TicketPayload,ConnectCode::ProtocolError,true,false);}
    {ConnectHarness h;begin(h);host_http_headers={{"Date","invalid"}};check(!h.connect(),"ticket date failed");result(h,ConnectStage::TicketDate,ConnectCode::ProtocolError,true,false);}
    {ConnectHarness h;begin(h);h.client_admitted=false;check(!h.connect(),"client operation gate refused");result(h,ConnectStage::ClientLease,ConnectCode::InvalidState,true,false);}
    {ConnectHarness h;begin(h);h.client_call_admitted=false;check(!h.connect(),"client call gate refused");result(h,ConnectStage::ClientStart,ConnectCode::InvalidState,true,false);}
    {ConnectHarness h;begin(h);h.mic_sample_rate_=16000;check(!h.connect(),"client media configuration refused");result(h,ConnectStage::ClientConfig,ConnectCode::ProtocolError,true,false);}
    {ConnectHarness h;begin(h);h.paid_deadline_us_=host_clock_us;check(!h.connect(),"expired preparation deadline refused");result(h,ConnectStage::TicketPermit,ConnectCode::InvalidState,false,false);}
    {ConnectHarness h;begin(h);host_fail_heap_alloc_at_least=131072;check(!h.connect(),"client receive allocation failed");result(h,ConnectStage::ClientMemory,ConnectCode::OutOfMemory,true,false);}
    {ConnectHarness h;begin(h);host_ws_init_fail=true;check(!h.connect(),"WS init failed");result(h,ConnectStage::ClientInit,ConnectCode::TransportInit,true,false);}
    {ConnectHarness h;begin(h);host_queue_create_fail=true;check(!h.connect(),"sender queue failed");result(h,ConnectStage::ClientQueue,ConnectCode::OutOfMemory,true,false);}
    {ConnectHarness h;begin(h);host_fail_task_name="rt_audio_tx";check(!h.connect(),"sender task creation failed");result(h,ConnectStage::ClientWorker,ConnectCode::TransportInit,true,false);}
    {ConnectHarness h;begin(h);host_ws_start_fail=true;check(!h.connect(),"WS start failed");result(h,ConnectStage::ClientStart,ConnectCode::TransportInit,true,true);}
    {ConnectHarness h;begin(h);const auto http=host_http_perform_calls;const auto ws=host_ws_start_calls;
     check(h.connect(),"normal real client starts against host boundary");check(host_http_perform_calls==http+1&&host_ws_start_calls==ws+1,"one ticket and one WS start");
     result(h,ConnectStage::Started,ConnectCode::None,true,true);}
    {PaidVoiceTest paid;paid.set_available(true,500);check(paid.request_start(true,0,500),"new local policy grant");check(paid.claim(0),"policy claim");auto id=paid.snapshot(0).approval_id;
     paid.note_connection(id,ConnectStage::TicketConfig,ConnectCode::ProtocolError);
     std::thread writer([&]{for(int i=0;i<2000;++i)paid.note_connection(id,ConnectStage::Started);});
     std::thread reader([&]{for(int i=0;i<2000;++i){const auto s=paid.snapshot(1).connection;assert(s.stage==ConnectStage::TicketConfig&&s.code==ConnectCode::ProtocolError);}});
     writer.join();reader.join();paid.request_stop();paid.finish();
     paid.note_connection(id,ConnectStage::ClientStart,ConnectCode::TransportInit,ConnectAttempt::WebsocketStart);
     check(paid.snapshot(2).connection.stage==ConnectStage::TicketConfig,"late old callback cannot overwrite first failure");
     auto next=paid.snapshot(2).approval_id;check(paid.request_start(true,3,next),"fresh grant resets diagnostic");check(paid.claim(3),"new policy claim");
     paid.note_connection(id,ConnectStage::TicketHttpRequest,ConnectCode::None,ConnectAttempt::TicketHttp);
     check(paid.snapshot(4).connection.stage==ConnectStage::None&&!paid.snapshot(4).connection.ticket_http_called,"old generation ignored after new grant");++cases;}
    {ConnectDiagnostics d;d.record(static_cast<ConnectStage>(255),static_cast<ConnectCode>(255),static_cast<ConnectAttempt>(255));
     char text[256]{};check(write_connect_json(text,sizeof(text),d.snapshot()),"invalid enums serialize safely");
     check(std::string(text).find("unknown")!=std::string::npos,"unknown enum bounded");
     char short_buffer[4]{};check(!write_connect_json(short_buffer,sizeof(short_buffer),d.snapshot()),"short serialization rejected");++cases;}
    {ConnectHarness h;begin(h);conv::paid_voice_test.request_stop();conv::paid_voice_test.finish();
     const auto calls=host_http_perform_calls;check(!h.connect(),"revoked coordinator cannot start");
     check(calls==host_http_perform_calls,"revoked permission makes no ticket request");
     check(conv::paid_voice_test.snapshot(1000).connection.stage==ConnectStage::None,"late refused callback does not rewrite finished scope");++cases;}
    {ConnectDiagnostics d;TicketPreparationPermit permit;permit.deadline_us=60000000;permit.generation=99999;
     permit.allowed=[]{return false;};permit.operation_lease=std::make_shared<int>(1);
     permit.connect_observer=[&](auto stage,auto code,auto attempt){d.record(stage,code,attempt);};
     const auto calls=host_http_perform_calls;
     check(!fetch_agent_ticket(kSavedAgentTokenUrl,std::string(43,'d'),permit),"direct denied ticket permit");
     check(calls==host_http_perform_calls&&!d.snapshot().ticket_http_called,"direct denial makes no HTTP call");
     check(d.snapshot().stage==ConnectStage::TicketPermit&&d.snapshot().code==ConnectCode::InvalidState,"direct ticket permit classification");++cases;}
    for(auto [error,code]:{std::pair{ESP_ERR_HTTP_CONNECT,ConnectCode::HttpConnect},
            {ESP_ERR_HTTP_WRITE_DATA,ConnectCode::HttpWrite},{ESP_ERR_HTTP_FETCH_HEADER,ConnectCode::HttpHeaders},
            {ESP_ERR_HTTP_READ_TIMEOUT,ConnectCode::HttpReadTimeout},{ESP_ERR_HTTP_CONNECTION_CLOSED,ConnectCode::HttpClosed},
            {ESP_ERR_HTTP_INCOMPLETE_DATA,ConnectCode::HttpIncomplete},{ESP_ERR_HTTP_INVALID_TRANSPORT,ConnectCode::HttpInvalidTransport},
            {ESP_ERR_HTTP_EAGAIN,ConnectCode::HttpPending},{ESP_ERR_INVALID_STATE,ConnectCode::InvalidState},
            {ESP_ERR_INVALID_ARG,ConnectCode::InvalidArgument},{ESP_ERR_NO_MEM,ConnectCode::OutOfMemory}}){
        ConnectHarness h;begin(h);host_http_perform_result=error;check(!h.connect(),"specific HTTP failure classified");
        check(host_http_perform_calls>0&&host_last_tls_reads==(error==ESP_ERR_HTTP_CONNECT?1:0),"TLS cache read only once on assigned connect transport");
        result(h,ConnectStage::TicketHttpRequest,code,true,false);
    }
    {ConnectHarness h;begin(h);host_http_perform_result=ESP_ERR_HTTP_CONNECT;host_last_tls_result=ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME;
     check(!h.connect(),"DNS error survives ticket failure");result(h,ConnectStage::TicketHttpRequest,ConnectCode::DnsFailure,true,false);}
    {ConnectHarness h;begin(h);host_http_perform_result=ESP_ERR_HTTP_CONNECT;host_last_tls_flags=64;host_last_tls_native=-12345;
     check(!h.connect(),"certificate verification flags classified");result(h,ConnectStage::TicketHttpRequest,ConnectCode::TlsVerify,true,false);}
    {ConnectHarness h;begin(h);host_http_perform_result=ESP_ERR_HTTP_CONNECT;host_last_tls_native=-12345;
     check(!h.connect(),"TLS library error classified without publishing number");result(h,ConnectStage::TicketHttpRequest,ConnectCode::TlsLibrary,true,false);}
    {tls_scope_snapshot s{};
     for(auto [fault,code]:{std::pair{TLS_SCOPE_FAULT_CLOCK,ConnectCode::ScopeClock},{TLS_SCOPE_FAULT_INVARIANT,ConnectCode::ScopeInvariant},{TLS_SCOPE_FAULT_CAPACITY,ConnectCode::ScopeCapacity}}){
        s.fault=fault;check(classify_ticket_http_failure(ESP_ERR_HTTP_CONNECT,nullptr,s)==code,"finite observed scope fault classification");
     }
     s.fault=TLS_SCOPE_FAULT_NONE;s.deadline_exceeded=true;
     check(classify_ticket_http_failure(ESP_ERR_HTTP_CONNECT,nullptr,s)==ConnectCode::ScopeDeadline,"scope deadline retained");
     s.deadline_exceeded=false;check(classify_ticket_http_failure(ESP_ERR_HTTP_CONNECT,nullptr,s)==ConnectCode::HttpConnect,"null client cannot read transport error cache");++cases;}
    {ConnectHarness h;begin(h);check(h.connect(),"success unchanged by diagnostics");check(host_last_tls_reads==0,"no error cache read on success");
     check(host_http.config.transport_type==HTTP_TRANSPORT_OVER_SSL,"ticket explicitly chooses SSL before SDK URL initialization");
     result(h,ConnectStage::Started,ConnectCode::None,true,true);}
    std::cout<<"connect diagnostic cases="<<cases<<" checks="<<checks<<" no real socket or model API\n";
}
