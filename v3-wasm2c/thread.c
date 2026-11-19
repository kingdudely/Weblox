/* impl/thread.c - worker threads, one module instance each.
 *
 * Chrome gives every worker its own instance of the module over the shared
 * memory; globals, TLS and the funcref table are per worker, only linear memory
 * is common.  That is exactly what wasm2c's instance struct gives us, so a
 * worker here is: fresh instance, own pthread, own TLS - no stack swapping.
 */
#include "../host/host.h"
#include "../host/plat.h"
#include <pthread.h>
#include <time.h>

/* The mailbox is how one thread asks another to run a JS-side operation: it
 * queues an item in shared memory, rings the target's mailbox word and waits.
 * Chrome delivers the ring as a worker message and the target runs
 * `_emscripten_check_mailbox`.  Here the ring is a condvar and the main thread
 * drains its own mailbox from the deferred queue. */
#define MB_MAX 32
typedef struct { u32 p; pthread_mutex_t m; pthread_cond_t c; int used, sig; } mbox_t;
static mbox_t g_mb[MB_MAX];
static pthread_mutex_t g_mb_mtx;
__attribute__((constructor)) static void mbmtx_init(void) {
  pthread_mutexattr_t a;                     /* recursive: same-thread fibres */
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_mb_mtx, &a);
}
static u32 g_main_pthread;

extern void wc_defer_mailbox(void);

static mbox_t* mb_for(u32 p, int create) {
  pthread_mutex_lock(&g_mb_mtx);
  mbox_t* free_slot = NULL;
  for (int i = 0; i < MB_MAX; i++) {
    if (g_mb[i].used && g_mb[i].p == p) { pthread_mutex_unlock(&g_mb_mtx); return &g_mb[i]; }
    if (!g_mb[i].used && !free_slot) free_slot = &g_mb[i];
  }
  if (create && free_slot) {
    memset(free_slot, 0, sizeof *free_slot);
    { pthread_mutexattr_t a;
      pthread_mutexattr_init(&a);
      pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&free_slot->m, &a); }
    pthread_cond_init(&free_slot->c, NULL);
    free_slot->used = 1;
    free_slot->p = p;
  } else free_slot = NULL;
  pthread_mutex_unlock(&g_mb_mtx);
  return free_slot;
}

extern int g_trace_calls;
extern __thread int g_is_main;
int wc_pump_deferred(void);

void wc_mailbox_await(u32 p) {
  { static int d; if (d++ < 30) fprintf(stderr, "[mb] await p=%x tid=%lx\n", p, (unsigned long)pthread_self()); }
  if (g_trace_calls > 0) fprintf(stderr, "[mbx] await tid=%lx\n", (unsigned long)pthread_self());
  mbox_t* b = mb_for(p, 1);
  if (!b) return;
  *(u32*)wc_ptr(p + 112) = 1;                 /* "busy", as the glue sets it */

  if (g_is_main) {
    /* `Cr` is not blocking: it arms `Atomics.waitAsync`, whose continuation
     * (`Ke`) runs check_mailbox from a later task, and returns to wasm at once.
     * So: run whatever the event loop would have run (deferred embind
     * callbacks, check_mailbox) and return - the harness pumps again after
     * main_loop_iteration comes back. */
    wc_pump_deferred();
    b->sig = 0;
    return;
  }

  pthread_mutex_lock(&b->m);
  while (!b->sig) pthread_cond_wait(&b->c, &b->m);
  b->sig = 0;
  pthread_mutex_unlock(&b->m);
}

static void mb_signal(u32 p) {
  mbox_t* b = mb_for(p, 0);
  if (!b) return;
  __atomic_store_n(&b->sig, 1, __ATOMIC_RELEASE);
  pthread_mutex_lock(&b->m);
  pthread_cond_signal(&b->c);
  pthread_mutex_unlock(&b->m);
}

extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;

struct worker {
  w2c_WebClient inst;
  u32 pthread_ptr;
  u32 start;
  u32 arg;
};

static void* worker_main(void* p) {
  struct worker* w = p;
  g_inst = &w->inst;
  wasm_rt_init_thread();
  wasm2c_WebClient_instantiate(&w->inst, &g_env, &g_wasi);

  /* the stack the parent allocated for us, exactly like the glue's `wi` */
  u32 top = *(u32*)wc_ptr(w->pthread_ptr + 48);
  u32 bottom = *(u32*)wc_ptr(w->pthread_ptr + 52);
  if (top && top > bottom) {
    w2c_WebClient_set_stack_limits(&w->inst, top, top - bottom);
    w2c_WebClient_set_stack_ptr(&w->inst, top);
  }
  *(u32*)wc_ptr(w->pthread_ptr + 112) = 1;   /* "started" */

  w2c_WebClient_0x5Femscripten_thread_init(&w->inst, w->pthread_ptr, 0u, 0u, 1u, 0u, 0u);
  w2c_WebClient_0x5Femscripten_tls_init(&w->inst);

  wasm_rt_funcref_t* e = &w->inst.w2c_0x5F_indirect_function_table.data[w->start];
  if (e->func) {
    WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
    WASM_RT_UNWIND_TARGET t;
    if (!wasm_rt_try(t)) {
      wasm_rt_set_unwind_target(&t);
      ((void (*)(void*, u32))e->func)(e->module_instance, w->arg);
      wasm_rt_set_unwind_target(outer);
    } else {
      wasm_rt_set_unwind_target(outer);
      fprintf(stderr, "[thread] %u threw %.*s\n", w->pthread_ptr,
              (int)wasm_rt_exception_size(), (const char*)wasm_rt_exception());
    }
  }
  return NULL;
}

u32 wc_impl_0x5F_pthread_create_js(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0; (void)a2;
  struct worker* w = calloc(1, sizeof *w);
  if (!w) return 11; /* EAGAIN */
  w->pthread_ptr = a1;
  w->start = a3;
  w->arg = a4;
  pthread_t th;
  if (pthread_create(&th, NULL, worker_main, w) != 0) { free(w); return 11; }
  pthread_detach(th);
  if (g_wc_verbose) fprintf(stderr, "[thread] spawn %u start=%u\n", a1, a3);
  return 0;
}

void wc_impl_0x5Femscripten_notify_mailbox_postmessage(struct w2c_env* a0, u32 target,
                                                       u32 self) {
  (void)a0;
  { static int d; if (d++ < 30) fprintf(stderr, "[mb] notify t=%x s=%x tid=%lx\n", target, self, (unsigned long)pthread_self()); }
  if (g_trace_calls > 0) fprintf(stderr, "[mbx] notify t=%x s=%x tid=%lx\n", target, self, (unsigned long)pthread_self());
  if (g_main_pthread && target == g_main_pthread) { wc_defer_mailbox(); return; }
  if (target == self) { mb_signal(target); return; }
  mb_signal(target);
}

/* mirrors WasmPlayer.js before main(): thread_init(ptr, is_main, …) + TLS init.
 * Registered through the import table too, so it works either way. */
void wc_impl_0x5Femscripten_init_main_thread_js(struct w2c_env* a0, u32 a1) {
  (void)a0;
  g_main_pthread = a1;
  w2c_WebClient_0x5Femscripten_thread_init(g_inst, a1, 1u, 1u, 1u, 262144u, 0u);
  w2c_WebClient_0x5Femscripten_tls_init(g_inst);
}
