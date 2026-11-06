/* Production HTTP connect -> SSL connect -> TLS admission with explicit fake
 * parser/transport allocation and lower TLS/socket/crypto boundaries. No socket. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_tls_scope.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_TLS_SCOPE_BUSY -2
#define HTTP_EVENT_DISCONNECTED 2
#define DEFAULT_HTTP_PORT 80
#define DEFAULT_HTTPS_PORT 443
#define HTTP_TRANSPORT_OVER_SSL 2
#define HTTP_GOTO_ON_FALSE_DBG(c,e,label,...) do{if(!(c)){ret=(e);goto label;}}while(0)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_CONNECTING 0x7001
#define ESP_ERR_HTTP_INVALID_TRANSPORT 0x7003
#define ESP_TLS_ERR_TYPE_ESP 1
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x8001
#define CONFIG_ESP_TLS_USING_MBEDTLS 1
#define INVALID_SOCKET -1
#define ERR_TCP_TRANSPORT_NO_MEM -2
#define ASYNC_TRANS_CONNECT_FAIL -1
#define ASYNC_TRANS_CONNECTING 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(...) ((void)0)
#define ESP_STATIC_ANALYZER_CHECK(c,r) do{if(c)return r;}while(0)
typedef int esp_err_t;
typedef void* esp_tls_error_handle_t;
enum{ESP_TLS_INIT=0,HTTP_STATE_UNINIT=0,HTTP_STATE_INIT=1,HTTP_STATE_CONNECTED=2,HTTP_EVENT_ON_CONNECTED=1,HTTP_METHOD_POST=2};
typedef struct {bool is_plain_tcp,non_block;int timeout_ms;} esp_tls_cfg_t;
typedef struct {esp_tls_scope*scope;uint32_t scope_generation;int sockfd,conn_state;void*error_handle;} esp_tls_t;
typedef struct {esp_tls_scope*scope;uint32_t scope_generation;esp_tls_t*tls;bool ssl_initialized;int sockfd;esp_tls_cfg_t cfg;} transport_esp_tls_t;
typedef struct transport {transport_esp_tls_t data;} *esp_transport_handle_t;
typedef struct {char*scheme,*host,*path,*query,*username,*password;int port,method;} Connection;
typedef struct {void*buffer;} Response;
typedef struct client {Response*response;esp_tls_scope*bounded_scope;uint32_t bounded_generation;bool bounded_performed,is_async;int state,timeout_ms;Connection connection_info;esp_transport_handle_t transport;void*transport_list;} *esp_http_client_handle_t;
typedef struct {esp_tls_scope*bounded_scope;uint32_t bounded_generation;bool is_async,disable_auto_redirect;int max_authorization_retries,method;const char*url;int transport_type,port;} esp_http_client_config_t;
static int64_t now_us=1000000,platform_skew;
static unsigned checks,cases,lower_calls,alloc_calls,close_calls,http_events;
static bool allowed=true,fail_alloc,fail_prepare,lower_fd=true;
static int lower_result=1;
static int64_t lower_advance;
static int64_t esp_timer_get_time(void){return now_us;}
static uint64_t esp_tls_get_platform_time(void){return (uint64_t)(now_us+platform_skew);}
static bool permitted(void*context){(void)context;return allowed;}
static void check(bool value,const char*why){++checks;if(!value){fprintf(stderr,"%s\n",why);abort();}}
static int esp_tls_low_level_conn(const char*host,int length,int port,const esp_tls_cfg_t*cfg,esp_tls_t*tls){
    (void)host;(void)length;(void)port;(void)cfg;++lower_calls;
    if(lower_fd && tls->sockfd<0){
        tls_scope_io creation={0};
        if(tls_scope_io_begin(tls->scope,tls->scope_generation,now_us,&creation)!=TLS_SCOPE_OK)return -1;
        check(tls_scope_creation_begin(&creation)==TLS_SCOPE_OK,"fake lower boundary owns creator lease");
        check(tls_scope_publish_fd(&creation,42,now_us)==TLS_SCOPE_OK,"fake fd published to real ledger");
        tls->sockfd=42;check(tls_scope_creation_end(&creation)==TLS_SCOPE_OK,"creator returns");
        check(tls_scope_io_end(&creation)==TLS_SCOPE_OK,"creator lease retired");
    }
    now_us+=lower_advance;return lower_result;
}
static esp_tls_t* esp_tls_init(void){
    ++alloc_calls;if(fail_alloc)return NULL;esp_tls_t*t=calloc(1,sizeof(*t));check(t!=NULL,"host allocation");t->sockfd=-1;return t;
}
static int close_scope(esp_tls_scope*s,uint32_t generation){
    int fd=-1;tls_scope_result r=tls_scope_close_claim(s,generation,&fd);
    if(r==TLS_SCOPE_ALREADY)return 0;
    if(r!=TLS_SCOPE_OK)return -1;
    if(fd>=0)++close_calls;
    return tls_scope_close_finish(s,generation,fd,0)==TLS_SCOPE_OK?0:-1;
}
static int esp_tls_conn_destroy(esp_tls_t*t){
    if(t->scope && close_scope(t->scope,t->scope_generation)!=0)return -1;
    free(t);return 0;
}
static int esp_tls_get_error_handle(esp_tls_t*t,esp_tls_error_handle_t*out){(void)t;*out=NULL;return 0;}
static int esp_tls_get_conn_sockfd(esp_tls_t*t,int*out){*out=t->sockfd;return t->sockfd<0?-1:0;}
static transport_esp_tls_t* ssl_get_context_data(esp_transport_handle_t t){return t?&t->data:NULL;}
static void capture_tcp_transport_error(esp_transport_handle_t t,int e){(void)t;(void)e;}
static void esp_transport_set_errors(esp_transport_handle_t t,void*e){(void)t;(void)e;}
#include "tls.inc"
#include "ssl.inc"
static int esp_http_client_prepare(esp_http_client_handle_t c){(void)c;return fail_prepare?ESP_ERR_INVALID_STATE:ESP_OK;}
static int esp_http_client_close(esp_http_client_handle_t c);
static esp_transport_handle_t esp_transport_list_get_transport(void*list,const char*scheme){(void)scheme;return list;}
static int esp_transport_connect(esp_transport_handle_t t,const char*h,int port,int timeout){return ssl_connect(t,h,port,timeout);}
static int esp_transport_connect_async(esp_transport_handle_t t,const char*h,int port,int timeout){(void)t;(void)h;(void)port;(void)timeout;return -1;}
static void http_dispatch_event(esp_http_client_handle_t c,int event,void*data,int length){(void)c;(void)event;(void)data;(void)length;++http_events;}
static void http_dispatch_event_to_event_loop(int event,void*data,size_t length){(void)event;(void)data;(void)length;}
#include "http.inc"

static int esp_transport_close(esp_transport_handle_t t){return close_scope(t->data.scope,t->data.scope_generation);}
static void*esp_transport_get_error_handle(esp_transport_handle_t t){(void)t;return NULL;}
static void esp_http_client_cached_buf_cleanup(void*b){(void)b;}
static int esp_http_client_set_header(esp_http_client_handle_t c,const char*k,const char*v){(void)c;(void)k;(void)v;return ESP_OK;}
static bool http_utils_assign_string(char**dest,const char*s,int length){
    if(length<0)length=(int)strlen(s);char*copy=malloc((size_t)length+1);if(!copy)return false;
    memcpy(copy,s,(size_t)length);copy[length]=0;free(*dest);*dest=copy;return true;
}
static int http_utils_get_string_between(char*s,const char*a,const char*b,char**o){(void)s;(void)a;(void)b;(void)o;return ESP_FAIL;}
static int http_utils_get_string_after(char*s,const char*a,char**o){(void)s;(void)a;(void)o;return ESP_FAIL;}
enum{UF_SCHEMA,UF_HOST,UF_PORT,UF_USERINFO,UF_PATH,UF_QUERY,UF_FRAGMENT};
struct http_parser_url{struct{unsigned off,len;}field_data[7];};
static void http_parser_url_init(struct http_parser_url*u){memset(u,0,sizeof(*u));}
/* Deliberate parser substitute: this fixture accepts one synthetic HTTPS URL. */
static int http_parser_parse_url(const char*s,size_t n,int connect,struct http_parser_url*u){
    (void)n;(void)connect;if(strcmp(s,"https://synthetic.invalid/")!=0)return -1;
    u->field_data[UF_SCHEMA].len=5;u->field_data[UF_HOST].off=8;u->field_data[UF_HOST].len=17;u->field_data[UF_PATH].off=25;u->field_data[UF_PATH].len=1;return 0;
}
#include "http-init.inc"

typedef struct {esp_tls_scope scope;struct client http;struct transport transport;Response response;} Fixture;
static void init(Fixture*f){
    memset(f,0,sizeof(*f));now_us=1000000;platform_skew=0;allowed=true;fail_alloc=fail_prepare=false;lower_result=1;lower_fd=true;lower_advance=0;
    lower_calls=alloc_calls=close_calls=http_events=0;
    check(tls_scope_init(&f->scope,1,now_us,now_us+15000000),"real request scope initialization");
    check(tls_scope_set_permission(&f->scope,1,permitted,NULL),"real permission callback");
    check(tls_scope_bind_sdk(&f->scope,1,&f->http),"HTTP owner binds real scope");
    f->http.response=&f->response;f->http.bounded_scope=&f->scope;f->http.bounded_generation=1;f->http.connection_info=(Connection){.scheme="https",.host="synthetic.invalid",.port=443};f->http.state=HTTP_STATE_INIT;f->http.timeout_ms=15000;f->http.transport_list=&f->transport;
    f->transport.data.sockfd=-1;
    esp_http_client_config_t config={&f->scope,1,false,true,-1,HTTP_METHOD_POST,"https://synthetic.invalid",0,0};
    check(http_scope_config_valid(&config),"actual HTTP HTTPS config gate");
    check(esp_transport_ssl_set_scope(&f->transport,&f->scope,1)==ESP_OK,"actual SSL scope attachment");
}
static int run(Fixture*f){int r=http_scope_begin(&f->http);return r==ESP_OK?esp_http_client_connect(&f->http):r;}
static void finish(Fixture*f){
    if(f->transport.data.tls){check(esp_tls_conn_destroy(f->transport.data.tls)==0,"fake destructor uses real ledger close");f->transport.data.tls=NULL;}
    else check(close_scope(&f->scope,1)==0,"empty or previously closed scope proves closed");
    check(tls_scope_observe(&f->scope,1,now_us).io_count==0,"no retired IO still owned");
    check(tls_scope_release(&f->scope,1),"real scope releases only after ledger close");++cases;
}
int main(void){
    {Fixture f;init(&f);check(run(&f)==ESP_OK,"production HTTP -> SSL -> TLS admission succeeds under compatible boundaries");check(lower_calls==1&&f.http.state==HTTP_STATE_CONNECTED&&f.transport.data.sockfd==42,"connected prefix and published fd");finish(&f);check(close_calls==1,"one fd close on success");}
    {Fixture f;init(&f);allowed=false;check(run(&f)==ESP_ERR_INVALID_STATE,"revoked grant denied before TLS");check(lower_calls==0&&alloc_calls==0,"no lower call after revoke");finish(&f);}
    {Fixture f;init(&f);check(http_scope_begin(&f.http)==0&&http_scope_begin(&f.http)==ESP_ERR_INVALID_STATE,"one perform admission cannot retry");check(lower_calls==0,"no synthetic retry");finish(&f);}
    {Fixture f;init(&f);platform_skew=-100;check(run(&f)==ESP_ERR_HTTP_CONNECT,"clock mismatch maps to generic HTTP connect error");check(f.scope.fault==TLS_SCOPE_FAULT_CLOCK&&lower_calls==0,"clock failure distinguished at scope before lower boundary");platform_skew=0;finish(&f);}
    {Fixture f;init(&f);fail_alloc=true;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==0,"TLS allocation failure maps to HTTP connect");finish(&f);}
    {Fixture f;init(&f);fail_prepare=true;check(run(&f)==ESP_ERR_INVALID_STATE&&alloc_calls==0,"HTTP preparation failure occurs before TLS");finish(&f);}
    {Fixture f;init(&f);lower_fd=false;lower_result=-1;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==1,"lower failure before fd creation");finish(&f);check(close_calls==0,"no invented fd close");}
    {Fixture f;init(&f);lower_result=-1;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==1,"lower failure after fd publication");finish(&f);check(close_calls==1,"one fd close after lower failure");}
    {Fixture f;init(&f);lower_result=0;lower_advance=16000000;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==1,"pending lower boundary returns beyond finite request budget without retry");finish(&f);}
    {Fixture f;init(&f);f.transport.data.cfg.non_block=true;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==0,"scoped async configuration refused by actual TLS function");finish(&f);}
    {Fixture f;init(&f);f.transport.data.cfg.is_plain_tcp=true;check(run(&f)==ESP_ERR_HTTP_CONNECT&&lower_calls==0,"scoped plain TCP refused by actual TLS function");finish(&f);}
    {Fixture f;init(&f);check(run(&f)==0,"first SSL connection succeeds");unsigned before=lower_calls;check(ssl_connect(&f.transport,"synthetic.invalid",443,15000)==-1&&lower_calls==before,"same SSL owner refuses a second connect");finish(&f);}

    for(int explicit_ssl=0;explicit_ssl<2;++explicit_ssl){
        Fixture f;init(&f);f.http.connection_info=(Connection){0};
        esp_http_client_config_t config={0};config.transport_type=explicit_ssl?HTTP_TRANSPORT_OVER_SSL:0;
        check(config_transport_defaults(&f.http,&config)==ESP_OK,"exact SDK defaults branch succeeds");
        check(f.http.connection_info.port==(explicit_ssl?443:80),"exact transport default port");
        check(esp_http_client_set_url(&f.http,"https://synthetic.invalid/")==ESP_OK,"actual URL setup succeeds");
        check(f.http.connection_info.port==443,"URL parser sets HTTPS port");
        check(tls_scope_observe(&f.scope,1,now_us).close_confirmed==!explicit_ssl,"default 80-to-443 change closes fresh scope during URL initialization");
        int result=run(&f);
        if(explicit_ssl){check(result==ESP_OK&&lower_calls==1,"explicit SSL preserves fresh admission and reaches compatible TLS boundary");}
        else{check(result==ESP_ERR_INVALID_STATE&&lower_calls==0&&alloc_calls==0,"zero-initialized transport reproduces invalid_state before any lower TLS call");}
        finish(&f);free(f.http.connection_info.scheme);free(f.http.connection_info.host);free(f.http.connection_info.path);
    }
    printf("{\"status\":\"passed\",\"cases\":%u,\"checks\":%u,\"real_network_used\":false,\"lower_tls_crypto_socket_mocked\":true}\n",cases,checks);
}
