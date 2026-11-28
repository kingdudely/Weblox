// JSPI-style suspended fibers for the clean C11 host.
//
// minicoro provides the machine-stack switching.  This file owns everything
// wasm-specific about a switch: the instance's shadow-stack window and the
// WABT thread-local trap buffer / unwind target / active exception / depth.
// All resumes happen from the host stack; a fiber only ever yields back to
// the host scheduler, so minicoro never has to resume a NORMAL coroutine.
//
// Each pending resumption key belongs to one fiber record, exactly like the
// `NativeFiber` map in the Windows-host reference.  A transfer consumes the
// target's key and files the caller's key in its place.
#define MINICORO_IMPL
#include "minicoro.h"

#include "fibers.h"
#include "atomics.h"
#include "deferred.h"
#include "guest.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define FIBER_LIMIT 1024
#define GUEST_STACK_SIZE (32u * 1024u * 1024u)
#define ROOT_STACK_SIZE (8u * 1024u * 1024u)

typedef enum {
  FSTATE_FREE = 0,
  FSTATE_RUNNING,
  FSTATE_WAIT_CREATE,
  FSTATE_WAIT_TRANSFER,
  FSTATE_PARKED, /* parked by an async futex wait, not a fibre transfer */
  FSTATE_DEAD
} fstate;

typedef struct weblox_fiber {
  mco_coro* co;                 /* NULL for the host-stack pseudo fiber */
  w2c_WebClient* inst;
  u32 key;                      /* resumption key while suspended */
  u32 result;                   /* value delivered by whoever resumes us */
  fstate state;
  int used;
  int root;                     /* host root (1) vs guest fiber (0) */
  int dead;                     /* guest fiber body finished */
  int done;
  int threw;
  wasm_rt_trap_t trap;          /* trap to re-raise when this fiber resumes */
  struct {
    wasm_rt_tag_t tag;
    u32 size;
    unsigned char data[WASM_EXN_MAX_SIZE];
  } failed_exception;

  u32 create_resumption;        /* key of the creator of a guest fiber */
  struct weblox_fiber* creator_fiber; /* stack that called create (may be host) */
  fstate prev_state;            /* state before a futex park */
  u32 prev_key;                 /* continuation before a futex park */
  u32 park_key;                 /* synthetic id used in park logs */
  u32 entry, arg;               /* fiber_entry arguments */
  void (*fn)(void*);            /* root body */
  void* fn_arg;

  struct {
    u32 sp, base, end;
  } guest;
  weblox_runtime_state rt;
  struct weblox_fiber* next;
} weblox_fiber;

typedef struct fiber_thread {
  weblox_fiber host;            /* represents the native host stack */
  weblox_fiber* current;        /* NULL-safe: always set to host on host stack */
  weblox_fiber* next;           /* fiber chosen by the currently yielding one */
  weblox_fiber* list;           /* this thread's whole continuation registry */
  weblox_fiber* root;           /* root this thread is currently driving */
  w2c_WebClient* inst;
  SDL_ThreadID main_thread;
  int initialized;
  int root_mode;
  u32 park_seq;
} fiber_thread;

static _Thread_local fiber_thread g_ft;
// Set once by the main thread; worker TLS copies it so on_main_thread() stays
// false for them even when they have to initialise their own fibre registry.
static SDL_ThreadID g_process_main_thread;
static int g_process_main_set;

static int verbose(void) {
  static int cached = -1;
  if (cached < 0) cached = getenv("WC_FIB") ? 1 : 0;
  return cached;
}

void weblox_runtime_capture(weblox_runtime_state* state) {
  if (!state) return;
  memcpy(&state->trap_buffer, &g_wasm_rt_jmp_buf, sizeof state->trap_buffer);
  state->unwind_target = wasm_rt_get_unwind_target();
  state->exception_tag = wasm_rt_exception_tag();
  state->exception_size = wasm_rt_exception_size();
  if (state->exception_size > WASM_EXN_MAX_SIZE)
    state->exception_size = WASM_EXN_MAX_SIZE;
  if (state->exception_size)
    memcpy(state->exception_data, wasm_rt_exception(), state->exception_size);
#if WASM_RT_STACK_DEPTH_COUNT
  state->depth = wasm_rt_call_stack_depth;
  state->saved_depth = wasm_rt_saved_call_stack_depth;
#endif
}

void weblox_runtime_restore(const weblox_runtime_state* state) {
  if (!state) return;
  memcpy(&g_wasm_rt_jmp_buf, &state->trap_buffer, sizeof g_wasm_rt_jmp_buf);
  wasm_rt_set_unwind_target(state->unwind_target);
  wasm_rt_load_exception(state->exception_tag, state->exception_size,
                         state->exception_size ? state->exception_data : NULL);
#if WASM_RT_STACK_DEPTH_COUNT
  wasm_rt_call_stack_depth = state->depth;
  wasm_rt_saved_call_stack_depth = state->saved_depth;
#endif
}

static weblox_runtime_state fresh_runtime(void) {
  weblox_runtime_state state;
  memset(&state, 0, sizeof state);
  state.unwind_target = NULL;
#if WASM_RT_STACK_DEPTH_COUNT
  state.depth = 0;
  state.saved_depth = 0;
#endif
  return state;
}

static void capture_guest(weblox_fiber* fiber) {
  if (!fiber || !fiber->inst) return;
  fiber->guest.sp = w2c_WebClient_get_stack_ptr(fiber->inst);
  fiber->guest.base = w2c_WebClient_get_stack_base(fiber->inst);
  fiber->guest.end = w2c_WebClient_get_stack_end(fiber->inst);
}

static void restore_guest(const weblox_fiber* fiber) {
  if (!fiber || !fiber->inst) return;
  // The host stack has no saved window until it leaves for a fibre.  Before
  // that the fresh instance's own shadow-stack limits must be left alone.
  if (!fiber->guest.base && !fiber->guest.end) return;
  w2c_WebClient_set_stack_limits(fiber->inst, fiber->guest.base,
                                fiber->guest.end);
  w2c_WebClient_set_stack_ptr(fiber->inst, fiber->guest.sp);
}

// Keys live in this thread's registry only: a worker must never resolve a
// continuation created by the main thread (or by another worker).
static weblox_fiber* lookup(u32 key) {
  if (!key) return NULL;
  for (weblox_fiber* f = g_ft.list; f; f = f->next)
    if (f->used && f->state != FSTATE_RUNNING && f->state != FSTATE_FREE &&
        f->state != FSTATE_PARKED && f->key == key &&
        (!f->inst || !g_ft.inst || f->inst == g_ft.inst))
      return f;
  return NULL;
}

static void unlink_fiber(weblox_fiber* fiber) {
  weblox_fiber** link = &g_ft.list;
  while (*link && *link != fiber) link = &(*link)->next;
  if (*link) *link = fiber->next;
}

static weblox_fiber* alloc_fiber(void) {
  int count = 0;
  for (weblox_fiber* f = g_ft.list; f; f = f->next) ++count;
  if (count >= FIBER_LIMIT) return NULL;
  weblox_fiber* fiber = calloc(1, sizeof *fiber);
  if (!fiber) return NULL;
  fiber->next = g_ft.list;
  g_ft.list = fiber;
  fiber->used = 1;
  fiber->state = FSTATE_RUNNING;
  fiber->rt = fresh_runtime();
  return fiber;
}

static void install_fiber(weblox_fiber* fiber) {
  weblox_runtime_restore(&fiber->rt);
  restore_guest(fiber);
  g_ft.current = fiber;
}

static void install_host(void) {
  weblox_runtime_restore(&g_ft.host.rt);
  restore_guest(&g_ft.host);
  g_ft.current = &g_ft.host;
}

// The native host stack is the scheduler.  Resume fiber after fiber until
// control comes back to the host (a root finished or parked with no work).
static void host_dispatch(weblox_fiber* first) {
  weblox_fiber* next = first;
  while (next && next != &g_ft.host) {
    weblox_fiber* fiber = next;
    next = NULL;
    weblox_runtime_capture(&g_ft.host.rt);
    capture_guest(&g_ft.host);
    install_fiber(fiber);
    mco_result res = mco_resume(fiber->co);
    if (res != MCO_SUCCESS)
      fprintf(stderr, "[fib] resume failed: %s\n",
              mco_result_description(res));
    next = g_ft.next;
    g_ft.next = NULL;
    install_host();
  }
}

// File the current continuation under `key`, hand `incoming` to `target`,
// and wait for somebody to transfer back.  The value returned is `result`
// installed by that resumer.
static u32 switch_away(weblox_fiber* target, u32 key, fstate state,
                       u32 incoming) {
  weblox_fiber* self = g_ft.current ? g_ft.current : &g_ft.host;
  if (target && (target->done || target->dead || target == self)) {
    fprintf(stderr, "[fib] stale resume target, falling back to host\n");
    target = &g_ft.host;
  }
  if (target && target->co && mco_status(target->co) != MCO_SUSPENDED) {
    fprintf(stderr, "[fib] resume target is not suspended\n");
    target = &g_ft.host;
  }
  weblox_runtime_capture(&self->rt);
  capture_guest(self);
  self->key = key;
  self->state = state;
  if (verbose())
    fprintf(stderr, "[fib] switch key=%u -> key=%u state=%d co=%d\n", key,
            target ? target->key : 0, (int)state, self->co ? 1 : 0);
  if (target) {
    target->result = incoming;
    target->state = FSTATE_RUNNING;
    target->key = 0;
  }
  if (self->co) {
    g_ft.next = target;
    mco_yield(self->co);
  } else {
    host_dispatch(target);
  }
  // host_dispatch leaves the host state installed on the host stack and the
  // resumed fiber's state installed when mco_yield returns inside a fiber.
  if (self->trap != WASM_RT_TRAP_NONE) {
    wasm_rt_trap_t trap = self->trap;
    self->trap = WASM_RT_TRAP_NONE;
    if (trap == WASM_RT_TRAP_UNCAUGHT_EXCEPTION)
      wasm_rt_load_exception(self->failed_exception.tag,
                             self->failed_exception.size,
                             self->failed_exception.data);
    wasm_rt_trap(trap);
  }
  return self->result;
}

// Suspend the calling fibre at an async futex wait.  Unlike a fibre transfer
// there is no destination to resume now: we hand control straight back to the
// host event loop, which keeps turning rAF while the guest stack stays parked.
// The fibre's transfer continuation is preserved so a later transfer can still
// find it once the wait has been resumed.
u32 weblox_fiber_park(void) {
  weblox_fiber* self = g_ft.current ? g_ft.current : &g_ft.host;
  if (!self->co) return 0;  // native host stack: nothing to suspend
  weblox_runtime_capture(&self->rt);
  capture_guest(self);
  self->prev_state = self->state;
  self->prev_key = self->key;
  self->state = FSTATE_PARKED;
  self->key = 0;
  self->result = 0;
  self->park_key = 0xF0000000u | ++g_ft.park_seq;
  fprintf(stderr, "[fib] park key=%u prev=%u tid=%llu\n", self->park_key,
          self->prev_key, (unsigned long long)SDL_GetThreadID(NULL));
  g_ft.next = &g_ft.host;
  mco_yield(self->co);
  fprintf(stderr, "[fib] resume key=%u code=%u\n", self->park_key,
          self->result);
  return self->result;
}

void weblox_fiber_resume_parked(void* handle, u32 code) {
  weblox_fiber* fiber = (weblox_fiber*)handle;
  if (!fiber || !fiber->co || fiber->state != FSTATE_PARKED) return;
  if (!weblox_fiber_on_host_stack()) {
    fprintf(stderr, "[fib] deferred park resume key=%u (called from fibre)\n",
            fiber->park_key);
    return;
  }
  fprintf(stderr, "[fib] resume park key=%u code=%u root=%d\n",
          fiber->park_key, code, fiber->root ? 1 : 0);
  fiber->result = code;
  fiber->state = fiber->prev_state;
  fiber->key = fiber->prev_key;
  host_dispatch(fiber);
}

static void report_trap(weblox_fiber* fiber, wasm_rt_trap_t trap) {
  if (trap == WASM_RT_TRAP_NONE) return;
  fiber->trap = trap;
  fiber->threw = 1;
  if (trap == WASM_RT_TRAP_UNCAUGHT_EXCEPTION) {
    u32 size = wasm_rt_exception_size();
    if (size > WASM_EXN_MAX_SIZE) size = WASM_EXN_MAX_SIZE;
    fiber->failed_exception.tag = wasm_rt_exception_tag();
    fiber->failed_exception.size = size;
    if (size)
      memcpy(fiber->failed_exception.data, wasm_rt_exception(), size);
  }
  fprintf(stderr, "[fib] wasm trap %s (%d)\n", wasm_rt_strerror(trap),
          (int)trap);
}

static void guest_body(mco_coro* co) {
  weblox_fiber* fiber = (weblox_fiber*)co->user_data;
  g_ft.current = fiber;
  wasm_rt_set_unwind_target(NULL);
  wasm_rt_load_exception(NULL, 0, NULL);
#if WASM_RT_STACK_DEPTH_COUNT
  wasm_rt_call_stack_depth = 0;
  wasm_rt_saved_call_stack_depth = 0;
#endif
  restore_guest(fiber);
  if (verbose())
    fprintf(stderr, "[fib] entry key=%u fn=%u arg=%u\n",
            fiber->create_resumption, fiber->entry, fiber->arg);
  wasm_rt_trap_t trap = wasm_rt_impl_try();
  if (trap == WASM_RT_TRAP_NONE) {
    w2c_WebClient_fiber_entry(fiber->inst, fiber->create_resumption,
                              fiber->entry, fiber->arg);
    fputs("[fib] fiber returned without transferring control\n", stderr);
    trap = WASM_RT_TRAP_UNREACHABLE;
  }
  report_trap(fiber, trap);
  fiber->dead = 1;
  fiber->state = FSTATE_DEAD;

  // The creator's key is the return path; deliver the trap (if any) to its
  // guard when that continuation resumes.
  weblox_fiber* creator = fiber->creator_fiber;
  if (creator) report_trap(creator, trap);
  switch_away(creator, 0, FSTATE_DEAD, trap == WASM_RT_TRAP_NONE ? 0u : 1u);
  // A destroyed fiber is never resumed again.
  fputs("[fib] guest body fell through\n", stderr);
}

static void root_body(mco_coro* co) {
  weblox_fiber* fiber = (weblox_fiber*)co->user_data;
  g_ft.current = fiber;
  wasm_rt_set_unwind_target(NULL);
  wasm_rt_load_exception(NULL, 0, NULL);
#if WASM_RT_STACK_DEPTH_COUNT
  wasm_rt_call_stack_depth = 0;
  wasm_rt_saved_call_stack_depth = 0;
#endif
  restore_guest(fiber);
  wasm_rt_trap_t trap = wasm_rt_impl_try();
  if (trap == WASM_RT_TRAP_NONE) {
    if (fiber->fn) fiber->fn(fiber->fn_arg);
  } else {
    report_trap(fiber, trap);
  }
  fiber->done = 1;
  if (verbose())
    fprintf(stderr, "[fib] root done threw=%d\n", fiber->threw ? 1 : 0);
  // Hand control back to the host stack; do not save this finished stack.
  g_ft.next = &g_ft.host;
  mco_yield(co);
}

static void init_thread(w2c_WebClient* inst, SDL_ThreadID main_tid) {
  memset(&g_ft, 0, sizeof g_ft);
  g_ft.initialized = 1;
  g_ft.current = &g_ft.host;
  g_ft.main_thread = main_tid;
  g_ft.inst = inst;
  // The host stack is a first-class continuation in the registry: the engine
  // creates a fibre by filing a key for its creator and transferring to it.
  // On a worker the creator is this native stack unless we run the entry on a
  // root fibre, so leaving the host out of the registry lost that key.
  g_ft.host.inst = inst;
  g_ft.host.used = 1;
  g_ft.host.state = FSTATE_RUNNING;
  g_ft.host.rt = fresh_runtime();
  g_ft.list = &g_ft.host;
  if (!SDL_WasInit(SDL_INIT_EVENTS)) SDL_InitSubSystem(SDL_INIT_EVENTS);
  weblox_atomic_init();
  weblox_deferred_init();
  weblox_mailbox_init();
  fprintf(stderr, "[fib] init main_tid=%llu tid=%llu\n",
          (unsigned long long)main_tid,
          (unsigned long long)SDL_GetThreadID(NULL));
}

void weblox_fibers_init(void) {
  if (g_ft.initialized) return;
  SDL_ThreadID tid = SDL_GetThreadID(NULL);
  if (!g_process_main_set) {
    g_process_main_thread = tid;
    g_process_main_set = 1;
  }
  init_thread(weblox_instance, tid);
}

void weblox_fiber_init_worker(w2c_WebClient* inst) {
  if (g_ft.initialized) {
    weblox_fiber_set_instance(inst);
    return;
  }
  init_thread(inst,
              g_process_main_set ? g_process_main_thread : SDL_GetThreadID(NULL));
}

void weblox_fiber_set_instance(w2c_WebClient* inst) {
  g_ft.inst = inst;
  g_ft.host.inst = inst;
}

w2c_WebClient* weblox_fiber_instance(void) {
  return g_ft.inst ? g_ft.inst : weblox_instance;
}

int weblox_fiber_on_main_thread(void) {
  return g_ft.initialized && SDL_GetThreadID(NULL) == g_ft.main_thread;
}

int weblox_fiber_on_host_stack(void) {
  return !g_ft.initialized || !g_ft.current || g_ft.current == &g_ft.host;
}

int weblox_fiber_on_root(void) {
  return g_ft.initialized && g_ft.current && g_ft.current != &g_ft.host &&
         g_ft.current->root;
}

u32 weblox_fiber_current_key(void) {
  return g_ft.current ? g_ft.current->key : 0;
}

void* weblox_fiber_current_handle(void) {
  return g_ft.current ? (void*)g_ft.current : (void*)&g_ft.host;
}

int weblox_fiber_has_suspended_root(void) {
  for (weblox_fiber* f = g_ft.list; f; f = f->next)
    if (f->root && f->used && !f->done && f->state != FSTATE_RUNNING) return 1;
  return 0;
}

void* weblox_run_on_root(w2c_WebClient* inst, void (*fn)(void*), void* arg) {
  // A coroutine may only yield; mco_resume from a fibre would switch stacks
  // from a suspended stack and corrupt both.  Nested callbacks run inline on
  // the root they are already on instead (see weblox_deferred_pump()).
  if (!weblox_fiber_on_host_stack()) {
    fprintf(stderr, "[fib] nested root refused on_root=%d\n",
            weblox_fiber_on_root());
    return NULL;
  }
  if (!inst) inst = weblox_fiber_instance();
  if (!inst) return NULL;
  if (!g_ft.initialized) weblox_fibers_init();
  g_ft.root_mode = 1;
  weblox_fiber_set_instance(inst);
  weblox_thread_maybe_init_main(inst);

  weblox_fiber* root = alloc_fiber();
  if (!root) return NULL;
  root->root = 1;
  root->inst = inst;
  g_ft.root = root;
  root->fn = fn;
  root->fn_arg = arg;
  root->guest.sp = w2c_WebClient_get_stack_ptr(inst);
  root->guest.base = w2c_WebClient_get_stack_base(inst);
  root->guest.end = w2c_WebClient_get_stack_end(inst);
  mco_desc desc = mco_desc_init(root_body, ROOT_STACK_SIZE);
  desc.user_data = root;
  if (mco_create(&root->co, &desc) != MCO_SUCCESS) {
    unlink_fiber(root);
    free(root);
    return NULL;
  }
  host_dispatch(root);  // captures host state itself, then restores it
  return root;
}

int weblox_root_done(void* handle) {
  weblox_fiber* root = (weblox_fiber*)handle;
  return !root || root->done ? 1 : 0;
}

void weblox_root_free(void* handle) {
  weblox_fiber* root = (weblox_fiber*)handle;
  if (!root) return;
  unlink_fiber(root);
  if (root->co) {
    mco_destroy(root->co);
    root->co = NULL;
  }
  if (g_ft.root == root) g_ft.root = NULL;
  root->used = 0;
  free(root);
}

u32 w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js(
    struct w2c_env* env, u32 h, u32 sp, u32 limit, u32 fn, u32 arg) {
  (void)env;
  if (!g_ft.initialized) weblox_fibers_init();
  if (!h || lookup(h)) {
    fprintf(stderr, "[fib] invalid creator continuation %u\n", h);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  if (sp <= limit || sp > weblox_mem_size()) {
    fprintf(stderr, "[fib] bad guest stack window sp=%u limit=%u\n", sp,
            limit);
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  }
  weblox_fiber* fiber = alloc_fiber();
  if (!fiber) {
    fputs("[fib] fiber table full\n", stderr);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  fiber->inst = weblox_fiber_instance();
  fiber->creator_fiber = g_ft.current ? g_ft.current : &g_ft.host;
  fiber->create_resumption = h;
  fiber->entry = fn;
  fiber->arg = arg;
  fiber->guest.sp = sp;
  fiber->guest.base = sp;
  fiber->guest.end = limit;

  mco_desc desc = mco_desc_init(guest_body, GUEST_STACK_SIZE);
  desc.user_data = fiber;
  if (mco_create(&fiber->co, &desc) != MCO_SUCCESS) {
    unlink_fiber(fiber);
    free(fiber);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  if (verbose())
    fprintf(stderr, "[fib] create key=%u sp=%u limit=%u fn=%u\n", h, sp,
            limit, fn);

  u32 result = switch_away(fiber, h, FSTATE_WAIT_CREATE, 0);
  if (fiber->dead) {
    if (fiber->co) {
      mco_destroy(fiber->co);
      fiber->co = NULL;
    }
    unlink_fiber(fiber);
    free(fiber);
  }
  return result;
}

u32 w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js(struct w2c_env* env, u32 from,
                                                u32 to) {
  (void)env;
  if (!g_ft.initialized) weblox_fibers_init();
  if (from == to) return 0;      // JS's undefined coerces to zero.
  weblox_fiber* self = g_ft.current ? g_ft.current : &g_ft.host;
  // Zero means "the current root's continuation" in the browser glue.  The
  // creator chain is the root on both the main thread and a worker.
  u32 dest = to;
  if (!dest) {
    for (weblox_fiber* c = self->creator_fiber; c && c != self;
         c = c->creator_fiber)
      if (c->key) { dest = c->key; break; }
    if (!dest && g_ft.root && g_ft.root != self && g_ft.root->key)
      dest = g_ft.root->key;
    if (!dest && &g_ft.host != self && g_ft.host.key) dest = g_ft.host.key;
  }
  weblox_fiber* target = lookup(dest);
  if (!target) {
    fprintf(stderr, "[fib] unknown destination continuation %u (to=%u from=%u "
                    "self=%u inst=%p)\n",
            dest, to, from, self->key, (void*)g_ft.inst);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  if (lookup(from)) {
    fprintf(stderr, "[fib] duplicate source continuation %u\n", from);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  if (verbose()) fprintf(stderr, "[fib] transfer %u -> %u\n", from, dest);
  return switch_away(target, from, FSTATE_WAIT_TRANSFER, from);
}

void w2c_env_fiber_destroy_js(struct w2c_env* env, u32 h) {
  (void)env;
  weblox_fiber* fiber = lookup(h);
  if (!fiber || fiber->root || fiber->state != FSTATE_WAIT_TRANSFER) {
    fprintf(stderr, "[fib] invalid continuation to destroy %u\n", h);
    return;
  }
  fprintf(stderr, "[fib] destroy key=%u\n", h);
  unlink_fiber(fiber);
  if (fiber->co) {
    mco_destroy(fiber->co);
    fiber->co = NULL;
  }
  fiber->used = 0;
  fiber->state = FSTATE_DEAD;
  free(fiber);
}

void w2c_env_emscripten_unwind_to_js_event_loop(struct w2c_env* env) {
  (void)env;
  weblox_fiber* self = g_ft.current ? g_ft.current : &g_ft.host;
  fprintf(stderr, "[fib] unwind_to_js_event_loop\n");
  if (!self->root && self->creator_fiber) {
    switch_away(self->creator_fiber, self->create_resumption,
                FSTATE_WAIT_TRANSFER, 0);
  } else if (self->co) {
    g_ft.next = &g_ft.host;
    mco_yield(self->co);
  }
}
