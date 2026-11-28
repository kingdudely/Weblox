// Deferred queue and event-loop inversion.
//
// emscripten_async_call() and a worker's mailbox notification both file work
// here; callbacks are invoked through the instance's funcref table, never by
// hardcoded index.  A futex wait calls weblox_deferred_service() directly so
// the callback runs inline on the waiting fiber.
#include "deferred.h"
#include "abort_debug.h"
#include "fibers.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define DEFERRED_LIMIT 4096
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

int weblox_deferred_pump(w2c_WebClient* inst) {
  if (!inst) inst = weblox_fiber_instance();
  if (!inst) return 0;
  weblox_fiber_set_instance(inst);
  weblox_thread_maybe_init_main(inst);
  if (weblox_fiber_on_main_thread()) SDL_PumpEvents();

  int ran = 0;
  wasm_rt_trap_t trap = WASM_RT_TRAP_NONE;
  // A callback may legitimately queue another one (or re-arm a futex).  Bound
  // the drain so a self-sustaining chain cannot spin the host forever.
  for (int i = 0; i < 32; ++i) {
    if (!weblox_deferred_service(inst, &trap)) break;
    ++ran;
    if (trap != WASM_RT_TRAP_NONE) break;
  }

  if (g_scheduled && g_turns_ready) {
    weblox_runtime_state saved;
    weblox_runtime_capture(&saved);
    wasm_rt_trap_t caught = wasm_rt_impl_try();
    if (caught == WASM_RT_TRAP_NONE) {
      w2c_WebClient_main_loop_iteration(inst);
      ++ran;
    } else {
      fprintf(stderr, "[def] main_loop_iteration trapped: %s\n",
              wasm_rt_strerror(caught));
    }
    weblox_runtime_restore(&saved);
  }
  return ran;
}
