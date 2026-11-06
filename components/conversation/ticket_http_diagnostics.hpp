// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "conversation/connect_diagnostics.hpp"
#include <esp_http_client.h>
#include <esp_tls_errors.h>
#include "esp_tls_scope.h"

namespace stackchan::conversation {
// This consumes only the SDK error cache, never a request or a retry. Native
// error integers and certificate flags stay on the stack and are not published.
inline ConnectCode classify_ticket_http_failure(esp_err_t result,
        esp_http_client_handle_t client,const tls_scope_snapshot& scope) {
    if(scope.fault==TLS_SCOPE_FAULT_CLOCK)return ConnectCode::ScopeClock;
    if(scope.fault==TLS_SCOPE_FAULT_INVARIANT)return ConnectCode::ScopeInvariant;
    if(scope.fault==TLS_SCOPE_FAULT_CAPACITY)return ConnectCode::ScopeCapacity;
    if(scope.deadline_exceeded)return ConnectCode::ScopeDeadline;
    switch(result) {
    case ESP_ERR_NO_MEM:return ConnectCode::OutOfMemory;
    case ESP_ERR_INVALID_ARG:return ConnectCode::InvalidArgument;
    case ESP_ERR_INVALID_STATE:return ConnectCode::InvalidState;
    case ESP_ERR_TIMEOUT:return ConnectCode::Timeout;
    case ESP_ERR_HTTP_WRITE_DATA:return ConnectCode::HttpWrite;
    case ESP_ERR_HTTP_FETCH_HEADER:return ConnectCode::HttpHeaders;
    case ESP_ERR_HTTP_READ_TIMEOUT:return ConnectCode::HttpReadTimeout;
    case ESP_ERR_HTTP_CONNECTION_CLOSED:return ConnectCode::HttpClosed;
    case ESP_ERR_HTTP_INCOMPLETE_DATA:return ConnectCode::HttpIncomplete;
    case ESP_ERR_HTTP_INVALID_TRANSPORT:return ConnectCode::HttpInvalidTransport;
    case ESP_ERR_HTTP_EAGAIN:case ESP_ERR_HTTP_CONNECTING:return ConnectCode::HttpPending;
    case ESP_ERR_HTTP_CONNECT: {
        // The actual HTTP connect path assigned a nonnull transport before
        // returning this code. Do not read a transport cache for early errors.
        if(!client)return ConnectCode::HttpConnect;
        int native=0,flags=0;
        const auto tls_error=esp_http_client_get_and_clear_last_tls_error(client,&native,&flags);
        if(flags!=0)return ConnectCode::TlsVerify;
        if(tls_error==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME)return ConnectCode::DnsFailure;
        if(native!=0)return ConnectCode::TlsLibrary;
        return ConnectCode::HttpConnect;
    }
    default:return ConnectCode::HttpOtherFailure;
    }
}
}
