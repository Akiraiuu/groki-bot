/* Declaration double; no DNS packets are sent. */
#pragma once
#include <stdint.h>
typedef int err_t;
typedef unsigned char u8_t;
typedef struct {uint32_t addr;} ip_addr_t;
#define IP_IS_V4(a) ((void)(a),1)
#define ip_2_ip4(a) (a)
#define ip4_addr_get_u32(a) ((a)->addr)
#define ERR_OK 0
#define ERR_INPROGRESS (-5)
#define ERR_MEM (-1)
#define LWIP_DNS_ADDRTYPE_IPV4 0
typedef void (*dns_found_callback)(const char*,const ip_addr_t*,void*);
#ifdef __cplusplus
extern "C" {
#endif
err_t dns_gethostbyname_addrtype(const char*,ip_addr_t*,dns_found_callback,void*,u8_t);
int dns_cancel_request(dns_found_callback,void*);
#ifdef __cplusplus
}
#endif
