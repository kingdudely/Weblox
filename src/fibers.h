// JSPI-style fibers for the clean C11 host.
//
// The switching backend is minicoro (single header, vendored in deps/minicoro).
// Only fibers.c includes minicoro.h and defines MINICORO_IMPL, so this header
// never leaks the coroutine representation to other translation units.
//
// WABT keeps the trap buffer, unwind target, active exception and call-depth
// counters in thread-local storage.  A suspended C stack must own a copy of
// all of them, otherwise a trap in one fibre longjmps into another fibre (or
// into the host).  weblox_runtime_capture/restore do exactly that and are the
// contract between this file and the wasm-calling code in deferred/atomics.
#ifndef WEBLOX_FIBERS_H
#define WEBLOX_FIBERS_H

#include "host.h"

#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

#include <stddef.h>
#include <string.h>

typedef struct weblox_runtime_state {
  wasm_rt_jmp_buf trap_buffer;
  WASM_RT_UNWIND_TARGET* unwind_target;
  wasm_rt_tag_t exception_tag;
  u32 exception_size;
  unsigned char exception_data[WASM_EXN_MAX_SIZE];
#if WASM_RT_STACK_DEPTH_COUNT
  u32 depth;
  u32 saved_depth;
#endif
} weblox_runtime_state;

void weblox_runtime_capture(weblox_runtime_state* state);
void weblox_runtime_restore(const weblox_runtime_state* state);

// Call once after wasm_rt_init(), before the first wasm call.  Idempotent.
// Also initialises the SDL event subsystem and the atomics/deferred/mailbox
// modules, so main.c only needs this one init call.
void weblox_fibers_init(void);

// Set up this (worker) thread's fibre registry.  The main thread id is shared
// with the main registry so a worker never mistakes itself for the UI thread.
void weblox_fiber_init_worker(w2c_WebClient* inst);

// Which instance the imports on the current thread belong to.  Worker threads
// set this in their own TLS before instantiating; the host uses weblox_instance.
void weblox_fiber_set_instance(w2c_WebClient* inst);
w2c_WebClient* weblox_fiber_instance(void);

// True on the thread that called weblox_fibers_init().  Main-thread waits pump
// SDL events and run deferred callbacks inline; workers just block.
int weblox_fiber_on_main_thread(void);

// True while no guest/root fiber owns this thread's stack, i.e. mco_resume is
// safe.  Fibres must only mco_yield, so a wake-up from inside a callback is
// queued and drained here instead of resuming a coroutine from a coroutine.
int weblox_fiber_on_host_stack(void);

// True when the running stack is a host root fibre (as opposed to the native
// host stack or a guest fibre).  Callbacks entered on a root must not nest
// another root: mco_resume is only legal from the host stack.
int weblox_fiber_on_root(void);

// Continuation key currently filed for the running fibre (0 when running).
u32 weblox_fiber_current_key(void);

// Opaque handle for the running fibre, used by the futex park/ready queue.
void* weblox_fiber_current_handle(void);

// Non-zero while at least one host root fibre is suspended at a futex/transfer.
// The pump uses this to avoid launching a new root while another is suspended.
int weblox_fiber_has_suspended_root(void);

// Run fn(arg) on a fresh root fibre.  Returns an opaque handle (NULL on
// allocation failure or when called from inside another fibre) that stays
// valid until weblox_root_free().  The call returns as soon as the root
// finishes or parks; use weblox_root_done() to tell the two apart and
// weblox_deferred_pump() to drive parked roots.
void* weblox_run_on_root(w2c_WebClient* inst, void (*fn)(void*), void* arg);
int weblox_root_done(void* handle);
void weblox_root_free(void* handle);

// Park the calling fibre and hand control to the host event loop.  The return
// value is the code installed by weblox_fiber_resume_parked().  Returns 0 with
// no switch when called from the native host stack (nothing to suspend).
u32 weblox_fiber_park(void);

// Resume a fibre parked by weblox_fiber_park() with a JS Atomics.wait result.
// Must be called from the host stack.  The fibre's runtime state and guest
// shadow-stack window are restored by the normal dispatch path.
void weblox_fiber_resume_parked(void* handle, u32 code);

#endif
