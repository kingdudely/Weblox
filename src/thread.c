// Emscripten worker threads: one wasm2c instance per pthread, sharing g_mem.
//
// TLS/table/globals are per worker just as they are per Web Worker in the
// browser.  The only guest fields read are the pthread struct's stack top,
// stack size and "started" flag; all wasm work still goes through generated
// exports and imports.
#include "deferred.h"
#include "abort_debug.h"
#include "fibers.h"
#include "guest.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct worker {
  w2c_WebClient inst;
  u32 pthread_ptr;
  u32 start;
  u32 arg;
} worker;

static u32 read_guest_u32(u32 address) {
  unsigned char* p = guest_ptr(address, 4);
  u32 value = 0;
  if (p) memcpy(&value, p, 4);
  return value;
}

static u32 g_pending_main_ptr;
static int g_pending_main_init;

static void guarded_call(w2c_WebClient* inst, void (*fn)(w2c_WebClient*)) {
  weblox_runtime_state saved;
  weblox_runtime_capture(&saved);
  wasm_rt_trap_t trap = wasm_rt_impl_try();
  if (trap == WASM_RT_TRAP_NONE) {
    fn(inst);
  } else {
    fprintf(stderr, "[thread] wasm trap: %s\n", wasm_rt_strerror(trap));
  }
  weblox_runtime_restore(&saved);
}

static void thread_init_fn(w2c_WebClient* inst) {
  w2c_WebClient_0x5Femscripten_thread_init(inst, g_pending_main_ptr, 1u, 1u,
                                            1u, 262144u, 0u);
}

static void tls_init_fn(w2c_WebClient* inst) {
  w2c_WebClient_0x5Femscripten_tls_init(inst);
}

void weblox_thread_maybe_init_main(w2c_WebClient* inst) {
  if (!g_pending_main_init || !inst) return;
  g_pending_main_init = 0;
  guarded_call(inst, thread_init_fn);
  guarded_call(inst, tls_init_fn);
  fprintf(stderr, "[thread] main init deferred to instance=%p\n",
          (void*)inst);
}

static int SDLCALL worker_main(void* param) {
  worker* w = (worker*)param;
  weblox_fiber_set_instance(&w->inst);
  wasm_rt_init_thread();
  wasm2c_WebClient_instantiate(&w->inst, &g_env, &g_wasi);

  // Offsets are relative to the guest pthread struct passed to the import,
  // exactly as in v3-wasm2c/thread.c and cpp/thread_host.cpp.
  const u32 top = read_guest_u32(w->pthread_ptr + 48);
  const u32 bottom = read_guest_u32(w->pthread_ptr + 52);
  if (top && top > bottom) {
    w2c_WebClient_set_stack_limits(&w->inst, top, top - bottom);
    w2c_WebClient_set_stack_ptr(&w->inst, top);
  }
  guest_put_u32(w->pthread_ptr + 112, 1);  // pthread "started" flag
  fprintf(stderr, "[thread] worker ptr=%u start=%u stack=%x..%x\n",
          w->pthread_ptr, w->start, top, bottom);

  {
    weblox_runtime_state saved;
    weblox_runtime_capture(&saved);
    w2c_WebClient_0x5Femscripten_thread_init(&w->inst, w->pthread_ptr, 0u, 1u,
                                              1u, 0u, 0u);
    w2c_WebClient_0x5Femscripten_tls_init(&w->inst);
    weblox_runtime_restore(&saved);
  }

  wasm_rt_funcref_table_t* table = &w->inst.w2c_0x5F_indirect_function_table;
  if (w->start < table->size && table->data[w->start].func) {
    wasm_rt_funcref_t* entry = &table->data[w->start];
    weblox_runtime_state saved;
    weblox_runtime_capture(&saved);
    wasm_rt_trap_t trap = wasm_rt_impl_try();
    if (trap == WASM_RT_TRAP_NONE)
      ((void (*)(void*, u32))entry->func)(entry->module_instance, w->arg);
    else
      fprintf(stderr, "[thread] start trapped: %s\n",
              wasm_rt_strerror(trap));
    weblox_runtime_restore(&saved);
  } else {
    fprintf(stderr, "[thread] bad start index %u\n", w->start);
  }

  wasm2c_WebClient_free(&w->inst);
  wasm_rt_free_thread();
  free(w);
  return 0;
}

u32 w2c_env_0x5F_pthread_create_js(struct w2c_env* env, u32 pthread_ptr,
                                   u32 attr, u32 start, u32 arg) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5F_pthread_create_js", (u64)(uintptr_t)env, (u64)pthread_ptr, (u64)attr, (u64)start, (u64)arg);
  (void)env;
  (void)attr;
  worker* w = calloc(1, sizeof *w);
  if (!w) return 11;  // EAGAIN
  w->pthread_ptr = pthread_ptr;
  w->start = start;
  w->arg = arg;
  SDL_Thread* thread = SDL_CreateThread(worker_main, "weblox-worker", w);
  if (!thread) {
    free(w);
    return 11;
  }
  SDL_DetachThread(thread);
  fprintf(stderr, "[thread] spawn ptr=%u start=%u\n", pthread_ptr, start);
  return 0;
}

void w2c_env_0x5Femscripten_init_main_thread_js(struct w2c_env* env, u32 ptr) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Femscripten_init_main_thread_js", (u64)(uintptr_t)env, (u64)ptr);
  (void)env;
  weblox_mailbox_set_main(ptr);
  // The browser installs the main thread before the module's static
  // constructors run.  By the time this import fires wasm_call_ctors has a
  // live instance (host.cpp publishes it right after instantiate), so perform
  // thread_init/tls_init right here: TLS-backed globals are invalid until
  // then and the engine aborts inside initialize() otherwise.  Keep the
  // deferred path only for hosts that call the import from instantiate().
  if (weblox_instance) {
    extern void weblox_deferred_fill_funcrefs(w2c_WebClient*);
    weblox_deferred_fill_funcrefs(weblox_instance);
    g_pending_main_ptr = ptr;
    guarded_call(weblox_instance, thread_init_fn);
    guarded_call(weblox_instance, tls_init_fn);
    fprintf(stderr, "[thread] main init inline ptr=%u inst=%p\n", ptr,
            (void*)weblox_instance);
    return;
  }
  g_pending_main_ptr = ptr;
  g_pending_main_init = 1;
}

void w2c_env_0x5Femscripten_thread_cleanup(struct w2c_env* env, u32 ptr) {
  WEBLOX_IMPORT_TRACE_CALL("w2c_env_0x5Femscripten_thread_cleanup", (u64)(uintptr_t)env, (u64)ptr);
  (void)env;
  fprintf(stderr, "[thread] cleanup ptr=%u\n", ptr);
  weblox_mailbox_signal(ptr);
  weblox_mailbox_cleanup(ptr);
}
