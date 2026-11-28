// Deferred EM_ASYNC-style callbacks and the event-loop inversion.
//
// emscripten_async_call() queues a funcref index/argument pair here; the host
// pump runs every callback and rAF turn on a root fiber so a main-thread futex
// wait can park that stack and be resumed by weblox_atomic_pump().  Callbacks
// already on a fiber are serviced inline; mco_resume stays on the host stack.
#ifndef WEBLOX_DEFERRED_H
#define WEBLOX_DEFERRED_H

#include "host.h"

#include "wasm-rt-impl.h"

int weblox_deferred_init(void);  // idempotent

// Run one queued callback on the current stack inside a wasm_rt_try guard and
// with the caller's runtime state saved/restored.  On a wasm trap, *trap is
// set and 0 is returned: no longjmp escapes while a caller holds a mutex.
// Returns 1 when a callback ran, 0 when the queue was empty or trapped.
int weblox_deferred_service(w2c_WebClient* inst, wasm_rt_trap_t* trap);

// Run the queue and one scheduled main_loop_iteration.  Returns the number of
// units of work performed.  Intended to be called from main.c's loop.
int weblox_deferred_pump(w2c_WebClient* inst);

// A worker posted to the main thread's mailbox: drain it on the next pump.
void weblox_deferred_mailbox(void);

// Mailbox registry (implemented in mailbox.c, declared here because there is
// no separate public mailbox header).
int weblox_mailbox_init(void);
void weblox_mailbox_set_main(u32 id);
int weblox_mailbox_signal(u32 id);
void weblox_mailbox_cleanup(u32 id);

// thread.c defers _emscripten_thread_init/_tls_init until a host call knows
// the instance (the import itself runs during instantiate, when it doesn't).
void weblox_thread_maybe_init_main(w2c_WebClient* inst);

#endif
