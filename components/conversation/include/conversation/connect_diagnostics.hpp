// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <functional>
#include "conversation_error.hpp"

namespace stackchan::conversation {
// No raw network errors, identifiers, URLs, headers, ticket or audio content.
enum class ConnectStage : std::uint8_t {
    None, Permission, PreparationLease, TicketPermit, TicketConfig, TicketClock,
    TicketAdmission, TicketMemory, TicketScope, TicketHttpInit, TicketHttpSetup,
    TicketHttpRequest, TicketHttpCleanup, TicketHttpStatus, TicketDate, TicketPayload,
    TicketResult, ClientLease, ClientConfig, ClientScope, ClientMemory, ClientInit,
    ClientQueue, ClientWorker, ClientStart, Started, Unknown
};
enum class ConnectCode : std::uint8_t {
    None, NotConnected, TransportInit, SendFailed, ProtocolError, ServerError,
    Timeout, InvalidState, OutOfMemory, Busy, BodyLimit, Cleanup,
    HttpAuth, HttpRedirect, HttpClient, HttpServer, HttpOther,
    HttpConnect, HttpWrite, HttpHeaders, HttpReadTimeout, HttpClosed, HttpIncomplete,
    HttpInvalidTransport, HttpPending, HttpOtherFailure, InvalidArgument,
    DnsFailure, TlsVerify, TlsLibrary, ScopeClock, ScopeInvariant, ScopeCapacity, ScopeDeadline, Unknown
};
enum class ConnectAttempt : std::uint8_t { None, TicketHttp, WebsocketStart };
inline const char* connect_stage_name(ConnectStage s) {
    constexpr const char* names[]={"none","permission","preparation_lease","ticket_permit",
        "ticket_config","ticket_clock","ticket_admission","ticket_memory","ticket_scope",
        "ticket_http_init","ticket_http_setup","ticket_http_request","ticket_http_cleanup",
        "ticket_http_status","ticket_date","ticket_payload","ticket_result","client_lease",
        "client_config","client_scope","client_memory","client_init","client_queue",
        "client_worker","client_start","started","unknown"};
    const auto n=static_cast<unsigned>(s);
    return n<sizeof(names)/sizeof(*names)?names[n]:"unknown";
}
inline const char* connect_code_name(ConnectCode c) {
    constexpr const char* names[]={"none","not_connected","transport_init","send_failed",
        "protocol_error","server_error","timeout","invalid_state","out_of_memory","busy",
        "body_limit","cleanup","http_auth","http_redirect","http_client","http_server",
        "http_other","http_connect","http_write","http_headers","http_read_timeout",
        "http_closed","http_incomplete","http_invalid_transport","http_pending","http_other_failure",
        "invalid_argument","dns_failure","tls_verify","tls_library","scope_clock","scope_invariant",
        "scope_capacity","scope_deadline","unknown"};
    const auto n=static_cast<unsigned>(c);
    return n<sizeof(names)/sizeof(*names)?names[n]:"unknown";
}
inline ConnectCode connect_code(ConversationError e) {
    switch(e) {
    case ConversationError::NotConnected:return ConnectCode::NotConnected;
    case ConversationError::TransportInit:return ConnectCode::TransportInit;
    case ConversationError::SendFailed:return ConnectCode::SendFailed;
    case ConversationError::ProtocolError:return ConnectCode::ProtocolError;
    case ConversationError::ServerError:return ConnectCode::ServerError;
    case ConversationError::Timeout:return ConnectCode::Timeout;
    case ConversationError::InvalidState:return ConnectCode::InvalidState;
    case ConversationError::OutOfMemory:return ConnectCode::OutOfMemory;
    }
    return ConnectCode::Unknown;
}
inline ConnectCode connect_http_code(int status) {
    if(status==401 || status==403)return ConnectCode::HttpAuth;
    if(status>=300 && status<400)return ConnectCode::HttpRedirect;
    if(status>=400 && status<500)return ConnectCode::HttpClient;
    if(status>=500 && status<600)return ConnectCode::HttpServer;
    return ConnectCode::HttpOther;
}
using ConnectObserver=std::function<void(ConnectStage,ConnectCode,ConnectAttempt)>;
inline void observe_connect(const ConnectObserver& observer, ConnectStage stage,
        ConnectCode code=ConnectCode::None,ConnectAttempt attempt=ConnectAttempt::None) {
    if(observer)observer(stage,code,attempt);
}
struct ConnectSnapshot {
    ConnectStage stage{ConnectStage::None};
    ConnectCode code{ConnectCode::None};
    // A call was entered, not proof that bytes left the device or were billed.
    bool ticket_http_called{},websocket_start_called{};
};
class ConnectDiagnostics {
public:
    // Caller owns synchronization. No allocation, I/O, permission or lifecycle action.
    void record(ConnectStage stage,ConnectCode code,ConnectAttempt attempt) {
        if(state_.code!=ConnectCode::None)return; // Preserve the first failure and prefix.
        state_.stage=static_cast<unsigned>(stage)<=static_cast<unsigned>(ConnectStage::Unknown)?stage:ConnectStage::Unknown;
        state_.code=static_cast<unsigned>(code)<=static_cast<unsigned>(ConnectCode::Unknown)?code:ConnectCode::Unknown;
        state_.ticket_http_called|=attempt==ConnectAttempt::TicketHttp;
        state_.websocket_start_called|=attempt==ConnectAttempt::WebsocketStart;
    }
    ConnectSnapshot snapshot()const{return state_;}
private:
    ConnectSnapshot state_{};
};
inline bool write_connect_json(char* buffer,std::size_t size,const ConnectSnapshot& s) {
    const int n=std::snprintf(buffer,size,
        "{\"stage\":\"%s\",\"code\":\"%s\",\"ticket_http_called\":%s,\"websocket_start_called\":%s}",
        connect_stage_name(s.stage),connect_code_name(s.code),
        s.ticket_http_called?"true":"false",s.websocket_start_called?"true":"false");
    return n>=0 && static_cast<std::size_t>(n)<size;
}
}
