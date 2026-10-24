// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "host_transport.hpp"
enum {HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_HEADER, HTTP_METHOD_POST};
struct esp_http_client_event_t {int event_id{}, data_len{}; void* data{}; void* user_data{}; char* header_key{}; char* header_value{};};
struct esp_http_client_config_t {
    const char* url{}; int method{}, timeout_ms{}, max_authorization_retries{};
    bool disable_auto_redirect{};
    int (*crt_bundle_attach)(void*){};
    int (*event_handler)(esp_http_client_event_t*){};
    void* user_data{};
};
struct HostHttp {esp_http_client_config_t config;};
inline HostHttp host_http;
inline std::string host_http_body, host_http_url, host_http_authorization;
inline std::vector<std::pair<std::string, std::string>> host_http_headers;
inline int host_http_status = 200, host_http_perform_calls = 0, host_http_cleanups = 0;
using esp_http_client_handle_t = HostHttp*;
inline auto esp_http_client_init(const esp_http_client_config_t* cfg) {
    host_http.config = *cfg; host_http_url = cfg->url; return &host_http;
}
inline int esp_http_client_set_header(HostHttp*, const char* key, const char* value) {
    if (std::string(key) == "Authorization") host_http_authorization = value;
    return ESP_OK;
}
inline int esp_http_client_set_post_field(HostHttp*, const char*, int length) { return length == 0 ? ESP_OK : ESP_FAIL; }
inline int esp_http_client_perform(HostHttp* h) {
    ++host_http_perform_calls;
    for (auto& [key, value] : host_http_headers) {
        esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, 0, nullptr, h->config.user_data, key.data(), value.data()};
        if (h->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    }
    esp_http_client_event_t event{HTTP_EVENT_ON_DATA, static_cast<int>(host_http_body.size()), host_http_body.data(), h->config.user_data};
    return h->config.event_handler(&event);
}
inline int esp_http_client_get_status_code(HostHttp*) {return host_http_status;}
inline int esp_http_client_cleanup(HostHttp*) {++host_http_cleanups; return ESP_OK;}
