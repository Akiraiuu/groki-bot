/* Local offline prototype. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { TLS_SCOPE_OK=0, TLS_SCOPE_PENDING=1, TLS_SCOPE_ALREADY=2,
    TLS_SCOPE_REJECTED=-1, TLS_SCOPE_STALE=-2, TLS_SCOPE_FAILED=-3 } tls_scope_result;
typedef enum { TLS_SCOPE_FAULT_NONE, TLS_SCOPE_FAULT_CLOCK, TLS_SCOPE_FAULT_CAPACITY,
    TLS_SCOPE_FAULT_SHUTDOWN, TLS_SCOPE_FAULT_CLOSE, TLS_SCOPE_FAULT_INVARIANT } tls_scope_fault;
typedef struct esp_tls_scope {
    uint32_t generation, serial, slots[32], io_mask, creator_slot, creator_serial;
    uintptr_t callers[32];
    int64_t call_deadlines[32];
    int64_t origin_us, last_us, deadline_us;
    int fd;
    void *tls_owner;
    void *sdk_owner;
    bool (*permission_probe)(void*);
    void *permission_context;
    tls_scope_fault fault;
    bool initialized, owned, cancelled, creation_started, creation_active, connect_started, connect_active;
    bool shutdown_taken, shutdown_active, shutdown_done, shutdown_ok;
    bool close_taken, close_active, close_confirmed;
} esp_tls_scope;
typedef struct { esp_tls_scope *owner;uint32_t generation, slot, serial;bool valid; } tls_scope_io;
typedef struct {
    bool owned,cancelled,creation_active,connect_active,shutdown_active,shutdown_done,shutdown_ok;
    bool close_active,close_confirmed,deadline_exceeded;
    uint32_t io_count;int fd;tls_scope_fault fault;int64_t remaining_us;
} tls_scope_snapshot;
/* Storage is zero initialized, single use, and externally pinned until EVERY
 * SDK/application/cancellation caller has made its last access. Never reset a
 * live scope. All OS calls run outside the short state critical section. */
bool tls_scope_init(esp_tls_scope*,uint32_t,int64_t now_us,int64_t deadline_us);
bool tls_scope_bind(esp_tls_scope*,uint32_t,void* tls_owner);
bool tls_scope_bind_sdk(esp_tls_scope*,uint32_t,void* sdk_owner);
/* Immutable, nonblocking application-permission probe. External caller pins
 * context lifetime; invoked outside the state lock, never after release. */
bool tls_scope_set_permission(esp_tls_scope*,uint32_t,bool (*probe)(void*),void*);
tls_scope_result tls_scope_io_begin(esp_tls_scope*,uint32_t,int64_t,tls_scope_io*);
/* Per-caller nested budgets never overwrite a concurrent reader/writer's budget.
 * Nonnegative timeout includes the whole operation; negative uses scope end.
 * Tokens must begin/end on the same task. No ISR callers. */
tls_scope_result tls_scope_io_begin_timed(esp_tls_scope*,uint32_t,int64_t,int timeout_ms,tls_scope_io*);
int64_t tls_scope_io_remaining(esp_tls_scope*,uint32_t,int64_t);
tls_scope_result tls_scope_io_end(tls_scope_io*);
bool tls_scope_permitted(esp_tls_scope*,uint32_t,int64_t);
tls_scope_result tls_scope_creation_begin(tls_scope_io*);
tls_scope_result tls_scope_publish_fd(tls_scope_io*,int fd,int64_t now_us);
tls_scope_result tls_scope_creation_end(tls_scope_io*);
tls_scope_result tls_scope_connect_begin(tls_scope_io*,int64_t now_us);
tls_scope_result tls_scope_connect_end(tls_scope_io*);
tls_scope_result tls_scope_cancel(esp_tls_scope*,uint32_t);
typedef int (*tls_scope_shutdown_fn)(int fd,void* user);
/* A PENDING return has made no shutdown call. At most ONE actual shutdown is
 * admitted. Failed shutdown never authorizes freeing. Connect syscall must
 * return before cancellation may borrow the fd. */
tls_scope_result tls_scope_cancel_run(esp_tls_scope*,uint32_t,tls_scope_shutdown_fn,void*);
tls_scope_result tls_scope_close_claim(esp_tls_scope*,uint32_t,int* fd);
tls_scope_result tls_scope_close_finish(esp_tls_scope*,uint32_t,int fd,int result);
tls_scope_snapshot tls_scope_observe(esp_tls_scope*,uint32_t,int64_t now_us);
bool tls_scope_release(esp_tls_scope*,uint32_t);
#ifdef __cplusplus
}
#endif
