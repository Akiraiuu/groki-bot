/* SDK types and RTOS/transport boundaries are synthetic, no network operations. */
#pragma once
#include <stdint.h>
#include "esp_tls_scope.h"
#define ESP_WEBSOCKET_CLOSE_PENDING (-2)
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include <errno.h>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define WS_TRANSPORT_REDIRECT_HEADER_SUPPORT 1
#define WS_HTTP_REDIRECT(code) ((code)>=300 && (code)<400)
#define WEBSOCKET_TX_LOCK_TIMEOUT_MS 30
#define WS_TRANSPORT_OPCODES_FIN 128
#define WS_TRANSPORT_OPCODES_CLOSE 8
#define WS_TRANSPORT_OPCODES_PING 9
#define WS_TRANSPORT_OPCODES_PONG 10
#define WS_TRANSPORT_OPCODES_NONE 0
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 10u
#define pdMS_TO_TICKS(ms) ((ms)/portTICK_PERIOD_MS)
#define ESP_WS_CLIENT_MEM_CHECK(TAG,x,action) do {if(!(x)){action;}}while(0)
#define ESP_WS_CLIENT_STATE_CHECK(TAG,x,action) do {if((x)->state<WEBSOCKET_STATE_INIT){action;}}while(0)
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef int ws_transport_opcodes_t;
typedef int esp_websocket_error_type_t;
typedef void* TaskHandle_t;
static const int STOPPED_BIT=1,CLOSE_FRAME_SENT_BIT=2,REQUESTED_STOP_BIT=4,WAKEUP_BIT=8,CONTROLLED_STOP_BIT=16;
enum {WEBSOCKET_STATE_ERROR=-1,WEBSOCKET_STATE_UNKNOW=0,WEBSOCKET_STATE_INIT,
      WEBSOCKET_STATE_CONNECTED,WEBSOCKET_STATE_WAIT_TIMEOUT,WEBSOCKET_STATE_CLOSING};
enum {WEBSOCKET_EVENT_BEGIN=1,WEBSOCKET_EVENT_BEFORE_CONNECT,WEBSOCKET_EVENT_CONNECTED,
      WEBSOCKET_EVENT_CLOSED,WEBSOCKET_EVENT_FINISH,WEBSOCKET_EVENT_DATA,WEBSOCKET_EVENT_DISCONNECTED};
enum {WEBSOCKET_ERROR_TYPE_NONE,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT,WEBSOCKET_ERROR_TYPE_SERVER_CLOSE};
typedef struct {int last_error,esp_tls_error_code,esp_tls_flags;} FakeError;
typedef FakeError* esp_tls_error_handle_t;
typedef struct {bool busy;unsigned depth;} FakeMutex;
typedef struct {EventBits_t bits;} FakeEvents;
typedef struct {int placeholder;} FakeTransport;
typedef FakeTransport* esp_transport_handle_t;
typedef struct {
    esp_transport_handle_t ext_transport;
    int port,network_timeout_ms,task_stack,task_prio,task_core_id;
    unsigned ping_interval_sec,pingpong_timeout_sec;
    const char *host,*scheme,*task_name;
    char *uri;
    bool auto_reconnect,close_reconnect;
    bool enable_bounded_stop,disable_auto_reconnect,enable_close_reconnect;
    esp_tls_scope* bounded_scope;uint32_t bounded_generation;int64_t bounded_deadline_us;
} FakeConfig;
typedef FakeConfig esp_websocket_client_config_t;
struct esp_websocket_client {
    FakeConfig *config;
    FakeEvents *status_bits;
    FakeMutex *lock,*tx_lock;
    esp_transport_handle_t transport;
    void* transport_list;
    void* event_handle;
    TaskHandle_t task_handle;
    bool run,wait_for_pong_resp,selected_for_destroying,controlled_mode,last_fin,controlled_disconnect_pending;
    bool controlled_connect_attempted,controlled_started;
    esp_tls_scope* controlled_scope;uint32_t controlled_generation;int64_t controlled_deadline_us;
    int controlled_close_result;
    int state,payload_len,payload_offset,last_opcode,close_status_code,wait_timeout_ms;
    uint64_t ping_tick_ms,pingpong_tick_ms,reconnect_tick_ms;
    struct {int error_type,esp_ws_handshake_status_code;} error_handle;
    int buffer_size;
    char *tx_buffer,*rx_buffer;
    char *errormsg_buffer;int errormsg_size;
};
typedef struct esp_websocket_client* esp_websocket_client_handle_t;
static int fixture_close_ledger(void);
static int64_t fake_us=1000,set_cost_us=0;
static unsigned wait_calls,forever_waits,creates,closes,frees,writes,connects,allocations,buffer_frees,mutex_takes,task_deletes,finishes;
static unsigned reads,data_events,connected_events,disconnected_events;
static bool cancel_after_read,cancel_after_data,cancel_after_connect;
static int connect_result=-1,write_result=0,close_result=0;
static unsigned redirects,errors;
static FakeTransport default_transport;
static TickType_t last_wait_ticks,last_send_ticks;
static int last_raw_timeout;
static TaskHandle_t current_task=(void*)1;
static void (*pending_task)(void*);
static void *pending_arg;
static bool task_create_fail,ack_during_wait,cancel_after_lock,cancel_after_write,fail_cleanup_lock;
static esp_websocket_client_handle_t active_client;
static char buffer[8];
static unsigned order[64],order_count;
static void note(unsigned n) {if(order_count<64)order[order_count++]=n;}
static int64_t esp_timer_get_time(void) {return fake_us;}
static uint64_t _tick_get_ms(void) {return (uint64_t)(fake_us/1000);}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current_task;}
static EventBits_t xEventGroupGetBits(FakeEvents *g) {return g->bits;}
static EventBits_t xEventGroupSetBits(FakeEvents *g,EventBits_t b) {g->bits|=b;fake_us+=set_cost_us;note(b&STOPPED_BIT?7:1);return g->bits;}
static EventBits_t xEventGroupClearBits(FakeEvents *g,EventBits_t b) {g->bits&=~b;return g->bits;}
static EventBits_t xEventGroupWaitBits(FakeEvents *g,EventBits_t b,bool clear,bool all,TickType_t ticks) {
    (void)clear;(void)all;++wait_calls;last_wait_ticks=ticks;if(ticks==portMAX_DELAY)++forever_waits;
    if(ack_during_wait){g->bits|=STOPPED_BIT;fixture_close_ledger();}
    if(!(g->bits&b) && ticks!=portMAX_DELAY)fake_us+=(int64_t)ticks*portTICK_PERIOD_MS*1000;
    return g->bits;
}
static int xSemaphoreTakeRecursive(FakeMutex *m,TickType_t ticks) {
    (void)ticks;++mutex_takes;
    if(m->busy || fail_cleanup_lock)return 0;
    ++m->depth;note(m==active_client->tx_lock?2:3);
    if(cancel_after_lock)active_client->status_bits->bits|=CONTROLLED_STOP_BIT;
    return pdPASS;
}
static void xSemaphoreGiveRecursive(FakeMutex *m) {if(!m->depth)abort();--m->depth;note(m==active_client->lock?5:6);}
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char *name,int stack,void* arg,int priority,TaskHandle_t* handle,int core) {
    (void)name;(void)stack;(void)priority;(void)core;++creates;if(task_create_fail)return 0;
    pending_task=fn;pending_arg=arg;*handle=(void*)2;return pdTRUE;
}
static void vTaskDelete(void *task) {(void)task;++task_deletes;note(8);}
static esp_transport_handle_t esp_transport_list_get_transport(void* list,const char* scheme) {(void)list;(void)scheme;return &default_transport;}
static int esp_transport_get_default_port(esp_transport_handle_t t) {(void)t;return 443;}
static int esp_transport_connect(esp_transport_handle_t t,const char *host,int port,int timeout) {
    (void)t;(void)host;(void)port;(void)timeout;++connects;
    if(cancel_after_connect)active_client->status_bits->bits|=CONTROLLED_STOP_BIT;
    return connect_result;
}
static int esp_transport_poll_read(esp_transport_handle_t t,int timeout) {(void)t;(void)timeout;return -1;}
static int fixture_close_ledger(void) {
    if(!active_client->controlled_mode)return close_result;
    if(close_result==ESP_WEBSOCKET_CLOSE_PENDING)return close_result;
    int fd=-1;const tls_scope_result claim=tls_scope_close_claim(active_client->controlled_scope,active_client->controlled_generation,&fd);
    if(claim==TLS_SCOPE_PENDING)return ESP_WEBSOCKET_CLOSE_PENDING;
    if(claim==TLS_SCOPE_ALREADY)return ESP_OK;
    if(claim!=TLS_SCOPE_OK)return ESP_FAIL;
    return tls_scope_close_finish(active_client->controlled_scope,active_client->controlled_generation,fd,close_result)==TLS_SCOPE_OK?ESP_OK:ESP_FAIL;
}
static int esp_transport_close(esp_transport_handle_t t) {(void)t;++closes;note(4);return fixture_close_ledger();}
static int esp_transport_ws_poll_connection_closed(esp_transport_handle_t t,int timeout) {(void)t;(void)timeout;return -1;}
static int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t t) {(void)t;return 0;}
static esp_tls_error_handle_t esp_transport_get_error_handle(esp_transport_handle_t t) {(void)t;return NULL;}
static const char* esp_err_to_name(int value) {(void)value;return "synthetic_error";}
static int esp_websocket_client_create_transport(esp_websocket_client_handle_t c) {c->transport=&default_transport;return ESP_OK;}
static const char* esp_transport_ws_get_redir_uri(esp_transport_handle_t t) {(void)t;++redirects;return "wss://synthetic.invalid/unused";}
static esp_err_t esp_websocket_client_set_uri(esp_websocket_client_handle_t c,const char *uri) {(void)c;(void)uri;return ESP_OK;}
static void esp_websocket_client_dispatch_event(esp_websocket_client_handle_t c,int event,const void* data,int len) {
    (void)c;(void)data;(void)len;if(event==WEBSOCKET_EVENT_FINISH)++finishes;
    if(event==WEBSOCKET_EVENT_CONNECTED)++connected_events;
    if(event==WEBSOCKET_EVENT_DISCONNECTED)++disconnected_events;
    if(event==WEBSOCKET_EVENT_DATA){++data_events;if(cancel_after_data)active_client->status_bits->bits|=CONTROLLED_STOP_BIT;}
}
static void esp_websocket_client_error(esp_websocket_client_handle_t c,const char* format,...) {(void)c;(void)format;++errors;}
static esp_err_t esp_websocket_client_abort_connection(esp_websocket_client_handle_t,int);
static int esp_transport_read(esp_transport_handle_t t,char* data,int len,int timeout) {
    (void)t;(void)timeout;++reads;memset(data,1,(size_t)len);
    if(cancel_after_read)active_client->status_bits->bits|=CONTROLLED_STOP_BIT;
    return len;
}
static int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t) {(void)t;return 12;}
static bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t) {(void)t;return true;}
static int esp_transport_ws_get_read_opcode(esp_transport_handle_t t) {(void)t;return 1;}
static void esp_event_loop_run(void* h,unsigned ticks) {(void)h;(void)ticks;}
static int destroy_and_free_resources(esp_websocket_client_handle_t c) {(void)c;++frees;return ESP_OK;}
static esp_err_t esp_websocket_new_buf(esp_websocket_client_handle_t c,bool tx) {++allocations;if(tx)c->tx_buffer=buffer;else c->rx_buffer=buffer;return ESP_OK;}
static void esp_websocket_free_buf(esp_websocket_client_handle_t c,bool tx) {(void)c;(void)tx;++buffer_frees;}
static int esp_transport_ws_send_raw(esp_transport_handle_t t,int opcode,const char* data,int len,int timeout) {
    (void)t;(void)opcode;(void)data;++writes;last_raw_timeout=timeout;last_send_ticks=(TickType_t)timeout;
    if(cancel_after_write)active_client->status_bits->bits|=CONTROLLED_STOP_BIT;
    return write_result<0?write_result:len;
}
static uint16_t htons(uint16_t value) {return (uint16_t)((value<<8)|(value>>8));}
static uint16_t ntohs(uint16_t value) {return htons(value);}
int esp_websocket_client_send_with_opcode(esp_websocket_client_handle_t,ws_transport_opcodes_t,const uint8_t*,int,TickType_t);

esp_err_t esp_websocket_client_request_stop(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_finish_cleanup(esp_websocket_client_handle_t);
