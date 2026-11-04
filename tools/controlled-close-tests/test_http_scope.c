/* Actual HTTP lifecycle methods with HTTP/parser/transport declarations doubled. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_tls_scope.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_TLS_SCOPE_BUSY -2
typedef int esp_err_t;
typedef void* esp_transport_handle_t;
enum{HTTP_STATE_INIT=1,HTTP_EVENT_DISCONNECTED,HTTP_METHOD_POST};
typedef struct {void*data;} Buffer;
typedef struct {void*headers;Buffer*buffer;} Data;
typedef struct {char*scheme;} Connection;
typedef struct esp_http_client {
    esp_tls_scope*bounded_scope;uint32_t bounded_generation;bool bounded_performed;Connection connection_info;
    void*transport;void*transport_list;int state;Data*request,*response;
    void*if_name,*parser,*parser_settings,*auth_data,*current_header_key,*current_header_value,*location,*auth_header;
} *esp_http_client_handle_t;
typedef struct {esp_tls_scope*bounded_scope;uint32_t bounded_generation;bool is_async,disable_auto_redirect;int max_authorization_retries,method;const char*url;} esp_http_client_config_t;
static unsigned checks,freed,close_attempts,close_calls,list_calls;static int close_error,list_result;static int64_t now=1000;
static esp_tls_scope*active;
static void check(int v,const char*why){++checks;if(!v){fprintf(stderr,"%s\n",why);abort();}}
static int64_t esp_timer_get_time(void){return now;}
static int esp_transport_close(esp_transport_handle_t t){(void)t;++close_calls;int fd=-1;
    tls_scope_result claim=tls_scope_close_claim(active,1,&fd);if(claim==TLS_SCOPE_ALREADY)return 0;if(claim==TLS_SCOPE_PENDING)return -2;if(claim!=TLS_SCOPE_OK)return -1;
    if(fd>=0)++close_attempts;return tls_scope_close_finish(active,1,fd,close_error)==TLS_SCOPE_OK?0:-1;}
static esp_transport_handle_t esp_transport_list_get_transport(void*h,const char*s){(void)h;(void)s;return (void*)1;}
static int esp_transport_list_destroy(void*h){(void)h;++list_calls;return list_result;}
static void*esp_transport_get_error_handle(void*t){return t;}
static void http_dispatch_event(void*c,int e,void*p,int n){(void)c;(void)e;(void)p;(void)n;}
static void http_dispatch_event_to_event_loop(int e,void*p,size_t n){(void)e;(void)p;(void)n;}
static void fixture_free(void*p){if(p)++freed;free(p);}
static void http_header_destroy(void*h){fixture_free(h);}
static void esp_http_client_cached_buf_cleanup(Buffer*b){(void)b;}
static void _clear_connection_info(esp_http_client_handle_t h){(void)h;}
static void _clear_auth_data(esp_http_client_handle_t h){(void)h;}
#define free fixture_free
#include "http_methods.inc"
#undef free
static esp_http_client_handle_t make(esp_tls_scope*s,bool fd){active=s;close_attempts=close_calls=list_calls=freed=0;close_error=list_result=0;
    check(tls_scope_init(s,1,1000,60000000),"fresh HTTP scope");
    if(fd){tls_scope_io io={0};tls_scope_io_begin(s,1,1000,&io);tls_scope_creation_begin(&io);tls_scope_publish_fd(&io,42,1000);tls_scope_creation_end(&io);tls_scope_io_end(&io);}
    esp_http_client_handle_t h=calloc(1,sizeof(*h));h->bounded_scope=s;h->bounded_generation=1;h->transport=h->transport_list=(void*)1;h->connection_info.scheme="https";h->state=HTTP_STATE_INIT;
    h->request=calloc(1,sizeof(Data));h->request->buffer=calloc(1,sizeof(Buffer));h->request->buffer->data=malloc(3);return h;}
int main(void){
    {esp_tls_scope scope={0};esp_http_client_handle_t h=make(&scope,true);tls_scope_io io={0};tls_scope_io_begin(&scope,1,1000,&io);
    check(esp_http_client_cleanup(h)==-2&&freed==0&&list_calls==0&&close_attempts==0,"INIT state with pending IO cannot skip close and free handler/HTTP owner");
    tls_scope_io_end(&io);check(esp_http_client_cleanup(h)==0&&freed==4&&close_attempts==1,"actual returned reference permits one close and then HTTP buffer destruction");check(tls_scope_release(&scope,1),"closed HTTP scope releases");}
    {esp_tls_scope scope={0};esp_http_client_handle_t h=make(&scope,true);close_error=-1;
    check(esp_http_client_cleanup(h)==-1&&freed==0&&list_calls==0&&close_attempts==1,"real close failure retains HTTP owner and response handler");
    check(esp_http_client_cleanup(h)==-1&&close_attempts==1,"ledger refuses another actual close after failed close");
    // Harness-only teardown: no reset/retry API is added to production.
    free(h->request->buffer->data);free(h->request->buffer);free(h->request);free(h);}
    {esp_tls_scope scope={0};esp_http_client_handle_t h=make(&scope,true);list_result=-2;
    check(esp_http_client_cleanup(h)==-2&&freed==0&&tls_scope_observe(&scope,1,now).close_confirmed,"pending list cleanup preserves HTTP even when TLS close already confirmed");
    list_result=0;check(esp_http_client_cleanup(h)==0&&close_attempts==1&&freed==4,"list continuation never repeats actual socket close");check(tls_scope_release(&scope,1),"list completion releases scope");}
    {esp_tls_scope scope={0};esp_http_client_handle_t h=make(&scope,false);h->transport=h->transport_list=NULL;
    check(esp_http_client_cleanup(h)==0&&close_attempts==0&&freed==4,"empty pre-connect initialization can be proved closed");check(tls_scope_release(&scope,1),"empty scope released");}
    {esp_tls_scope scope={0};esp_http_client_handle_t h=make(&scope,false);
    esp_http_client_config_t c={&scope,1,false,true,-1,HTTP_METHOD_POST,"https://synthetic.invalid"};
    check(http_scope_config_valid(&c),"scoped HTTPS config admitted");c.is_async=true;check(!http_scope_config_valid(&c),"async scope rejected");c.is_async=false;c.disable_auto_redirect=false;check(!http_scope_config_valid(&c),"redirects rejected");c.disable_auto_redirect=true;c.max_authorization_retries=0;check(!http_scope_config_valid(&c),"implicit auth retry rejected");c.max_authorization_retries=-1;c.url="http://synthetic.invalid";check(!http_scope_config_valid(&c),"plain HTTP rejected before credentials");
    check(http_scope_begin(h)==0&&http_scope_begin(h)==ESP_ERR_INVALID_STATE,"exactly one perform admission per HTTP owner");check(esp_http_client_cleanup(h)==0&&tls_scope_release(&scope,1),"admission-only scope closes");}
    printf("{\"status\":\"passed\",\"checks\":%u,\"actual_http_methods\":4,\"http_transport_types_stubbed\":true,\"network_access\":false}\n",checks);
}
