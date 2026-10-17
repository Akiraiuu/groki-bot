// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <cstdint>
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
constexpr int pdTRUE = 1, pdPASS = 1, ESP_OK = 0, eDeleted = 0;
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
inline void* heap_caps_malloc(std::size_t n, unsigned) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
inline std::int64_t esp_timer_get_time() { return 1000000; }
inline int esp_crt_bundle_attach(void*) { return 0; }
struct HostQueue { unsigned capacity; std::deque<void*> items; };
using QueueHandle_t = HostQueue*;
inline QueueHandle_t xQueueCreate(unsigned n, unsigned) { return new HostQueue{n, {}}; }
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
inline int xTaskCreatePinnedToCoreWithCaps(void(*)(void*), const char*, unsigned,
                                         void*, unsigned, TaskHandle_t* h, int, unsigned) {
    *h = reinterpret_cast<void*>(1); return pdPASS;
}
inline int eTaskGetState(TaskHandle_t) { return eDeleted; }
inline void vTaskDelay(unsigned) {}
inline void vTaskDeleteWithCaps(void*) {}

struct esp_websocket_client_config_t {
    const char* uri{};
    int (*crt_bundle_attach)(void*){};
    int buffer_size{}, task_stack{}, task_prio{};
    bool disable_auto_reconnect{};
    int network_timeout_ms{}, ping_interval_sec{}, pingpong_timeout_sec{};
    bool keep_alive_enable{};
    int keep_alive_idle{}, keep_alive_interval{}, keep_alive_count{};
};
enum { WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DATA,
       WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_CLOSED };
struct esp_websocket_event_data_t {
    int data_len{}, payload_offset{}, op_code{}, payload_len{};
    char* data_ptr{};
};
struct HostSocket {
    std::string uri;
    bool connected{};
    void (*callback)(void*, esp_event_base_t, std::int32_t, void*){};
    void* arg{};
    std::map<std::string, std::string> headers;
    std::vector<std::string> sent;
};
using esp_websocket_client_handle_t = HostSocket*;
inline HostSocket* host_socket{};
inline auto esp_websocket_client_init(const esp_websocket_client_config_t* cfg) {
    host_socket = new HostSocket; host_socket->uri = cfg->uri; return host_socket;
}
inline int esp_websocket_client_append_header(HostSocket* s, const char* k, const char* v) {
    s->headers[k] = v; return ESP_OK;
}
inline int esp_websocket_register_events(HostSocket* s, int,
            void(*callback)(void*, esp_event_base_t, std::int32_t, void*), void* arg) {
    s->callback = callback; s->arg = arg; return ESP_OK;
}
inline int esp_websocket_client_start(HostSocket* s) { s->connected = true; return ESP_OK; }
inline bool esp_websocket_client_is_connected(HostSocket* s) { return s->connected; }
inline int esp_websocket_client_send_text(HostSocket* s, const char* p, int n, unsigned) {
    s->sent.emplace_back(p, n); return n;
}
inline int esp_websocket_client_stop(HostSocket* s) { s->connected = false; return ESP_OK; }
inline int esp_websocket_client_destroy(HostSocket* s) { delete s; host_socket = nullptr; return ESP_OK; }
inline void host_connected() { host_socket->callback(host_socket->arg, nullptr, WEBSOCKET_EVENT_CONNECTED, nullptr); }
inline void host_receive(std::string json) {
    esp_websocket_event_data_t data;
    data.data_len = data.payload_len = json.size(); data.op_code = 1; data.data_ptr = json.data();
    host_socket->callback(host_socket->arg, nullptr, WEBSOCKET_EVENT_DATA, &data);
}
