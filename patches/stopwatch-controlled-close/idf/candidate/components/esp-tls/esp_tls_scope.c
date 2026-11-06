/* Local offline prototype. SPDX-License-Identifier: Apache-2.0 */
#include "esp_tls_scope.h"
#if defined(ESP_PLATFORM) && !CONFIG_IDF_TARGET_LINUX
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static DRAM_ATTR portMUX_TYPE scope_mux=portMUX_INITIALIZER_UNLOCKED;
static void state_lock(void){taskENTER_CRITICAL(&scope_mux);}
static void state_unlock(void){taskEXIT_CRITICAL(&scope_mux);}
static uintptr_t caller_id(void){return (uintptr_t)xTaskGetCurrentTaskHandle();}
#else
#include <stdatomic.h>
static atomic_flag scope_mux=ATOMIC_FLAG_INIT;
static void state_lock(void){while(atomic_flag_test_and_set_explicit(&scope_mux,memory_order_acquire)) {}}
static void state_unlock(void){atomic_flag_clear_explicit(&scope_mux,memory_order_release);}
static _Thread_local char caller_anchor;
static uintptr_t caller_id(void){return (uintptr_t)&caller_anchor;}
#endif
static bool current(const esp_tls_scope*s,uint32_t g){return s&&s->owned&&g&&s->generation==g;}
static void probe_permission(esp_tls_scope*s,uint32_t g){
    bool (*probe)(void*)=0;void*context=0;
    state_lock();if(current(s,g)&&!s->cancelled){probe=s->permission_probe;context=s->permission_context;}state_unlock();
    if(probe && !probe(context))tls_scope_cancel(s,g);
}
static void fail(esp_tls_scope*s,tls_scope_fault f){if(s->fault==TLS_SCOPE_FAULT_NONE)s->fault=f;s->cancelled=true;}
static void tick(esp_tls_scope*s,int64_t now){
    if(now<s->origin_us){fail(s,TLS_SCOPE_FAULT_CLOCK);return;}
    if(now>s->last_us)s->last_us=now;
    if(s->last_us>=s->deadline_us)s->cancelled=true;
}
static bool valid_io(const tls_scope_io*t){return t&&t->valid&&current(t->owner,t->generation)&&t->slot<32&&
    (t->owner->io_mask&(UINT32_C(1)<<t->slot))&&t->owner->slots[t->slot]==t->serial;}
static bool creator(const tls_scope_io*t){return valid_io(t)&&t->owner->creation_active&&
    t->owner->creator_slot==t->slot&&t->owner->creator_serial==t->serial;}
bool tls_scope_init(esp_tls_scope*s,uint32_t g,int64_t now,int64_t end){
    state_lock();bool ok=s&&!s->initialized&&g&&now>=0&&end>0;
    if(ok){s->initialized=s->owned=true;s->generation=g;s->origin_us=s->last_us=now;s->deadline_us=end;s->fd=-1;s->cancelled=now>=end;}
    state_unlock();return ok;
}
bool tls_scope_bind(esp_tls_scope*s,uint32_t g,void*owner){
    state_lock();bool ok=current(s,g)&&owner&&!s->tls_owner&&!s->creation_started&&!s->io_mask&&!s->close_taken;
    if(ok){
        s->tls_owner=owner;
    }
    state_unlock();
    return ok;
}
bool tls_scope_set_permission(esp_tls_scope*s,uint32_t g,bool(*probe)(void*),void*context){
    state_lock();const bool ok=current(s,g)&&probe&&!s->permission_probe&&!s->sdk_owner&&!s->tls_owner&&!s->io_mask&&!s->creation_started;
    if(ok){s->permission_probe=probe;s->permission_context=context;}state_unlock();return ok;
}
bool tls_scope_bind_sdk(esp_tls_scope*s,uint32_t g,void*owner){
    state_lock();const bool ok=current(s,g)&&owner&&!s->cancelled&&!s->sdk_owner&&!s->tls_owner&&!s->creation_started&&!s->io_mask&&!s->close_taken;
    if(ok){
        s->sdk_owner=owner;
    }
    state_unlock();
    return ok;
}
tls_scope_result tls_scope_io_begin(esp_tls_scope*s,uint32_t g,int64_t now,tls_scope_io*t){
    if(!t||t->valid)return TLS_SCOPE_REJECTED;
    probe_permission(s,g);
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(current(s,g)){
        tick(s,now);r=TLS_SCOPE_REJECTED;
        if(!s->cancelled&&!s->close_taken){
            if(s->io_mask==UINT32_MAX||s->serial==UINT32_MAX){fail(s,TLS_SCOPE_FAULT_CAPACITY);r=TLS_SCOPE_FAILED;}
            else for(uint32_t i=0;i<32;++i)if(!(s->io_mask&(UINT32_C(1)<<i))){
                s->io_mask|=UINT32_C(1)<<i;s->slots[i]=++s->serial;
                *t=(tls_scope_io){s,g,i,s->serial,true};r=TLS_SCOPE_OK;break;
            }
        }
    }state_unlock();return r;
}
tls_scope_result tls_scope_io_end(tls_scope_io*t){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(valid_io(t)){esp_tls_scope*s=t->owner;
        if(creator(t)){fail(s,TLS_SCOPE_FAULT_INVARIANT);r=TLS_SCOPE_REJECTED;}
        else{s->io_mask&=~(UINT32_C(1)<<t->slot);s->slots[t->slot]=0;s->callers[t->slot]=0;s->call_deadlines[t->slot]=0;r=TLS_SCOPE_OK;}
    }state_unlock();if(t&&r==TLS_SCOPE_OK)t->valid=false;return r;
}
tls_scope_result tls_scope_io_begin_timed(esp_tls_scope*s,uint32_t g,int64_t now,int timeout_ms,tls_scope_io*t){
    const uintptr_t caller=caller_id();
    if(!caller)return TLS_SCOPE_REJECTED;
    tls_scope_result r=tls_scope_io_begin(s,g,now,t);
    if(r!=TLS_SCOPE_OK)return r;
    const int64_t span=timeout_ms<0?INT64_MAX:(int64_t)timeout_ms*1000;
    state_lock();
    if(valid_io(t)){
        s->callers[t->slot]=caller;
        const int64_t local=span>INT64_MAX-now?INT64_MAX:now+span;
        s->call_deadlines[t->slot]=local<s->deadline_us?local:s->deadline_us;
    }else r=TLS_SCOPE_STALE;
    state_unlock();return r;
}
int64_t tls_scope_io_remaining(esp_tls_scope*s,uint32_t g,int64_t now){
    probe_permission(s,g);
    const uintptr_t caller=caller_id();int64_t result=-1;
    state_lock();if(current(s,g)){
        tick(s,now);
        if(!s->cancelled&&!s->close_taken){
            int64_t end=s->deadline_us;
            for(unsigned i=0;i<32;++i)if((s->io_mask&(UINT32_C(1)<<i))&&s->callers[i]==caller&&s->call_deadlines[i]<end)end=s->call_deadlines[i];
            result=end>s->last_us?end-s->last_us:0;
        }
    }state_unlock();return result;
}
bool tls_scope_permitted(esp_tls_scope*s,uint32_t g,int64_t now){
    probe_permission(s,g);
    state_lock();bool ok=current(s,g);if(ok){tick(s,now);ok=!s->cancelled&&!s->close_taken;}state_unlock();return ok;
}
tls_scope_result tls_scope_creation_begin(tls_scope_io*t){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(valid_io(t)){esp_tls_scope*s=t->owner;r=TLS_SCOPE_REJECTED;
        if(!s->cancelled&&!s->creation_started&&!s->close_taken){s->creation_started=s->creation_active=true;s->creator_slot=t->slot;s->creator_serial=t->serial;r=TLS_SCOPE_OK;}
    }state_unlock();return r;
}
tls_scope_result tls_scope_publish_fd(tls_scope_io*t,int fd,int64_t now){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(valid_io(t)){esp_tls_scope*s=t->owner;r=TLS_SCOPE_REJECTED;
        if(fd>=0&&creator(t)&&s->fd<0&&!s->close_taken){s->fd=fd;tick(s,now);r=s->cancelled?TLS_SCOPE_REJECTED:TLS_SCOPE_OK;}
    }state_unlock();return r; /* Even a cancelled fresh fd is now accounted for. */
}
tls_scope_result tls_scope_creation_end(tls_scope_io*t){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(creator(t)&&!t->owner->connect_active){t->owner->creation_active=false;r=TLS_SCOPE_OK;}
    state_unlock();return r;
}
tls_scope_result tls_scope_connect_begin(tls_scope_io*t,int64_t now){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(valid_io(t)){esp_tls_scope*s=t->owner;tick(s,now);r=TLS_SCOPE_REJECTED;
        if(creator(t)&&s->fd>=0&&!s->cancelled&&!s->connect_started&&!s->close_taken){s->connect_started=s->connect_active=true;r=TLS_SCOPE_OK;}
    }state_unlock();return r;
}
tls_scope_result tls_scope_connect_end(tls_scope_io*t){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(creator(t)&&t->owner->connect_active){t->owner->connect_active=false;r=TLS_SCOPE_OK;}
    state_unlock();return r;
}
tls_scope_result tls_scope_cancel(esp_tls_scope*s,uint32_t g){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;if(current(s,g)){s->cancelled=true;r=TLS_SCOPE_OK;}state_unlock();return r;
}
tls_scope_result tls_scope_cancel_run(esp_tls_scope*s,uint32_t g,tls_scope_shutdown_fn fn,void*user){
    if(!fn)return TLS_SCOPE_REJECTED;
    int fd=-1;state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(current(s,g)){
        s->cancelled=true;
        if(s->close_confirmed)r=TLS_SCOPE_ALREADY;
        else if(s->close_taken)r=s->close_active?TLS_SCOPE_PENDING:TLS_SCOPE_FAILED;
        else if(s->shutdown_active||s->connect_active)r=TLS_SCOPE_PENDING;
        else if(s->shutdown_taken)r=s->shutdown_ok?TLS_SCOPE_ALREADY:TLS_SCOPE_FAILED;
        else if(s->fd<0)r=s->creation_active?TLS_SCOPE_PENDING:TLS_SCOPE_ALREADY;
        else{s->shutdown_taken=s->shutdown_active=true;fd=s->fd;r=TLS_SCOPE_OK;}
    }state_unlock();if(r!=TLS_SCOPE_OK)return r;
    const int result=fn(fd,user); // No lock held. A live borrow prevents close/reuse.
    state_lock();
    if(!current(s,g)||!s->shutdown_active||s->fd!=fd){if(current(s,g))fail(s,TLS_SCOPE_FAULT_INVARIANT);r=TLS_SCOPE_FAILED;}
    else{s->shutdown_active=false;s->shutdown_done=true;s->shutdown_ok=result==0;if(result!=0)fail(s,TLS_SCOPE_FAULT_SHUTDOWN);r=result==0?TLS_SCOPE_OK:TLS_SCOPE_FAILED;}
    state_unlock();return r;
}
tls_scope_result tls_scope_close_claim(esp_tls_scope*s,uint32_t g,int*fd){
    if(!fd)return TLS_SCOPE_REJECTED;
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(current(s,g)){s->cancelled=true;
        if(s->close_confirmed)r=TLS_SCOPE_ALREADY;
        else if(s->close_taken)r=s->close_active?TLS_SCOPE_PENDING:TLS_SCOPE_FAILED;
        else if(s->io_mask||s->creation_active||s->connect_active||s->shutdown_active)r=TLS_SCOPE_PENDING;
        else{s->close_taken=s->close_active=true;*fd=s->fd;r=TLS_SCOPE_OK;}
    }state_unlock();return r;
}
tls_scope_result tls_scope_close_finish(esp_tls_scope*s,uint32_t g,int fd,int result){
    state_lock();tls_scope_result r=TLS_SCOPE_STALE;
    if(current(s,g)&&s->close_active&&s->fd==fd){s->close_active=false;
        if(result==0){s->fd=-1;s->close_confirmed=true;r=TLS_SCOPE_OK;}
        else{fail(s,TLS_SCOPE_FAULT_CLOSE);r=TLS_SCOPE_FAILED;}
    }state_unlock();return r;
}
tls_scope_snapshot tls_scope_observe(esp_tls_scope*s,uint32_t g,int64_t now){
    tls_scope_snapshot out={0};state_lock();if(current(s,g)){
        tick(s,now);uint32_t count=0;for(unsigned i=0;i<32;++i)count+=(s->io_mask>>i)&1u;
        out=(tls_scope_snapshot){s->owned,s->cancelled,s->creation_active,s->connect_active,s->shutdown_active,s->shutdown_done,s->shutdown_ok,
            s->close_active,s->close_confirmed,s->last_us>=s->deadline_us,count,s->fd,s->fault,
            s->deadline_us>s->last_us?s->deadline_us-s->last_us:0};
    }state_unlock();return out;
}
bool tls_scope_release(esp_tls_scope*s,uint32_t g){
    state_lock();bool ok=current(s,g)&&s->close_confirmed&&!s->io_mask&&!s->creation_active&&!s->connect_active&&!s->shutdown_active&&!s->close_active;
    if(ok){
        s->owned=false;
    }
    state_unlock();
    return ok;
}
