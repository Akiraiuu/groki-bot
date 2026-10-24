// SPDX-License-Identifier: BSL-1.0
#include "conversation/saved_agent.hpp"
#include "saved_agent_protocol.hpp"
#include <chrono>
#include <ctime>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

namespace stackchan::conversation {
namespace {
constexpr const char* kTag = "saved-agent";
struct Body { std::string text, date; bool overflow = false, date_seen = false, invalid_date = false; };
esp_err_t receive_body(esp_http_client_event_t* event) {
    auto& body = *static_cast<Body*>(event->user_data);
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
        const std::string& url, const std::string& device_token) {
    if (url != kSavedAgentTokenUrl || !base64url_id(device_token, 43, 128)) {
        ESP_LOGW(kTag, "configuration rejected (url_match=%d, device_credential_valid=%d)",
                 url == kSavedAgentTokenUrl, base64url_id(device_token, 43, 128));
        return tl::unexpected{ConversationError::ProtocolError};
    }
    const auto now = std::time(nullptr);
    if (now < 1735689600) {
        ESP_LOGW(kTag, "waiting for a valid system clock before certificate verification");
        return tl::unexpected{ConversationError::Timeout};
    }
    Body body;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = receive_body;
    config.user_data = &body;
    auto client = esp_http_client_init(&config);
    if (!client) return tl::unexpected{ConversationError::TransportInit};
    const std::string authorization = "Bearer " + device_token;
    auto result = esp_http_client_set_header(client, "Authorization", authorization.c_str());
    if (result == ESP_OK) result = esp_http_client_set_post_field(client, "", 0);
    const auto started = std::chrono::steady_clock::now();
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client); // release HTTPS/TLS before the voice WebSocket
    if (result != ESP_OK || body.overflow) return tl::unexpected{ConversationError::TransportInit};
    if (status != 200) return tl::unexpected{ConversationError::ServerError};
    const auto local_now = std::time(nullptr);
    double reference_now = local_now;
    if (body.date_seen) {
        const auto server_date = saved_agent_http_date(body.date);
        if (body.invalid_date || !server_date) {
            ESP_LOGW(kTag, "invalid or duplicate broker Date header");
            return tl::unexpected{ConversationError::ProtocolError};
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
    if (!ticket) ESP_LOGW(kTag, "ticket rejected (bytes=%u, broker_date=%d, reference_epoch=%lld)",
                          static_cast<unsigned>(body.text.size()), body.date_seen, static_cast<long long>(reference_now));
    if (ticket && ticket->token == device_token) return tl::unexpected{ConversationError::ProtocolError};
    return ticket;
}
}
