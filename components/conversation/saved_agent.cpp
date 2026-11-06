// SPDX-License-Identifier: BSL-1.0
#include "conversation/saved_agent.hpp"
#include "saved_agent_protocol.hpp"
#include "ticket_http_diagnostics.hpp"
#include <chrono>
#include <ctime>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include "esp_tls_scope.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <new>
#include <algorithm>

namespace stackchan::conversation {
namespace {
constexpr const char* kTag = "saved-agent";
struct Body { std::string text, date; bool overflow = false, date_seen = false, invalid_date = false; };
struct TicketOperation {
    Body body;TicketPreparationPermit permit;esp_tls_scope scope{};
    esp_http_client_handle_t client{};std::int64_t request_end{};
    TicketOperation*retained_next{};
    bool allowed()const {const auto now=esp_timer_get_time();return now>=0&&now<request_end&&permit.allowed&&permit.allowed();}
};
static std::atomic<bool> ticket_operation_busy{false};
static std::atomic<std::uint32_t> last_ticket_generation{0};
static std::atomic<TicketOperation*> retained_operations{nullptr};
static void retain(TicketOperation*owner){
    auto*previous=retained_operations.load(std::memory_order_acquire);
    do{owner->retained_next=previous;}while(!retained_operations.compare_exchange_weak(previous,owner,std::memory_order_acq_rel,std::memory_order_acquire));
    // Busy stays true. Unknown owner/lease never admits a replacement attempt.
}
esp_err_t receive_body(esp_http_client_event_t* event) {
    auto& owner=*static_cast<TicketOperation*>(event->user_data);
    if(!owner.allowed())return ESP_FAIL;
    auto& body=owner.body;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key) {
        const std::string_view key(event->header_key);
        if (key.size() == 4 && (key[0] == 'D' || key[0] == 'd') &&
            (key[1] == 'A' || key[1] == 'a') && (key[2] == 'T' || key[2] == 't') && (key[3] == 'E' || key[3] == 'e')) {
            if (body.date_seen || !event->header_value || std::strlen(event->header_value) != 29) body.invalid_date = true;
            else body.date = event->header_value;
            body.date_seen = true;
        }
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len < 0 || body.text.size() + event->data_len > 8192) {
        body.overflow = true;
        return ESP_FAIL;
    }
    if (event->data_len > 0) body.text.append(static_cast<const char*>(event->data), event->data_len);
    return ESP_OK;
}
}
tl::expected<AgentTicket, ConversationError> fetch_agent_ticket(
        const std::string& url, const std::string& device_token, const TicketPreparationPermit& permit) {
    const auto fail=[&](ConnectStage stage,ConnectCode code,ConversationError error) {
        observe_connect(permit.connect_observer,stage,code);return tl::unexpected{error};
    };
    observe_connect(permit.connect_observer,ConnectStage::TicketPermit);
    const auto scope_start=esp_timer_get_time();
    if(!permit.generation || !permit.allowed || !permit.operation_lease || !permit.allowed() || scope_start<0 || permit.deadline_us<=scope_start)
        return fail(ConnectStage::TicketPermit,ConnectCode::InvalidState,ConversationError::InvalidState);
    if (url != kSavedAgentTokenUrl || !base64url_id(device_token, 43, 128)) {
        ESP_LOGW(kTag, "configuration rejected (url_match=%d, device_credential_valid=%d)",
                 url == kSavedAgentTokenUrl, base64url_id(device_token, 43, 128));
        return fail(ConnectStage::TicketConfig,ConnectCode::ProtocolError,ConversationError::ProtocolError);
    }
    const auto now = std::time(nullptr);
    if (now < 1735689600) {
        ESP_LOGW(kTag, "waiting for a valid system clock before certificate verification");
        return fail(ConnectStage::TicketClock,ConnectCode::Timeout,ConversationError::Timeout);
    }
    bool expected=false;
    if(!ticket_operation_busy.compare_exchange_strong(expected,true,std::memory_order_acq_rel))return fail(ConnectStage::TicketAdmission,ConnectCode::Busy,ConversationError::InvalidState);
    if(permit.generation==last_ticket_generation.load(std::memory_order_acquire)) {
        ticket_operation_busy.store(false,std::memory_order_release);return fail(ConnectStage::TicketAdmission,ConnectCode::Busy,ConversationError::InvalidState);
    }
    last_ticket_generation.store(permit.generation,std::memory_order_release);
    std::unique_ptr<TicketOperation> owner{new(std::nothrow) TicketOperation{}};
    if(!owner){ticket_operation_busy.store(false,std::memory_order_release);return fail(ConnectStage::TicketMemory,ConnectCode::OutOfMemory,ConversationError::OutOfMemory);}
    owner->permit=permit;
    const auto request_budget=std::min<std::int64_t>(15000000,permit.deadline_us-scope_start);
    owner->request_end=scope_start+request_budget;
    auto& body=owner->body;
    if(!tls_scope_init(&owner->scope,1,scope_start,owner->request_end) ||
       !tls_scope_set_permission(&owner->scope,1,+[](void*context){return static_cast<TicketOperation*>(context)->allowed();},owner.get())) {
        ticket_operation_busy.store(false,std::memory_order_release);return fail(ConnectStage::TicketScope,ConnectCode::InvalidState,ConversationError::InvalidState);
    }
    esp_http_client_config_t config{};
    config.url = url.c_str();
    // Keep initial transport defaults aligned with the fixed HTTPS endpoint.
    // A later 80-to-443 URL change would close and cancel the bounded scope.
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = static_cast<int>(std::max<std::int64_t>(1,request_budget/1000));
    config.bounded_scope=&owner->scope;config.bounded_generation=1;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = receive_body;
    config.user_data = owner.get();
    auto client = esp_http_client_init(&config);owner->client=client;
    if(!client){
        int fd=-1;const auto claim=tls_scope_close_claim(&owner->scope,1,&fd);
        if((claim!=TLS_SCOPE_ALREADY && (claim!=TLS_SCOPE_OK || tls_scope_close_finish(&owner->scope,1,fd,fd<0?0:-1)!=TLS_SCOPE_OK)) || !tls_scope_release(&owner->scope,1)) {
            retain(owner.release());return fail(ConnectStage::TicketHttpCleanup,ConnectCode::Cleanup,ConversationError::TransportInit);
        }
        ticket_operation_busy.store(false,std::memory_order_release);return fail(ConnectStage::TicketHttpInit,ConnectCode::TransportInit,ConversationError::TransportInit);
    }
    const std::string authorization = "Bearer " + device_token;
    auto result = esp_http_client_set_header(client, "Authorization", authorization.c_str());
    if (result == ESP_OK) result = esp_http_client_set_post_field(client, "", 0);
    const auto started = std::chrono::steady_clock::now();
    if(result!=ESP_OK)observe_connect(permit.connect_observer,ConnectStage::TicketHttpSetup,ConnectCode::TransportInit);
    if(result==ESP_OK && owner->allowed()) {
        observe_connect(permit.connect_observer,ConnectStage::TicketHttpRequest,ConnectCode::None,ConnectAttempt::TicketHttp);
        result=esp_http_client_perform(client);
    } else {
        observe_connect(permit.connect_observer,ConnectStage::TicketPermit,ConnectCode::InvalidState);
        result=ESP_FAIL;
    }
    const int status = esp_http_client_get_status_code(client);
    // Read only finite categories before cleanup clears errors or cancels scope.
    const auto request_failure=result==ESP_OK?ConnectCode::None:
        classify_ticket_http_failure(result,client,tls_scope_observe(&owner->scope,1,esp_timer_get_time()));
    // Capture the first request failure before cleanup can report another failure.
    if(body.overflow)observe_connect(permit.connect_observer,ConnectStage::TicketHttpRequest,ConnectCode::BodyLimit);
    else if(result!=ESP_OK)observe_connect(permit.connect_observer,ConnectStage::TicketHttpRequest,request_failure);
    else if(status!=200)observe_connect(permit.connect_observer,ConnectStage::TicketHttpStatus,connect_http_code(status));
    observe_connect(permit.connect_observer,ConnectStage::TicketHttpCleanup);
    tls_scope_cancel(&owner->scope,1);
    for(;;){
        const auto closed=esp_http_client_cleanup(client);
        if(closed==ESP_OK){owner->client=nullptr;break;}
        if(closed!=-2 || esp_timer_get_time()>=permit.deadline_us){
            retain(owner.release());return fail(ConnectStage::TicketHttpCleanup,ConnectCode::Cleanup,ConversationError::TransportInit);
        }
        vTaskDelay(1); // BUSY continuation only; never retry a real close failure.
    }
    if(!tls_scope_observe(&owner->scope,1,esp_timer_get_time()).close_confirmed || !tls_scope_release(&owner->scope,1)) {
        retain(owner.release());return fail(ConnectStage::TicketHttpCleanup,ConnectCode::Cleanup,ConversationError::TransportInit);
    }
    ticket_operation_busy.store(false,std::memory_order_release);
    if(!owner->allowed())return fail(ConnectStage::TicketPermit,ConnectCode::Timeout,ConversationError::Timeout);
    if (result != ESP_OK || body.overflow) return fail(ConnectStage::TicketHttpRequest,body.overflow?ConnectCode::BodyLimit:request_failure,ConversationError::TransportInit);
    if (status != 200) return fail(ConnectStage::TicketHttpStatus,connect_http_code(status),ConversationError::ServerError);
    const auto local_now = std::time(nullptr);
    double reference_now = local_now;
    if (body.date_seen) {
        const auto server_date = saved_agent_http_date(body.date);
        if (body.invalid_date || !server_date) {
            ESP_LOGW(kTag, "invalid or duplicate broker Date header");
            return fail(ConnectStage::TicketDate,ConnectCode::ProtocolError,ConversationError::ProtocolError);
        }
        // TLS and HTTP 200 have succeeded against the fixed broker. Its Date
        // authenticates the clock reference without weakening ticket lifetime
        // limits or modifying the device's system/RTC clock. Conservatively
        // include the whole request duration when calculating remaining life.
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        reference_now = static_cast<double>(*server_date) + std::ceil(elapsed);
        const auto skew = static_cast<long long>(local_now) - *server_date;
        if (skew < -15 || skew > 15) ESP_LOGW(kTag, "system clock differs from verified broker by %lld seconds", skew);
    }
    auto ticket = parse_agent_ticket(body.text.data(), body.text.size(), reference_now);
    if (!ticket)observe_connect(permit.connect_observer,ConnectStage::TicketPayload,connect_code(ticket.error()));
    if (!ticket) ESP_LOGW(kTag, "ticket rejected (bytes=%u, broker_date=%d, reference_epoch=%lld)",
                          static_cast<unsigned>(body.text.size()), body.date_seen, static_cast<long long>(reference_now));
    if (ticket && ticket->token == device_token) return fail(ConnectStage::TicketPayload,ConnectCode::ProtocolError,ConversationError::ProtocolError);
    if(!owner->allowed())return fail(ConnectStage::TicketPermit,ConnectCode::Timeout,ConversationError::Timeout);
    return ticket;
}
}
