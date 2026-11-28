// Deferred queue and event-loop inversion.
//
// emscripten_async_call() and a worker's mailbox notification both file work
// here; callbacks are invoked through the instance's funcref table, never by
// hardcoded index.  The host pump runs every callback and rAF turn on a root
// fibre: a main-thread futex wait then parks that fibre and lets the host keep
// turning the event loop until weblox_atomic_pump() resumes it.  A callback
// already on a fibre is serviced inline, since mco_resume is only legal from
// the native host stack.
#include "deferred.h"
#include "abort_debug.h"
#include "atomics.h"
#include "fibers.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFERRED_LIMIT 4096
#define PUMP_LIMIT 32
#define LIVE_ROOT_LIMIT 64
#define MAILBOX_FN 0xFFFFFFFFu

typedef struct deferred_item {
  u32 fn;
  u32 arg;
} deferred_item;

static deferred_item g_queue[DEFERRED_LIMIT];
static unsigned g_head, g_tail;
static SDL_Mutex* g_lock;
// Browsers call rAF every frame regardless of schedule_main_loop.  Keep the
// flag set so the guest always gets a turn.
static volatile int g_scheduled = 1;
// The browser only starts rAF turns after the page has configured the client.
// Running main_loop_iteration re-entrantly from a mailbox await inside
// wasm_call_ctors hits abort_js because the engine is not initialised yet.
static volatile int g_turns_ready;

typedef enum {
  ROOT_CALLBACK = 0,
  ROOT_TURN = 1
} root_kind;

// A callback that parks at a futex wait is not done with its C stack.  Keep
// its root handle (and the context passed to the root body) alive until the
// root finishes; reap them on every pump.
typedef struct {
  void* handle;
  void* arg;
  root_kind kind;
} live_root;

static live_root g_live_roots[LIVE_ROOT_LIMIT];

void weblox_deferred_enable_turns(void) { g_turns_ready = 1; }

int weblox_deferred_turns_enabled(void) { return g_turns_ready != 0; }

// Emscripten leaves holes in the indirect function table for lazily
// initialised functions.  The proven C++ host fills them with the first
// populated funcref before the static constructors run; otherwise an early
// call_indirect traps in initialize().
void weblox_deferred_fill_funcrefs(w2c_WebClient* inst) {
  if (!inst) return;
  wasm_rt_funcref_table_t* table = &inst->w2c_0x5F_indirect_function_table;
  if (!table->data || table->size <= 1 || !table->data[1].func) return;
  const wasm_rt_funcref_t fill = table->data[1];
  for (u64 i = 0; i < table->size; ++i)
    if (!table->data[i].func) table->data[i] = fill;
}

int weblox_deferred_init(void) {
  if (!g_lock) g_lock = SDL_CreateMutex();
  return g_lock != NULL;
}

static void queue_push(u32 fn, u32 arg) {
  if (!weblox_deferred_init()) return;
  SDL_LockMutex(g_lock);
  unsigned next = (g_tail + 1) % DEFERRED_LIMIT;
  if (next != g_head) {
    g_queue[g_tail].fn = fn;
    g_queue[g_tail].arg = arg;
    g_tail = next;
  } else {
    fprintf(stderr, "[def] queue full, dropping fn=%u\n", fn);
  }
  SDL_UnlockMutex(g_lock);
}

// Peek only for logging: the pump is the sole consumer, so the head is stable
// while it decides which root to launch.
static int queue_peek(deferred_item* item) {
  if (!g_lock) return 0;
  SDL_LockMutex(g_lock);
  int have = g_head != g_tail;
  if (have && item) *item = g_queue[g_head];
  SDL_UnlockMutex(g_lock);
  return have;
}

void w2c_env_emscripten_async_call(struct w2c_env* env, u32 fn, u32 arg,
                                   u32 timeout_ms) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_emscripten_async_call", (u64)(uintptr_t)env, (u64)fn, (u64)arg, (u64)timeout_ms);
  (void)env;
  (void)timeout_ms;
  queue_push(fn, arg);
}

void w2c_env_schedule_main_loop(struct w2c_env* env) {
  WEBLOX_IMPORT_TRACE_CALL("w2c_env_schedule_main_loop", (u64)(uintptr_t)env);
  (void)env;
  g_scheduled = 1;
}

void weblox_deferred_mailbox(void) { queue_push(MAILBOX_FN, 0); }

int weblox_deferred_service(w2c_WebClient* inst, wasm_rt_trap_t* trap) {
  if (!inst) return 0;
  if (!weblox_deferred_init()) return 0;
  if (trap) *trap = WASM_RT_TRAP_NONE;

  deferred_item item;
  SDL_LockMutex(g_lock);
  if (g_head == g_tail) {
    SDL_UnlockMutex(g_lock);
    return 0;
  }
  item = g_queue[g_head];
  g_head = (g_head + 1) % DEFERRED_LIMIT;
  SDL_UnlockMutex(g_lock);

  weblox_runtime_state saved;
  weblox_runtime_capture(&saved);
  wasm_rt_trap_t caught = wasm_rt_impl_try();
  if (caught == WASM_RT_TRAP_NONE) {
    if (item.fn == MAILBOX_FN) {
      w2c_WebClient_0x5Femscripten_check_mailbox(inst);
    } else if (item.fn < inst->w2c_0x5F_indirect_function_table.size) {
      wasm_rt_funcref_t* entry =
          &inst->w2c_0x5F_indirect_function_table.data[item.fn];
      if (entry->func)
        ((void (*)(void*, u32))entry->func)(entry->module_instance, item.arg);
    } else {
      fprintf(stderr, "[def] bad funcref index %u\n", item.fn);
    }
  }
  weblox_runtime_restore(&saved);
  if (caught != WASM_RT_TRAP_NONE) {
    fprintf(stderr, "[def] callback trapped: %s\n",
            wasm_rt_strerror(caught));
    if (trap) *trap = caught;
    return 0;
  }
  return 1;
}

// Root body for one queued callback.  The context is heap allocated by the
// pump so it stays valid if this callback parks and resumes on a later turn.
typedef struct {
  w2c_WebClient* inst;
  u32 fn;
} callback_ctx;

static void callback_root(void* arg) {
  callback_ctx* ctx = (callback_ctx*)arg;
  fprintf(stderr, "[def] root fn=%u\n", ctx->fn);
  wasm_rt_trap_t trap = WASM_RT_TRAP_NONE;
  if (!weblox_deferred_service(ctx->inst, &trap) &&
      trap != WASM_RT_TRAP_NONE)
    fprintf(stderr, "[def] root fn=%u trapped: %s\n", ctx->fn,
            wasm_rt_strerror(trap));
}

// Root body for the single rAF turn.  root_body() owns the outer WABT guard;
// this inner guard reports the trap without letting a longjmp escape while a
// host lock is held, and the saved runtime state restores the root's guard.
typedef struct {
  w2c_WebClient* inst;
} turn_ctx;

static void turn_root(void* arg) {
  turn_ctx* ctx = (turn_ctx*)arg;
  fprintf(stderr, "[def] root fn=main_loop_iteration\n");
  weblox_runtime_state saved;
  weblox_runtime_capture(&saved);
  wasm_rt_trap_t caught = wasm_rt_impl_try();
  if (caught == WASM_RT_TRAP_NONE) {
    w2c_WebClient_main_loop_iteration(ctx->inst);
  } else {
    fprintf(stderr, "[def] main_loop_iteration trapped: %s\n",
            wasm_rt_strerror(caught));
  }
  weblox_runtime_restore(&saved);
}

// True when this stack already belongs to a fibre.  Guest/root fibres may only
// mco_yield; starting a root from one would resume a coroutine from a
// coroutine and abandon the caller's stack.
static int on_root(void) {
  return weblox_fiber_on_root() || !weblox_fiber_on_host_stack();
}

static int live_root_count(void) {
  int count = 0;
  for (int i = 0; i < LIVE_ROOT_LIMIT; ++i)
    if (g_live_roots[i].handle) ++count;
  return count;
}

static void reap_roots(void) {
  for (int i = 0; i < LIVE_ROOT_LIMIT; ++i) {
    live_root* slot = &g_live_roots[i];
    if (!slot->handle || !weblox_root_done(slot->handle)) continue;
    fprintf(stderr, "[def] root done kind=%d\n", (int)slot->kind);
    weblox_root_free(slot->handle);
    free(slot->arg);
    memset(slot, 0, sizeof *slot);
  }
}

// Take ownership of arg whether or not the root could be started.  Returns 1
// when a unit of guest work was started, 0 when the queue/registry was full or
// a root could not be allocated.
static int launch_root(w2c_WebClient* inst, void (*fn)(void*), void* arg,
                       root_kind kind) {
  if (!arg) return 0;
  if (!weblox_fiber_on_host_stack()) {
    fprintf(stderr, "[def] root skipped: already on fibre\n");
    free(arg);
    return 0;
  }
  int live = live_root_count();
  if (live >= LIVE_ROOT_LIMIT) {
    fprintf(stderr, "[def] root saturated live=%d limit=%d\n", live,
            LIVE_ROOT_LIMIT);
    free(arg);
    return 0;
  }
  void* handle = weblox_run_on_root(inst, fn, arg);
  if (!handle) {
    fprintf(stderr, "[def] root create failed\n");
    free(arg);
    return 0;
  }
  if (weblox_root_done(handle)) {
    weblox_root_free(handle);
    free(arg);
    return 1;
  }
  // The root parked: keep its stack and argument alive until it is resumed
  // and finishes.  Capacity was checked above, so a free slot exists.
  for (int i = 0; i < LIVE_ROOT_LIMIT; ++i) {
    if (g_live_roots[i].handle) continue;
    g_live_roots[i].handle = handle;
    g_live_roots[i].arg = arg;
    g_live_roots[i].kind = kind;
    return 1;
  }
  fprintf(stderr, "[def] root registry saturated; handle leaked alive\n");
  return 1;
}

int weblox_deferred_pump(w2c_WebClient* inst) {
  if (!inst) inst = weblox_fiber_instance();
  if (!inst) return 0;
  weblox_fiber_set_instance(inst);
  weblox_thread_maybe_init_main(inst);
  if (weblox_fiber_on_main_thread()) SDL_PumpEvents();

  // A main-thread futex wait parks its root instead of blocking this loop.
  // Resume any woken/expired waiters before running the next engine turn.
  weblox_atomic_pump();
  reap_roots();

  int ran = 0;
  // A callback may legitimately queue another one (or re-arm a futex).  Bound
  // the drain so a self-sustaining chain cannot spin the host forever.
  for (int i = 0; i < PUMP_LIMIT; ++i) {
    deferred_item item;
    if (!queue_peek(&item)) break;
    if (on_root()) {
      // Nested pump from a callback/mailbox await: service inline on the
      // current (parkable) root instead of nesting mco_resume.
      fprintf(stderr, "[def] root fn=%u inline\n", item.fn);
      wasm_rt_trap_t trap = WASM_RT_TRAP_NONE;
      if (!weblox_deferred_service(inst, &trap)) break;
      ++ran;
      if (trap != WASM_RT_TRAP_NONE) break;
    } else {
      callback_ctx* ctx = calloc(1, sizeof *ctx);
      if (!ctx) break;
      ctx->inst = inst;
      ctx->fn = item.fn;
      if (!launch_root(inst, callback_root, ctx, ROOT_CALLBACK)) break;
      ++ran;
    }
  }

  if (g_scheduled && g_turns_ready) {
    if (on_root()) {
      // A nested pump must not re-enter an engine turn while its caller is
      // suspended inside one; the outer turn will schedule the next one.
    } else {
      turn_ctx* ctx = calloc(1, sizeof *ctx);
      if (ctx) {
        ctx->inst = inst;
        if (launch_root(inst, turn_root, ctx, ROOT_TURN)) ++ran;
      }
    }
  }

  // A resumed root may have made another waiter ready.  Hand it a turn now
  // rather than waiting a whole frame, still on the host stack only.
  weblox_atomic_pump();
  reap_roots();
  return ran;
}
