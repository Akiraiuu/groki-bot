// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "host_transport.hpp"
enum {HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_HEADER, HTTP_METHOD_POST};
struct esp_http_client_event_t {int event_id{}, data_len{}; void* data{}; void* user_data{}; char* header_key{}; char* header_value{};};
struct esp_http_client_config_t {
    const char* url{}; int method{}, timeout_ms{}, max_authorization_retries{};
    bool disable_auto_redirect{};esp_tls_scope*bounded_scope{};std::uint32_t bounded_generation{};
    int (*crt_bundle_attach)(void*){};
    int (*event_handler)(esp_http_client_event_t*){};
    void* user_data{};
};
struct HostHttp {esp_http_client_config_t config;bool performed{};};
inline HostHttp host_http;
inline std::string host_http_body, host_http_url, host_http_authorization;
inline std::vector<std::pair<std::string, std::string>> host_http_headers;
inline int host_http_status = 200, host_http_perform_calls = 0, host_http_cleanups = 0,host_http_cleanup_result=ESP_OK;
inline void(*host_http_perform_hook)(){};
inline bool host_http_init_fail{},host_http_setup_fail{};
inline int host_http_perform_result=ESP_OK;
using esp_http_client_handle_t = HostHttp*;
inline auto esp_http_client_init(const esp_http_client_config_t* cfg) {
    if(host_http_init_fail)return static_cast<HostHttp*>(nullptr);
    if(cfg->bounded_scope && !tls_scope_bind_sdk(cfg->bounded_scope,cfg->bounded_generation,&host_http))return static_cast<HostHttp*>(nullptr);
    host_http.config = *cfg;host_http.performed=false;host_http_url = cfg->url; return &host_http;
}
inline int esp_http_client_set_header(HostHttp*, const char* key, const char* value) {
    if(host_http_setup_fail)return ESP_FAIL;
    if (std::string(key) == "Authorization") host_http_authorization = value;
    return ESP_OK;
}
inline int esp_http_client_set_post_field(HostHttp*, const char*, int length) { return length == 0 ? ESP_OK : ESP_FAIL; }
inline int esp_http_client_perform(HostHttp* h) {
    if(h->performed || !tls_scope_permitted(h->config.bounded_scope,h->config.bounded_generation,host_clock_us))return ESP_FAIL;
    h->performed=true;++host_http_perform_calls;if(host_http_perform_hook)host_http_perform_hook();
    if(host_http_perform_result!=ESP_OK)return host_http_perform_result;
    for (auto& [key, value] : host_http_headers) {
        esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, 0, nullptr, h->config.user_data, key.data(), value.data()};
        if (h->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    }
    esp_http_client_event_t event{HTTP_EVENT_ON_DATA, static_cast<int>(host_http_body.size()), host_http_body.data(), h->config.user_data};
    return h->config.event_handler(&event);
}
inline int esp_http_client_get_status_code(HostHttp*) {return host_http_status;}
inline int esp_http_client_cleanup(HostHttp*h) {
    ++host_http_cleanups;if(host_http_cleanup_result!=ESP_OK)return host_http_cleanup_result;
    int fd=-1;const auto claim=tls_scope_close_claim(h->config.bounded_scope,h->config.bounded_generation,&fd);
    if(claim==TLS_SCOPE_PENDING)return -2;
    if(claim==TLS_SCOPE_ALREADY)return ESP_OK;
    return claim==TLS_SCOPE_OK && tls_scope_close_finish(h->config.bounded_scope,h->config.bounded_generation,fd,0)==TLS_SCOPE_OK?ESP_OK:ESP_FAIL;
}
