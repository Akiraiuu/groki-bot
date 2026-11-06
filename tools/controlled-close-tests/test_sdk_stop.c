/* Executes verbatim SDK methods; no actual transport, task or socket exists. */
#include "sdk_boundary.h"
#include "sdk_methods.inc"
static unsigned checks;
static void check(bool ok,const char* why) {++checks;if(!ok){fprintf(stderr,"%s\n",why);abort();}}
static FakeEvents events;
static FakeMutex main_mutex,tx_mutex;
static FakeTransport transport;
static FakeConfig config;
static struct esp_websocket_client client;
static esp_tls_scope scope;
static void reset(bool controlled) {
    memset(&events,0,sizeof events);memset(&main_mutex,0,sizeof main_mutex);memset(&tx_mutex,0,sizeof tx_mutex);
    memset(&config,0,sizeof config);memset(&client,0,sizeof client);
    config.ext_transport=controlled?NULL:&transport;config.port=443;config.host="synthetic.invalid";config.scheme="wss";
    config.enable_bounded_stop=controlled;config.disable_auto_reconnect=true;
    config.network_timeout_ms=10000;config.ping_interval_sec=10;
    client.config=&config;client.controlled_mode=controlled;client.status_bits=&events;
    client.lock=&main_mutex;client.tx_lock=&tx_mutex;client.buffer_size=4;
    client.transport=&transport;client.task_handle=(void*)2;
    active_client=&client;current_task=(void*)1;events.bits=0;fake_us=1000;set_cost_us=0;
    memset(&scope,0,sizeof scope);if(!tls_scope_init(&scope,1,1000,INT64_MAX))abort();
    client.controlled_scope=config.bounded_scope=&scope;client.controlled_generation=config.bounded_generation=1;
    client.controlled_deadline_us=config.bounded_deadline_us=INT64_MAX;
    wait_calls=forever_waits=creates=closes=frees=writes=connects=allocations=buffer_frees=mutex_takes=task_deletes=finishes=0;
    reads=data_events=connected_events=disconnected_events=0;
    cancel_after_read=cancel_after_data=cancel_after_connect=false;connect_result=-1;write_result=0;close_result=0;redirects=errors=0;
    last_wait_ticks=last_send_ticks=0;last_raw_timeout=0;order_count=0;pending_task=NULL;pending_arg=NULL;
    task_create_fail=ack_during_wait=cancel_after_lock=cancel_after_write=fail_cleanup_lock=false;
}
static void run_pending(void) {void(*fn)(void*)=pending_task;void* arg=pending_arg;pending_task=NULL;pending_arg=NULL;check(fn!=NULL,"one deferred synthetic task");current_task=(void*)2;fn(arg);current_task=(void*)1;}
static void test_invalid_and_self_context(void) {
    reset(true);
    check(esp_websocket_client_stop_until(NULL,1000)==ESP_ERR_INVALID_ARG,"null handle rejected");
    check(esp_websocket_client_stop_until(&client,-1)==ESP_ERR_INVALID_ARG,"invalid deadline rejected");
    client.status_bits=NULL;check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_INVALID_ARG,"partial handle rejected");client.status_bits=&events;
    current_task=client.task_handle;check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_INVALID_STATE,"WS task cannot wait on itself");
    check(events.bits==0 && wait_calls==0 && closes==0,"rejected call has no stop wait or transport side effects");
    current_task=(void*)1;client.selected_for_destroying=true;
    check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_INVALID_STATE,"auto-destroy ownership rejected");
    events.bits=STOPPED_BIT;check(!esp_websocket_client_stop_observed(&client),"auto-free handle not advertised as observable");
    check(!esp_websocket_client_stop_observed(NULL),"null read is not an acknowledgement");
    reset(false);check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_INVALID_STATE,"extension requires init opt-in");
}
static void test_deadline_and_no_destruction(void) {
    reset(true);client.run=true;client.state=WEBSOCKET_STATE_CONNECTED;
    check(esp_websocket_client_stop_until(&client,36000)==ESP_ERR_TIMEOUT,"stalled SDK times out honestly");
    check(last_wait_ticks==3 && forever_waits==0 && fake_us==31000,"remaining original deadline rounded down to ticks");
    check((events.bits&(CONTROLLED_STOP_BIT|REQUESTED_STOP_BIT))==(CONTROLLED_STOP_BIT|REQUESTED_STOP_BIT),"sticky stop and wake latched");
    check(client.run && client.state==WEBSOCKET_STATE_CONNECTED && mutex_takes==0,"caller does not race run/state or SDK locks");
    check(closes==0 && frees==0 && !esp_websocket_client_stop_observed(&client),"timeout does not fake physical close or free owner");
    check(esp_websocket_client_destroy(&client)==ESP_ERR_INVALID_STATE && frees==0,"SDK destroy cannot wait forever or free active controlled handle");
    check(esp_websocket_client_destroy_on_exit(&client)==ESP_ERR_INVALID_STATE && !client.selected_for_destroying,"SDK cannot self-free an observed controlled owner");
    check(esp_websocket_client_start(&client)==ESP_ERR_INVALID_STATE && creates==0,"latched handle never restarts");
    reset(true);check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_TIMEOUT && wait_calls==0,"expired scope requests stop but never renews time");
    reset(true);check(esp_websocket_client_stop_until(&client,999)==ESP_ERR_TIMEOUT && wait_calls==0,"past original scope cannot be extended");
    reset(true);check(esp_websocket_client_stop_until(&client,1001)==ESP_ERR_TIMEOUT && last_wait_ticks==0 && forever_waits==0,"sub-tick remainder is a poll");
    reset(true);set_cost_us=50000;check(esp_websocket_client_stop_until(&client,20000)==ESP_ERR_TIMEOUT && wait_calls==0,"time sampled after requesting stop");
    reset(true);fake_us=-1;check(esp_websocket_client_stop_until(&client,1000)==ESP_ERR_INVALID_STATE && (events.bits&CONTROLLED_STOP_BIT),"invalid clock still latches stop and retains owner");
    reset(true);check(esp_websocket_client_stop_until(&client,INT64_MAX)==ESP_ERR_TIMEOUT && last_wait_ticks==portMAX_DELAY-1 && forever_waits==0,"large deadline never turns into infinite sentinel");
}
static void test_real_task_start_stop_race(void) {
    reset(true);events.bits=STOPPED_BIT;client.state=WEBSOCKET_STATE_UNKNOW;
    check(esp_websocket_client_start(&client)==ESP_OK && pending_task && creates==1,"actual SDK start schedules but does not run task yet");
    check(!esp_websocket_client_stop_observed(&client),"start clears old stopped acknowledgement");
    check(esp_websocket_client_stop_until(&client,20000)==ESP_ERR_TIMEOUT,"stop before WS task first instruction can time out");
    check(closes==0 && connects==0,"caller never closes transport behind pending task");
    run_pending();
    check(connects==0 && writes==0,"task run=true cannot overwrite sticky cancellation and open connection");
    check(finishes==1 && closes==1 && task_deletes==1 && esp_websocket_client_stop_observed(&client),"real task tail posts stopped only after finish and transport close");
    unsigned stop_index=64,close_index=64,last_release=0;
    for(unsigned i=0;i<order_count;++i){if(order[i]==4)close_index=i;if(order[i]==5||order[i]==6)last_release=i;if(order[i]==7)stop_index=i;}
    check(close_index<last_release && last_release<stop_index,"transport close and lock release precede stopped bit");
    check(main_mutex.depth==0 && tx_mutex.depth==0,"all cleanup locks returned");
    check(esp_websocket_client_start(&client)==ESP_ERR_INVALID_STATE && creates==1,"late stop ack does not admit another task");
    check(esp_websocket_client_destroy(&client)==ESP_OK && frees==1,"only stopped owner may be destroyed");
}
static void test_busy_owner_never_closed_underneath(void) {
    reset(true);events.bits=STOPPED_BIT;client.state=WEBSOCKET_STATE_UNKNOW;
    check(esp_websocket_client_start(&client)==ESP_OK,"busy-owner scope starts");
    (void)esp_websocket_client_stop_until(&client,1000);
    main_mutex.busy=true;run_pending();
    check(esp_websocket_client_stop_observed(&client) && !esp_websocket_client_transport_close_confirmed(&client) && closes==0 && finishes==0,"task exit can be observed while failed final lock retains transport");
    check(task_deletes==1 && frees==0 && tx_mutex.depth==0,"task may retire while failed handle stays owned; partial locks unwound");
    check(esp_websocket_client_destroy(&client)==ESP_ERR_INVALID_STATE && frees==0,"dead SDK task alone is never a resource-release ack");
}
static void test_sdk_write_gates(void) {
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;events.bits=CONTROLLED_STOP_BIT;
    check(esp_websocket_client_send_with_exact_opcode(&client,129,(const uint8_t*)"abcdefghij",10,30)==-1,"revoked SDK refuses write");
    check(mutex_takes==0 && writes==0 && allocations==0,"no new send work admitted after stop");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;cancel_after_lock=true;
    check(esp_websocket_client_send_with_exact_opcode(&client,129,(const uint8_t*)"abcdefghij",10,30)==-1,"cancellation while acquiring lock rechecked");
    check(writes==0 && allocations==0 && main_mutex.depth==0 && tx_mutex.depth==0,"post-lock rejection returns lock without I/O");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;cancel_after_write=true;
    check(esp_websocket_client_send_with_exact_opcode(&client,129,(const uint8_t*)"abcdefghij",10,30)==-1,"stop observed between fragments");
    check(writes==1 && buffer_frees==1 && closes==0 && main_mutex.depth==0 && tx_mutex.depth==0,"one in-flight fragment allowed; no next fragment, close or retained lock");
}
static void test_observed_and_task_failure(void) {
    reset(true);ack_during_wait=true;
    check(esp_websocket_client_stop_until(&client,21000)==ESP_OK && esp_websocket_client_stop_observed(&client),"only actual observed stopped bit permits success");
    check(closes==0 && frees==0 && forever_waits==0,"observer API never performs transport destruction");
    reset(true);events.bits=STOPPED_BIT;fake_us=999999;fixture_close_ledger();
    check(esp_websocket_client_stop_until(&client,1000)==ESP_OK && wait_calls==0,"already stopped remains a real fact even if caller must classify it late");
    reset(true);events.bits=STOPPED_BIT;task_create_fail=true;
    check(esp_websocket_client_start(&client)==ESP_FAIL && connects==0,"SDK task allocation failure sends no network traffic");
    check(esp_websocket_client_stop_observed(&client),"failed creation leaves actual no-task acknowledgement");
}
static void test_legacy_close_timeout_trap(void) {
    reset(false);client.run=true;client.state=WEBSOCKET_STATE_CONNECTED;
    (void)esp_websocket_client_close_with_optional_body(&client,true,1000,"",0,3);
    check(last_raw_timeout==-1 && writes==1,"legacy close passes an infinite send timeout despite finite close argument");
    check(wait_calls==2 && forever_waits==1 && last_wait_ticks==portMAX_DELAY,"legacy close has an infinite fallback wait; boundary intercepts rather than blocks it");
    reset(false);events.bits=STOPPED_BIT;client.state=WEBSOCKET_STATE_UNKNOW;
    check(esp_websocket_client_start(&client)==ESP_OK,"legacy mode start unchanged");run_pending();
    check(connects==1 && closes==2 && esp_websocket_client_stop_observed(&client),"legacy abort and final tail keep original behavior");
    check(esp_websocket_client_start(&client)==ESP_OK && creates==2,"legacy restart remains available without controlled opt-in");
}
static void test_receive_and_connect_cancellation(void) {
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;events.bits=CONTROLLED_STOP_BIT;
    check(esp_websocket_client_recv(&client)==ESP_OK && reads==0 && allocations==0,"stopped receive admits no new network or allocation");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;cancel_after_read=true;
    check(esp_websocket_client_recv(&client)==ESP_OK && reads==1 && data_events==0 && buffer_frees==1,"stop during in-flight read drops delivery and next read");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;cancel_after_data=true;
    check(esp_websocket_client_recv(&client)==ESP_OK && reads==1 && data_events==1 && buffer_frees==1,"fragmented receive checks stop between chunks");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;
    check(esp_websocket_client_recv(&client)==ESP_OK && reads==3 && data_events==3 && buffer_frees==1,"unrevoked receive still consumes complete synthetic payload");
    reset(true);events.bits=STOPPED_BIT;connect_result=0;cancel_after_connect=true;
    check(esp_websocket_client_start(&client)==ESP_OK,"connect race starts once");run_pending();
    check(connects==1 && connected_events==0 && reads==0 && writes==0,"late connect result cannot open input or send follow-ups");
    check(closes==1 && esp_websocket_client_stop_observed(&client),"late connect retains one orderly final close");
}
static void test_error_close_owner_and_cancelled_write(void) {
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;cancel_after_write=true;write_result=-1;
    check(esp_websocket_client_send_with_exact_opcode(&client,129,(const uint8_t*)"abcd",4,30)==-1,"cancelled failed write returns once");
    check(writes==1 && closes==0 && !client.controlled_disconnect_pending,"cancelled writer never closes TLS behind the owner");
    reset(true);client.state=WEBSOCKET_STATE_CONNECTED;client.run=true;
    check(esp_websocket_client_abort_connection(&client,WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT)==ESP_OK,"SDK error latches controlled close");
    check(closes==0 && disconnected_events==0 && client.controlled_disconnect_pending && (events.bits&CONTROLLED_STOP_BIT),"error route defers physical cleanup until both SDK locks retire");
    current_task=(void*)2;esp_websocket_client_task(&client);current_task=(void*)1;
    check(closes==1 && disconnected_events==1 && esp_websocket_client_stop_observed(&client),"single final owner reports disconnect only after close attempt");
}
static void test_failed_close_and_config_boundaries(void) {
    reset(true);check(controlled_config_valid(&config),"bounded mode accepts fresh owned non-reconnecting transport");
    config.disable_auto_reconnect=false;check(!controlled_config_valid(&config),"implicit reconnect rejected");config.disable_auto_reconnect=true;
    config.enable_close_reconnect=true;check(!controlled_config_valid(&config),"close reconnect rejected");config.enable_close_reconnect=false;
    config.ext_transport=&transport;check(!controlled_config_valid(&config),"external preconnected descriptor ownership not guessed");
    config.enable_bounded_stop=false;check(controlled_config_valid(&config) && !controlled_config_valid(NULL),"legacy config unchanged and null rejected");
    reset(true);client.controlled_connect_attempted=true;events.bits=CONTROLLED_STOP_BIT;close_result=-1;
    current_task=(void*)2;esp_websocket_client_task(&client);current_task=(void*)1;
    check(esp_websocket_client_stop_observed(&client) && !esp_websocket_client_transport_close_confirmed(&client),"SDK task exit is separate from failed transport close");
    check(esp_websocket_client_stop_until(&client,2000)==ESP_ERR_TIMEOUT && wait_calls==0,"failed close never becomes successful stop");
    check(esp_websocket_client_destroy(&client)==ESP_ERR_INVALID_STATE && frees==0,"failed final close preserves owned handle");
    reset(true);events.bits=STOPPED_BIT;check(esp_websocket_client_start(&client)==ESP_OK,"pre-connect cancellation starts once");
    (void)esp_websocket_client_stop_until(&client,1000);close_result=-1;run_pending();
    check(connects==0 && !esp_websocket_client_transport_close_confirmed(&client),"close failure cannot become a no-connect shortcut acknowledgement");
    reset(true);events.bits=STOPPED_BIT;connect_result=302;
    check(esp_websocket_client_start(&client)==ESP_OK,"redirect response has one original connect");run_pending();
    check(connects==1 && redirects==0 && errors==1 && (events.bits&CONTROLLED_STOP_BIT),"redirect cannot create another connection or forward credentials");
    check(esp_websocket_client_start(&client)==ESP_ERR_INVALID_STATE && creates==1,"controlled handle remains one-shot after error");
}
static void test_scope_wire_and_pending_cleanup(void) {
    reset(true);tls_scope_io io={0};check(tls_scope_io_begin(&scope,1,fake_us,&io)==TLS_SCOPE_OK,"live TLS operation enters");
    current_task=client.task_handle;check(esp_websocket_client_request_stop(&client)==ESP_OK,"WS callback context may request without self wait");current_task=(void*)1;
    check(tls_scope_observe(&scope,1,fake_us).cancelled && wait_calls==0 && closes==0,"request reaches real scope before any wait");
    events.bits|=STOPPED_BIT;client.controlled_started=true;client.controlled_close_result=ESP_WEBSOCKET_CLOSE_PENDING;
    check(esp_websocket_client_finish_cleanup(&client)==ESP_WEBSOCKET_CLOSE_PENDING && !esp_websocket_client_transport_close_confirmed(&client),"retired task does not authorize close over live IO");
    check(esp_websocket_client_destroy(&client)==ESP_ERR_INVALID_STATE && frees==0,"pending cleanup keeps SDK owner");
    check(tls_scope_io_end(&io)==TLS_SCOPE_OK,"actual TLS operation retires");
    check(esp_websocket_client_finish_cleanup(&client)==ESP_OK && esp_websocket_client_transport_close_confirmed(&client),"cleanup continues only after real reference return");
    const unsigned closed=closes;check(esp_websocket_client_finish_cleanup(&client)==ESP_OK && closes==closed,"confirmed close is not repeated");
    check(esp_websocket_client_destroy(&client)==ESP_OK && frees==1,"confirmed close and task exit permit SDK destruction");
    reset(true);events.bits=STOPPED_BIT;client.controlled_close_result=ESP_WEBSOCKET_CLOSE_PENDING;close_result=ESP_FAIL;
    check(esp_websocket_client_finish_cleanup(&client)==ESP_FAIL && closes==1,"close error returns once");
    check(esp_websocket_client_finish_cleanup(&client)==ESP_FAIL && closes==1 && !esp_websocket_client_transport_close_confirmed(&client),"sticky close failure never repeats close or manufactures success");
    reset(true);events.bits=STOPPED_BIT;client.controlled_close_result=ESP_WEBSOCKET_CLOSE_PENDING;main_mutex.busy=true;
    check(esp_websocket_client_finish_cleanup(&client)==ESP_WEBSOCKET_CLOSE_PENDING && closes==0,"busy SDK lock returns without transport call");
    main_mutex.busy=false;check(esp_websocket_client_finish_cleanup(&client)==ESP_OK,"sole owner can continue busy lock cleanup");
    reset(true);config.bounded_generation=2;check(!controlled_config_valid(&config),"stale scope generation rejected at init");
    config.bounded_generation=1;config.bounded_deadline_us=20000;check(!controlled_config_valid(&config),"different absolute scope end rejected");
    config.bounded_deadline_us=INT64_MAX;tls_scope_cancel(&scope,1);check(!controlled_config_valid(&config),"cancelled scope cannot initialize a client");
    reset(true);check(controlled_bind_scope(&client,&config),"first SDK owner binds a fresh scope");
    struct esp_websocket_client other={0};check(!controlled_bind_scope(&other,&config),"second SDK owner cannot share or cancel an existing scope");
    reset(true);int tls_owner=0;check(tls_scope_bind(&scope,1,&tls_owner),"standalone TLS owner binds");
    check(!controlled_bind_scope(&client,&config),"SDK refuses to adopt an already attached TLS object");
}
int main(void) {
    test_invalid_and_self_context();test_deadline_and_no_destruction();test_real_task_start_stop_race();
    test_busy_owner_never_closed_underneath();test_sdk_write_gates();test_observed_and_task_failure();test_legacy_close_timeout_trap();
    test_receive_and_connect_cancellation();test_error_close_owner_and_cancelled_write();
    test_failed_close_and_config_boundaries();
    test_scope_wire_and_pending_cleanup();
    printf("{\"status\":\"passed\",\"checks\":%u,\"actual_sdk_method_bodies\":true,\"rtos_transport_clock_stubbed\":true,\"device_accessed\":false}\n",checks);
    return 0;
}
