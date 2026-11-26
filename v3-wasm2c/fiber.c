/* impl/fiber.c - the engine's fibre imports.
 *
 * Chrome's glue (`ra`/`ta`/`na` in the shipped WebClient.js) is the spec.  It
 * keeps a map from a *resumption key* to [resolve, stack ptr, base, end]:
 *   - `create` files one under the new handle pointing at the creator's own
 *     state, points the shadow stack at the new window and starts the fibre;
 *   - a fibre yields by filing one under `from` (its `sp + 15`) and resuming
 *     another key;
 *   - a resume restores the target's stack bounds and pointer and returns the
 *     resumer's key, which the caller checks against the cell it left.
 * Suspension is JSPI in the browser; here each suspension point owns a
 * ucontext, because a fibre can resume *another point of itself* (both points
 * then live on the same machine stack at different depths).
 */
#define _GNU_SOURCE
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../host/host.h"
extern WASM_RT_THREAD_LOCAL uint32_t wasm_rt_call_stack_depth;


#define FIB_MAX 1024
#define SLOT_MAX 4096
#define FIB_STACK (32 * 1024 * 1024)

typedef struct slot slot_t;
typedef struct fib fib_t;

struct fib {
  u32 handle;
  u32 entry;
  u32 arg;
  int used, dead;
  void* hstack;
  slot_t* root;               /* root this fibre was spawned under */
};

struct slot {
  u32 key;
  u32 sp, lo, hi;
  u32 ret;                    /* value returned from the resume */
  u32 depth;                  /* wasm call depth owed to this point */
  ucontext_t ctx;             /* this suspension point */
  fib_t* owner;               /* NULL for a root context */
  slot_t* root;               /* the root this point belongs to */
  void* hstack;               /* host stack, for a root */
  int used, is_root, done;
};

#define ROOT_KEY 0u
#define FIB_KEY_BASE 0x80000000u      /* synthetic keys for non-fibre suspends */
#define ROOT_STACK (2 * 1024 * 1024)

static __thread fib_t g_fibs[FIB_MAX];
static __thread slot_t g_slots[SLOT_MAX];
static __thread fib_t* g_cur;      /* NULL means the root context */
static __thread u32 g_root_ret;
extern int g_fx;
int wc_fiber_current_is_root(void) { return g_cur == NULL; }
static slot_t* slot_new(u32 key);
static slot_t* slot_for(u32 key);

/* ---- scheduler context -------------------------------------------------
 *
 * Everything wasm runs as a *root* fibre spawned by the harness, so an import
 * that would block (a futex wait) can park its stack and let the loop carry on
 * - what JSPI does in the browser.  The harness pump sits in g_sched_ctx; a
 * parked point is resumed from there and parks again into it. */
static __thread ucontext_t g_sched_ctx;
static __thread u32 g_sched_depth;
static __thread void (*g_root_fn)(void*);
static __thread void* g_root_arg;
static __thread int g_root_done, g_root_live;
static __thread u32 g_key_seq;
static __thread void* g_root_stack;   /* stack of the root currently spawning */
/* The engine's handle 0 means "resume whoever created you", and a fibre may
 * only ever mean its *own* root - several roots can be in flight at once
 * (main() parked while a loop turn runs), exactly like the browser. */
static __thread slot_t* g_cur_root;

int wc_fiber_live(void) { return g_root_live; }
void wc_root_mode(void) { g_root_live = 1; }
int wc_root_active(void) { return g_root_live && g_cur_root && g_cur_root->used && !g_cur_root->done; }

static slot_t* cur_root(void) { return g_cur ? g_cur->root : g_cur_root; }

/* the root finished (or parked for good): drop its slot and host stack */
/* release a finished root (its slot and host stack) */
void wc_root_cleanup_slot(void* sp_) {
  slot_t* r = sp_;
  if (!r || !r->done) return;
  if (r->hstack) { free(r->hstack); r->hstack = NULL; }
  r->used = 0;
  if (g_cur_root == r) g_cur_root = NULL;
}

static void root_tramp(void) {
  void (*fn)(void*) = g_root_fn;
  void* arg = g_root_arg;
  slot_t* me = g_cur_root;
  wasm_rt_call_stack_depth = 0;
  fn(arg);
  me->done = 1;
  g_root_done = 1;
  setcontext(&g_sched_ctx);
}

/* run fn as the root fibre; 1 = it parked, 0 = it finished */
void* wc_root_spawn_slot(void (*fn)(void*), void* arg);
int wc_root_slot_done(void* sp_) { slot_t* t = sp_; return t ? t->done : 1; }

void* wc_root_spawn_slot(void (*fn)(void*), void* arg) {
  slot_t* prev_root = g_cur_root;
  slot_t* r = slot_new(FIB_KEY_BASE | (++g_key_seq));
  if (!r) { fn(arg); return NULL; }
  r->is_root = 1;
  r->root = r;
  g_cur_root = r;
  void* st = malloc(ROOT_STACK);
  r->hstack = st;
  g_root_stack = st;
  if (!st) { r->used = 0; g_cur_root = prev_root; fn(arg); return NULL; }
  g_root_fn = fn; g_root_arg = arg; g_root_done = 0; g_root_live = 1;
  getcontext(&r->ctx);
  r->ctx.uc_stack.ss_sp = st;
  r->ctx.uc_stack.ss_size = ROOT_STACK;
  r->ctx.uc_link = NULL;
  makecontext(&r->ctx, root_tramp, 0);
  g_cur = NULL;
  swapcontext(&g_sched_ctx, &r->ctx);
  g_cur_root = prev_root;
  return r;
}

int wc_root_spawn(void (*fn)(void*), void* arg) {
  void* s = wc_root_spawn_slot(fn, arg);
  return s && !wc_root_slot_done(s);
}

/* slot for a park at the current point (no switch yet) */
slot_t* wc_fiber_park_begin(void) {
  slot_t* r = cur_root();
  if ((!g_root_live || !r) && getenv("WC_FX"))
    fprintf(stderr, "[fx] park: live=%d cur=%p curroot=%p root=%p\n",
            g_root_live, (void*)g_cur, (void*)g_cur_root,
            g_cur ? (void*)g_cur->root : NULL);
  if (!g_root_live || !r) return NULL;
  if (g_cur) return slot_new(FIB_KEY_BASE | (++g_key_seq));
  return slot_for(r->key);
}

/* switch out until someone resumes the slot */
void wc_fiber_park(slot_t* s) {
  if (!s) return;
  s->depth = wasm_rt_call_stack_depth;
  swapcontext(&s->ctx, &g_sched_ctx);
}

/* park the current point and return to the pump; returns the slot so the
 * caller can read ->ret after it is resumed.  NULL = not parkable. */
slot_t* wc_fiber_suspend(void) {
  slot_t* s = wc_fiber_park_begin();
  if (!s) return NULL;
  wc_fiber_park(s);
  return s;
}

/* resume a parked point with a plain return value */
void wc_fiber_resume_slot(slot_t* t, u32 code) {
  if (!t || !t->used) return;
  t->ret = code;
  g_sched_depth = wasm_rt_call_stack_depth;
  wasm_rt_call_stack_depth = t->depth;
  w2c_WebClient_set_stack_limits(g_inst, t->hi, t->lo);
  w2c_WebClient_set_stack_ptr(g_inst, t->sp);
  fib_t* saved = g_cur;
  slot_t* saved_root = g_cur_root;
  g_cur = t->owner;
  g_cur_root = t->root ? t->root : g_cur_root;
  swapcontext(&g_sched_ctx, &t->ctx);
  g_cur_root = saved_root;
  g_cur = saved;
  wasm_rt_call_stack_depth = g_sched_depth;
}

void wc_fiber_slot_free(slot_t* t) { if (t) t->used = 0; }
u32 wc_fiber_slot_ret(slot_t* t) { return t ? t->ret : 0; }
int wc_fiber_slot_is_root(slot_t* t) { return t && t->key == ROOT_KEY; }

static slot_t* slot_for(u32 key) {
  for (int i = 0; i < SLOT_MAX; i++)
    if (g_slots[i].used && g_slots[i].key == key) return &g_slots[i];
  return NULL;
}

static slot_t* slot_new(u32 key) {
  slot_t* s = slot_for(key);
  if (!s)
    for (int i = 0; i < SLOT_MAX; i++)
      if (!g_slots[i].used) { s = &g_slots[i]; break; }
  if (!s)
    for (int i = 0; i < SLOT_MAX; i++)
      if (g_slots[i].used && g_slots[i].owner && g_slots[i].owner->dead) { s = &g_slots[i]; break; }
  if (!s) return NULL;
  memset(s, 0, sizeof(*s));
  { static __thread int q; if (g_wc_verbose && q++ < 40)
      fprintf(stderr, "[slot] key=%x cur=%p\n", key, (void*)g_cur); }
  s->used = 1;
  s->key = key;
  s->root = cur_root();
  s->owner = g_cur;
  s->sp = w2c_WebClient_get_stack_ptr(g_inst);
  s->hi = w2c_WebClient_get_stack_base(g_inst);   /* base == high */
  s->lo = w2c_WebClient_get_stack_end(g_inst);    /* end  == low */
  return s;
}

static fib_t* find_fib(u32 h) {
  for (int i = 0; i < FIB_MAX; i++)
    if (g_fibs[i].used && !g_fibs[i].dead && g_fibs[i].handle == h) return &g_fibs[i];
  return NULL;
}

/* restore a suspension point and switch to it; its own ctx is saved by the
 * caller (swapcontext writes it) */
static void enter_slot(slot_t* t, slot_t* mine, u32 from) {
  t->ret = from;
  /* every suspension point owns its wasm call depth, like its stack: the
   * counter is per thread, so suspended frames of one fibre would otherwise
   * count against whichever fibre runs next */
  mine->depth = wasm_rt_call_stack_depth;
  wasm_rt_call_stack_depth = t->depth;
  w2c_WebClient_set_stack_limits(g_inst, t->hi, t->lo);
  w2c_WebClient_set_stack_ptr(g_inst, t->sp);
  g_cur = t->owner;
  swapcontext(&mine->ctx, &t->ctx);
  wasm_rt_call_stack_depth = mine->depth;
}

static void tramp(void) {
  fib_t* self = g_cur;
  wasm_rt_call_stack_depth = 0;               /* a fresh wasm stack */
  w2c_WebClient_fiber_entry(g_inst, self->handle, self->entry, self->arg);
  self->dead = 1;
  slot_t* home = slot_for(self->handle);      /* the create slot */
  if (!home) { fprintf(stderr, "[fib] no home for %x\n", self->handle); abort(); }
  /* drop this fibre's suspension points: their contexts die with it, and the
   * host stack is freed below, so a later fibre at the same address must not
   * find them by key */
  for (int i = 0; i < SLOT_MAX; i++)
    if (g_slots[i].used && g_slots[i].owner == self && &g_slots[i] != home)
      g_slots[i].used = 0;
  slot_t* mine = slot_new(w2c_WebClient_get_stack_ptr(g_inst) + 15u);
  home->ret = 0;
  w2c_WebClient_set_stack_limits(g_inst, home->hi, home->lo);
  w2c_WebClient_set_stack_ptr(g_inst, home->sp);
  g_cur = home->owner;
  wasm_rt_call_stack_depth = home->depth;
  ucontext_t* ct = &home->ctx;
  home->used = 0;                            /* the handle is never resumed again */
  swapcontext(mine ? &mine->ctx : ct, ct);
  fprintf(stderr, "[fib] tramp fell through\n");
  abort();
}

u32 wc_impl_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js(struct w2c_env* a0, u32 h, u32 sp,
                                                       u32 limit, u32 fn, u32 arg) {
  (void)a0;
  fib_t* f = NULL;
  for (int i = 0; i < FIB_MAX; i++) if (!g_fibs[i].used) { f = &g_fibs[i]; break; }
  if (!f) return 0;
  memset(f, 0, sizeof(*f));
  f->used = 1;
  f->handle = h;
  f->root = cur_root();
  f->entry = fn;
  f->arg = arg;
  if (g_trace_calls > 0 || g_fx) fprintf(stderr, "[fx2] create h=%x fn=%x arg=%x\n", h, fn, arg);
  if (g_wc_verbose) fprintf(stderr, "[fibc] h=%x sp=%x limit=%x memsz=%llx\n", h, sp, limit, (unsigned long long)g_mem.size);
  f->hstack = malloc(FIB_STACK);
  if (!f->hstack) return 0;

  /* slot h resumes the creator, so file it before switching windows */
  slot_t* home = slot_new(h);
  if (!home) return 0;
  home->sp = w2c_WebClient_get_stack_ptr(g_inst);
  home->hi = w2c_WebClient_get_stack_base(g_inst);
  home->lo = w2c_WebClient_get_stack_end(g_inst);

  w2c_WebClient_set_stack_limits(g_inst, sp, limit);
  w2c_WebClient_set_stack_ptr(g_inst, sp);

  /* start the fibre on its own stack */
  ucontext_t start;
  getcontext(&start);
  start.uc_stack.ss_sp = f->hstack;
  start.uc_stack.ss_size = FIB_STACK;
  start.uc_link = NULL;
  makecontext(&start, tramp, 0);
  g_cur = f;
  slot_t* mine = slot_new(sp - 1);        /* the fibre's entry point */
  if (!mine) return 0;
  mine->owner = f;
  swapcontext(&home->ctx, &start);        /* returns when the fibre yields */
  if (g_wc_verbose) fprintf(stderr, "[create] h=%x ret=%x\n", h, home->ret);
  g_cur = home->owner;
  u32 r = home->ret;
  if (f->dead) { if (f->hstack) free(f->hstack); f->hstack = NULL; f->used = 0; }
  return r;                                  /* home stays: it is the return path */
}

extern int g_trace_calls;
extern void wc_bt_dump(void);
u32 wc_impl_0x5F_asyncjs_0x5Ffiber_transfer_js(struct w2c_env* a0, u32 from, u32 to) {
  (void)a0;
  { static int d; if (g_trace_calls > 0 && d < 4) { d++; fprintf(stderr, "[trbt]\n"); wc_bt_dump(); } }
  if (g_trace_calls > 0 || g_fx) fprintf(stderr, "[fx2] tr %x -> %x (%s)\n", from, to, slot_for(to) ? "have" : "MISS");
  if (from == to) return from;
  u32 tk = to;
  if (!tk) { slot_t* r = cur_root(); if (!r) return from; tk = r->key; }
  slot_t* mine = slot_new(from);
  if (!mine) return from;
  slot_t* t = slot_for(tk);
  if (!t) { t = slot_new(tk); if (!t) { mine->used = 0; return from; } }
  enter_slot(t, mine, from);
  u32 r = mine->ret;
  mine->used = 0;
  return r;
}

void wc_impl_fiber_destroy_js(struct w2c_env* a0, u32 h) {
  (void)a0;
  if (g_trace_calls > 0 || g_fx) fprintf(stderr, "[fx2] destroy h=%x\n", h);
  fib_t* f = find_fib(h);
  if (!f) return;
  f->dead = 1;
  slot_t* home = slot_for(h);
  slot_t* mine = slot_new(w2c_WebClient_get_stack_ptr(g_inst) + 15u);
  if (home) {
    home->owner = home->owner;               /* keep */
    enter_slot(home, mine ? mine : home, 0);
    home->used = 0;
  }
}
