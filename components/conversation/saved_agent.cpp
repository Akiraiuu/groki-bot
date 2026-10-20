// SPDX-License-Identifier: BSL-1.0
#include "conversation/saved_agent.hpp"
#include "saved_agent_protocol.hpp"
#include <ctime>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

namespace stackchan::conversation {
namespace {
struct Body { std::string text; bool overflow = false; };
esp_err_t receive_body(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    auto& body = *static_cast<Body*>(event->user_data);
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
        return tl::unexpected{ConversationError::ProtocolError};
    }
    const auto now = std::time(nullptr);
    if (now < 1735689600) return tl::unexpected{ConversationError::Timeout}; // wait for SNTP
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
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client); // release HTTPS/TLS before the voice WebSocket
    if (result != ESP_OK || body.overflow) return tl::unexpected{ConversationError::TransportInit};
    if (status != 200) return tl::unexpected{ConversationError::ServerError};
    auto ticket = parse_agent_ticket(body.text.data(), body.text.size(), std::time(nullptr));
    if (ticket && ticket->token == device_token) return tl::unexpected{ConversationError::ProtocolError};
    return ticket;
}
}
