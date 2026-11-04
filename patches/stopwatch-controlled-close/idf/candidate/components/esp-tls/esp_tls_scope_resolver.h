/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_tls_scope.h"
struct sockaddr_storage;
#ifdef __cplusplus
extern "C" {
#endif
/* Controlled IPv4 resolver. No synchronous getaddrinfo or helper task. Queued
 * callbacks retain a scope lease until actual retirement, including cancellation.
 * Caller already pins TLS/scope and holds its own creation lease. */
esp_err_t esp_tls_scope_resolve(const char*,size_t,int family,esp_tls_scope*,uint32_t,struct sockaddr_storage*);
#ifdef __cplusplus
}
#endif
