/* Actual two lwIP methods; table, IP and UDP declarations are doubles. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef unsigned char u8_t;
typedef struct {uint32_t addr;} ip_addr_t;
typedef void (*dns_found_callback)(const char*,const ip_addr_t*,void*);
#define LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING 1
#define LWIP_DNS_SECURE_RAND_SRC_PORT 2
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define DNS_MAX_REQUESTS 4
#define DNS_TABLE_SIZE 4
#define DNS_MAX_SOURCE_PORTS 4
enum {DNS_STATE_UNUSED,DNS_STATE_NEW,DNS_STATE_ASKING,DNS_STATE_DONE};
struct {int state,pcb_idx,ipaddr_cnt;char name[254];} dns_table[4];
struct {dns_found_callback found;void*arg;u8_t dns_table_idx;} dns_requests[4];
static void* dns_pcbs[4];
static unsigned checks,callbacks_a,callbacks_b,removed_pcbs;
static void check(int value,const char*why){++checks;if(!value){fprintf(stderr,"%s\n",why);abort();}}
#define LWIP_ASSERT_CORE_LOCKED() ((void)0)
#define LWIP_ASSERT(why,cond) check((cond),(why))
static void udp_remove(void*pcb){check(pcb!=NULL,"actual cleanup removes a live declared PCB");++removed_pcbs;}
#include "dns_methods.inc"
static void callback_a(const char*n,const ip_addr_t*a,void*ctx){(void)n;(void)a;(void)ctx;++callbacks_a;}
static void callback_b(const char*n,const ip_addr_t*a,void*ctx){(void)n;(void)a;(void)ctx;++callbacks_b;}
static int a,b,port;
static void reset(void){memset(dns_table,0,sizeof dns_table);memset(dns_requests,0,sizeof dns_requests);memset(dns_pcbs,0,sizeof dns_pcbs);callbacks_a=callbacks_b=removed_pcbs=0;dns_pcbs[0]=&port;}
int main(void){
    reset();dns_table[0].state=DNS_STATE_ASKING;dns_requests[0].found=callback_a;dns_requests[0].arg=&a;
    check(dns_cancel_request(callback_a,&a)==1,"exact consumer detached once");
    check(dns_requests[0].found==NULL&&dns_requests[0].arg==NULL&&dns_table[0].state==DNS_STATE_UNUSED&&callbacks_a==0,"cancel does not invoke callback or keep its query active");
    const unsigned count=removed_pcbs;check(dns_cancel_request(callback_a,&a)==0&&removed_pcbs==count,"repeat cancellation performs no cleanup work");
    reset();dns_table[0].state=DNS_STATE_ASKING;dns_requests[0].found=callback_a;dns_requests[0].arg=&a;
#if (LWIP_DNS_SECURE & LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING)
    dns_requests[1].found=callback_b;dns_requests[1].arg=&b;dns_requests[1].dns_table_idx=0;
    check(dns_cancel_request(callback_a,&a)==1&&dns_requests[1].found==callback_b&&dns_table[0].state==DNS_STATE_ASKING&&removed_pcbs==0,"shared query retains other consumer and socket");
    ip_addr_t address={1};dns_call_found(0,&address);check(callbacks_a==0&&callbacks_b==1,"only still-live shared consumer receives the result");
    reset();dns_requests[0].found=callback_a;dns_requests[0].arg=&a;dns_requests[0].dns_table_idx=250;
    check(dns_cancel_request(callback_a,&a)==-1&&dns_requests[0].found==callback_a,"invalid table provenance never fabricates removal");
#else
    dns_table[1].state=DNS_STATE_ASKING;dns_requests[1].found=callback_b;dns_requests[1].arg=&b;
    check(dns_cancel_request(callback_a,&a)==1&&dns_table[1].state==DNS_STATE_ASKING&&dns_requests[1].found==callback_b&&removed_pcbs==0,"separate query consumer stays alive");
#endif
    reset();dns_table[0].state=DNS_STATE_ASKING;dns_requests[0].found=callback_b;dns_requests[0].arg=&a;
    check(dns_cancel_request(callback_a,&a)==0&&dns_requests[0].found==callback_b,"callback identity is required in addition to context address");
    check(dns_cancel_request(callback_b,&b)==0&&dns_requests[0].found==callback_b,"other context cannot cancel this consumer");
    check(dns_cancel_request(NULL,&a)==0&&dns_cancel_request(callback_b,NULL)==0,"empty identities are rejected");
    printf("{\"status\":\"passed\",\"checks\":%u,\"actual_lwip_methods\":2,\"tables_and_udp_stubbed\":true,\"network_access\":false}\n",checks);
}
