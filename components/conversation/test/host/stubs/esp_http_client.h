// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "host_transport.hpp"
#define ESP_ERR_HTTP_BASE               (0x7000)                    /*!< Starting number of HTTP error codes */
#define ESP_ERR_HTTP_MAX_REDIRECT       (ESP_ERR_HTTP_BASE + 1)     /*!< The error exceeds the number of HTTP redirects */
#define ESP_ERR_HTTP_CONNECT            (ESP_ERR_HTTP_BASE + 2)     /*!< Error open the HTTP connection */
#define ESP_ERR_HTTP_WRITE_DATA         (ESP_ERR_HTTP_BASE + 3)     /*!< Error write HTTP data */
#define ESP_ERR_HTTP_FETCH_HEADER       (ESP_ERR_HTTP_BASE + 4)     /*!< Error read HTTP header from server */
#define ESP_ERR_HTTP_INVALID_TRANSPORT  (ESP_ERR_HTTP_BASE + 5)     /*!< There are no transport support for the input scheme */
#define ESP_ERR_HTTP_CONNECTING         (ESP_ERR_HTTP_BASE + 6)     /*!< HTTP connection hasn't been established yet */
#define ESP_ERR_HTTP_EAGAIN             (ESP_ERR_HTTP_BASE + 7)     /*!< Mapping of errno EAGAIN to esp_err_t */
#define ESP_ERR_HTTP_CONNECTION_CLOSED  (ESP_ERR_HTTP_BASE + 8)     /*!< Read FIN from peer and the connection closed */
#define ESP_ERR_HTTP_NOT_MODIFIED       (ESP_ERR_HTTP_BASE + 9)     /*!< HTTP 304 Not Modified, no update available */
#define ESP_ERR_HTTP_RANGE_NOT_SATISFIABLE (ESP_ERR_HTTP_BASE + 10) /*!< HTTP 416 Range Not Satisfiable, requested range in header is incorrect */
#define ESP_ERR_HTTP_READ_TIMEOUT       (ESP_ERR_HTTP_BASE + 11)    /*!< HTTP data read timeout */
#define ESP_ERR_HTTP_INCOMPLETE_DATA    (ESP_ERR_HTTP_BASE + 12)    /*!< Incomplete data received, less than Content-Length or last chunk */
#define ESP_ERR_HTTP_REDIRECT_DOWNGRADE (ESP_ERR_HTTP_BASE + 13)   /*!< HTTPS origin redirected to a non-HTTPS scheme (downgrade blocked) */
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
inline int host_last_tls_result{},host_last_tls_native{},host_last_tls_flags{},host_last_tls_reads{};

enum {HTTP_TRANSPORT_UNKNOWN, HTTP_TRANSPORT_OVER_TCP, HTTP_TRANSPORT_OVER_SSL};
enum {HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_HEADER, HTTP_METHOD_POST};
struct esp_http_client_event_t {int event_id{}, data_len{}; void* data{}; void* user_data{}; char* header_key{}; char* header_value{};};
struct esp_http_client_config_t {
    const char* url{}; int method{}, timeout_ms{}, max_authorization_retries{}, transport_type{};
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

inline int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client,int* code,int*flags){
    if(!client)return ESP_FAIL;
    ++host_last_tls_reads;*code=host_last_tls_native;*flags=host_last_tls_flags;
    const auto result=host_last_tls_result;host_last_tls_result=host_last_tls_native=host_last_tls_flags=0;
    return result;
}
