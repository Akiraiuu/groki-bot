/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_tls_scope_resolver.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM) && !CONFIG_IDF_TARGET_LINUX
#include "esp_attr.h"
static DRAM_ATTR portMUX_TYPE resolver_mux=portMUX_INITIALIZER_UNLOCKED;
static void lock(void){taskENTER_CRITICAL(&resolver_mux);}
static void unlock(void){taskEXIT_CRITICAL(&resolver_mux);}
#else
#include <stdatomic.h>
static atomic_flag resolver_mux=ATOMIC_FLAG_INIT;
static void lock(void){while(atomic_flag_test_and_set_explicit(&resolver_mux,memory_order_acquire)) {}}
static void unlock(void){atomic_flag_clear_explicit(&resolver_mux,memory_order_release);}
#endif
typedef enum {WAITING,READY,FAILED,CANCELLED} resolver_status;
typedef struct {
    esp_tls_scope*scope;uint32_t generation;tls_scope_io lease;
    unsigned refs;resolver_status status;
    int64_t deadline_us;
    bool started,pending,cancel_requested,cancel_posted;
    ip_addr_t address;char hostname[254];
} resolver_job;
static unsigned active_jobs;
static void release_job(resolver_job*j){
    lock();const bool last=--j->refs==0;if(last)--active_jobs;unlock();
    if(last){tls_scope_io lease=j->lease;free(j);tls_scope_io_end(&lease);}
}
static void found(const char*name,const ip_addr_t*address,void*arg){
    (void)name;resolver_job*j=arg;
    const int64_t now=esp_timer_get_time();
    const bool allowed=now>=0 && now<j->deadline_us && tls_scope_permitted(j->scope,j->generation,now);
    lock();j->pending=false;
    if(j->cancel_requested||!allowed)j->status=CANCELLED;
    else if(address && IP_IS_V4(address)){j->address=*address;j->status=READY;}
    else j->status=FAILED;
    unlock();release_job(j); // Retire the DNS callback's reference.
}
static void start(void*arg){
    resolver_job*j=arg;
    const int64_t now=esp_timer_get_time();
    const bool allowed=now>=0 && now<j->deadline_us && tls_scope_io_remaining(j->scope,j->generation,now)>0;
    lock();bool cancelled=j->cancel_requested||!allowed;
    if(cancelled)j->status=CANCELLED;
    else{j->started=j->pending=true;++j->refs;}unlock();
    if(!cancelled){
        ip_addr_t address;
        const err_t result=dns_gethostbyname_addrtype(j->hostname,&address,found,j,LWIP_DNS_ADDRTYPE_IPV4);
        if(result!=ERR_INPROGRESS){
            lock();j->pending=false;
            if(j->cancel_requested)j->status=CANCELLED;
            else if(result==ERR_OK && IP_IS_V4(&address)){j->address=address;j->status=READY;}
            else j->status=FAILED;
            unlock();release_job(j); // No asynchronous found callback will occur.
        }
    }
    release_job(j); // Last access from the queued start callback.
}
static void cancel(void*arg){
    resolver_job*j=arg;lock();const bool pending=j->pending;unlock();
    if(pending){
        const int removed=dns_cancel_request(found,j); // TCP/IP core only.
        if(removed==1){lock();j->pending=false;unlock();release_job(j);}
        // Unexpected missing/multiple requester: retain the DNS reference.
        // No false retirement and no second cancellation request.
    }
    release_job(j); // Last access from this queued cancellation.
}
static void request_cancel(resolver_job*j){
    lock();j->cancel_requested=true;j->status=CANCELLED;
    const bool post=j->started&&j->pending&&!j->cancel_posted;
    if(post){j->cancel_posted=true;++j->refs;}unlock();
    if(post && tcpip_try_callback(cancel,j)!=ERR_OK)release_job(j);
    // Full mailbox is not retried; active DNS reference stays pinned until its
    // real found/timeout callback. A queued start reads cancel before any query.
}
esp_err_t esp_tls_scope_resolve(const char*host,size_t length,int family,esp_tls_scope*scope,uint32_t generation,struct sockaddr_storage*out){
    if(!host||!out||!scope||length==0||length>=254||(family!=AF_UNSPEC&&family!=AF_INET))return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<length;++i){const unsigned char c=(unsigned char)host[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return ESP_ERR_INVALID_ARG;}
    const int64_t admitted_at=esp_timer_get_time();
    const int64_t remaining=tls_scope_io_remaining(scope,generation,admitted_at);
    if(remaining<=0)return ESP_ERR_INVALID_STATE;
    lock();bool admitted=active_jobs<2;if(admitted)++active_jobs;unlock();
    if(!admitted)return ESP_ERR_NO_MEM;
    resolver_job*j=calloc(1,sizeof(*j));
    if(!j){lock();--active_jobs;unlock();return ESP_ERR_NO_MEM;}
    j->scope=scope;j->generation=generation;j->refs=2;j->status=WAITING;memcpy(j->hostname,host,length);
    j->deadline_us=admitted_at+remaining; // Carry caller's per-operation end across tasks.
    if(tls_scope_io_begin(scope,generation,esp_timer_get_time(),&j->lease)!=TLS_SCOPE_OK){free(j);lock();--active_jobs;unlock();return ESP_ERR_INVALID_STATE;}
    if(tcpip_try_callback(start,j)!=ERR_OK){release_job(j);release_job(j);return ESP_FAIL;}
    esp_err_t result=ESP_FAIL;
    for(;;){
        if(tls_scope_io_remaining(scope,generation,esp_timer_get_time())<=0){request_cancel(j);result=ESP_ERR_INVALID_STATE;break;}
        lock();const resolver_status status=j->status;ip_addr_t address=j->address;unlock();
        if(status==READY){
            memset(out,0,sizeof(*out));struct sockaddr_in*ipv4=(struct sockaddr_in*)out;
#if LWIP_SOCKET_HAVE_SA_LEN
            ipv4->sin_len=sizeof(*ipv4);
#endif
            ipv4->sin_family=AF_INET;ipv4->sin_addr.s_addr=ip4_addr_get_u32(ip_2_ip4(&address));result=ESP_OK;break;
        }
        if(status!=WAITING)break;
        vTaskDelay(1);
    }
    release_job(j);return result; // Queued/DNS references may legitimately remain.
}
