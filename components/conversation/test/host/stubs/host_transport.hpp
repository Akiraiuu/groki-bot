// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
#include "esp_tls_scope.h"
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

using TickType_t = unsigned;
using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskHandle_t = void*;
using esp_event_base_t = const char*;
using esp_err_t = int;
constexpr int ESP_FAIL = -1, ESP_ERR_TIMEOUT=0x107, ESP_WEBSOCKET_CLOSE_PENDING=-2;
constexpr int pdTRUE = 1, pdPASS = 1, ESP_OK = 0, eDeleted = 0;
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
inline std::size_t host_fail_heap_alloc_at_least = static_cast<std::size_t>(-1);
inline void* heap_caps_malloc(std::size_t n, unsigned) {
    return n >= host_fail_heap_alloc_at_least ? nullptr : std::malloc(n);
}
inline void heap_caps_free(void* p) { std::free(p); }
inline std::int64_t host_clock_us=1000000;
inline std::int64_t esp_timer_get_time() { return host_clock_us; }
inline int esp_crt_bundle_attach(void*) { return 0; }
struct HostQueue { unsigned capacity; std::deque<void*> items; };
using QueueHandle_t = HostQueue*;
inline QueueHandle_t host_last_queue{};
inline bool host_queue_create_fail{},host_ws_init_fail{},host_ws_start_fail{};
inline unsigned host_ws_start_calls{};
inline QueueHandle_t xQueueCreate(unsigned n, unsigned) { if(host_queue_create_fail)return nullptr;host_last_queue = new HostQueue{n, {}}; return host_last_queue; }
inline int xQueueSend(QueueHandle_t q, void* p, unsigned) {
    if (q->items.size() >= q->capacity) return 0;
    q->items.push_back(*static_cast<void**>(p)); return 1;
}
inline int xQueueReceive(QueueHandle_t q, void* p, unsigned) {
    if (q->items.empty()) return 0;
    *static_cast<void**>(p) = q->items.front(); q->items.pop_front(); return 1;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->items.size(); }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
struct HostTask { void(*fn)(void*); void* arg; };
inline std::deque<HostTask> host_pending_tasks;
inline std::string host_fail_task_name;
inline void host_run_one_task() {
    if (host_pending_tasks.empty()) return;
    auto task=host_pending_tasks.front();host_pending_tasks.pop_front();task.fn(task.arg);
}
inline void host_run_all_tasks() { while (!host_pending_tasks.empty()) host_run_one_task(); }
inline int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*), const char* name, unsigned,
                                         void* arg, unsigned, TaskHandle_t* h, int, unsigned) {
    if (host_fail_task_name==name) return 0;
    host_pending_tasks.push_back({fn,arg});
    *h = reinterpret_cast<void*>(1); return pdPASS;
}
inline int eTaskGetState(TaskHandle_t) { return eDeleted; }
inline void vTaskDelay(unsigned) {
    // Tests inspect the TX queue without a racing worker. At teardown the
    // exit flag is set before the first delay, so run the worker's real exit
    // path and acknowledgement before its queue/socket may be freed.
    host_run_one_task();
}
inline void vTaskDeleteWithCaps(void*) {}

struct esp_websocket_client_config_t {
    const char* uri{};
    int (*crt_bundle_attach)(void*){};
    int buffer_size{}, task_stack{}, task_prio{};
    bool disable_auto_reconnect{},enable_bounded_stop{},enable_close_reconnect{};
    esp_tls_scope* bounded_scope{};std::uint32_t bounded_generation{};std::int64_t bounded_deadline_us{};
    int network_timeout_ms{}, ping_interval_sec{}, pingpong_timeout_sec{};
    bool keep_alive_enable{};
    int keep_alive_idle{}, keep_alive_interval{}, keep_alive_count{};
};
enum { WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DATA,
       WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_CLOSED, WEBSOCKET_EVENT_FINISH };
struct esp_websocket_event_data_t {
    int data_len{}, payload_offset{}, op_code{}, payload_len{};
    char* data_ptr{};
};
struct HostSocket {
    std::string uri;
    bool connected{},stopped{true};esp_tls_scope* scope{};std::uint32_t generation{};std::int64_t deadline{};
    void (*callback)(void*, esp_event_base_t, std::int32_t, void*){};
    void* arg{};
    std::map<std::string, std::string> headers;
    std::vector<std::string> sent;
};
using esp_websocket_client_handle_t = HostSocket*;
inline HostSocket* host_socket{};
inline auto esp_websocket_client_init(const esp_websocket_client_config_t* cfg) {
    if(host_ws_init_fail)return static_cast<HostSocket*>(nullptr);
    host_socket = new HostSocket; host_socket->uri = cfg->uri;host_socket->scope=cfg->bounded_scope;host_socket->generation=cfg->bounded_generation;host_socket->deadline=cfg->bounded_deadline_us;return host_socket;
}
inline int esp_websocket_client_append_header(HostSocket* s, const char* k, const char* v) {
    s->headers[k] = v; return ESP_OK;
}
inline int esp_websocket_register_events(HostSocket* s, int,
            void(*callback)(void*, esp_event_base_t, std::int32_t, void*), void* arg) {
    s->callback = callback; s->arg = arg; return ESP_OK;
}
inline int esp_websocket_client_start(HostSocket* s) { ++host_ws_start_calls;if(host_ws_start_fail)return ESP_FAIL;s->connected = true;s->stopped=false;return ESP_OK; }
inline bool esp_websocket_client_is_connected(HostSocket* s) { return s->connected; }
inline int esp_websocket_client_send_text(HostSocket* s, const char* p, int n, unsigned) {
    s->sent.emplace_back(p, n); return n;
}
inline int host_stop_result=ESP_OK,host_destroy_result=ESP_OK;
inline unsigned host_stop_calls=0,host_destroy_calls=0,host_delete_queues=0;
inline void(*host_stop_hook)(){};
inline int esp_websocket_client_stop(HostSocket* s) {
    ++host_stop_calls;
    if (host_stop_hook) host_stop_hook();
    if (host_stop_result!=ESP_OK) return host_stop_result;
    s->connected=false;
    if (s->callback) s->callback(s->arg,nullptr,WEBSOCKET_EVENT_FINISH,nullptr);
    return ESP_OK;
}
inline int esp_websocket_client_destroy(HostSocket* s) {
    ++host_destroy_calls;
    if (host_destroy_result!=ESP_OK) return host_destroy_result;
    delete s; host_socket=nullptr; return ESP_OK;
}
inline void host_connected() { host_socket->callback(host_socket->arg, nullptr, WEBSOCKET_EVENT_CONNECTED, nullptr); }
inline void host_receive(std::string json) {
    esp_websocket_event_data_t data;
    data.data_len = data.payload_len = json.size(); data.op_code = 1; data.data_ptr = json.data();
    host_socket->callback(host_socket->arg, nullptr, WEBSOCKET_EVENT_DATA, &data);
}

inline unsigned host_cancel_calls=0,host_cleanup_calls=0;
inline int host_cancel_result=ESP_OK;
inline bool host_task_stopped=true;
inline int host_cleanup_result=ESP_OK;
inline int esp_websocket_client_request_stop(HostSocket*s) {
    ++host_cancel_calls;if(host_cancel_result!=ESP_OK)return host_cancel_result;return tls_scope_cancel(s->scope,s->generation)==TLS_SCOPE_OK?ESP_OK:ESP_FAIL;
}
inline bool esp_websocket_client_stop_observed(HostSocket*s){return s->stopped;}
inline bool esp_websocket_client_transport_close_confirmed(HostSocket*s){return s->stopped&&tls_scope_observe(s->scope,s->generation,host_clock_us).close_confirmed;}
inline int esp_websocket_client_stop_until(HostSocket*s,std::int64_t end){
    ++host_stop_calls;if(host_stop_hook)host_stop_hook();
    if(end!=s->deadline)return ESP_FAIL;
    if(host_stop_result!=ESP_OK)return host_stop_result;
    s->connected=false;s->stopped=host_task_stopped;
    return s->stopped?ESP_OK:ESP_ERR_TIMEOUT;
}
inline int esp_websocket_client_finish_cleanup(HostSocket*s){
    ++host_cleanup_calls;if(host_cleanup_result!=ESP_OK)return host_cleanup_result;
    if(!s->stopped)return ESP_WEBSOCKET_CLOSE_PENDING;
    int fd=-1;const auto claim=tls_scope_close_claim(s->scope,s->generation,&fd);
    if(claim==TLS_SCOPE_ALREADY)return ESP_OK;
    if(claim==TLS_SCOPE_PENDING)return ESP_WEBSOCKET_CLOSE_PENDING;
    return claim==TLS_SCOPE_OK&&tls_scope_close_finish(s->scope,s->generation,fd,0)==TLS_SCOPE_OK?ESP_OK:ESP_FAIL;
}
