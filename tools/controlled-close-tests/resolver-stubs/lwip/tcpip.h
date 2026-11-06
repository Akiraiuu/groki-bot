#pragma once
#include "dns.h"
#ifdef __cplusplus
extern "C" {
#endif
err_t tcpip_try_callback(void (*)(void*),void*);
#ifdef __cplusplus
}
#endif
