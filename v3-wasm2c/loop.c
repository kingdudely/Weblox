/* impl/loop.c - the event-loop inversion.
 *
 * The engine asks to be called back (`schedule_main_loop`) instead of blocking.
 * The host pumps main_loop_iteration while that flag is set, and runs deferred
 * EM_ASYNC-style callbacks first.
 */
#include "../host/host.h"
#include "../host/plat.h"
#include <pthread.h>
#include <stdarg.h>

extern int wc_embind_invoke(const char* name, int argc, ...);

volatile int g_wc_sched;

struct deferred {
  u32 fn, arg;
  const char* name;            /* non-NULL: call an embind callback instead */
  int nargs;
  u32 a[5];
};
#define QN 8192
#define MAILBOX_FN 0xFFFFFFFFu
extern void w2c_WebClient_0x5Femscripten_check_mailbox(w2c_WebClient*);
static struct deferred q[QN];
static int qh, qt;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;

void wc_impl_schedule_main_loop(struct w2c_env* a0) {
  (void)a0;
  g_wc_sched = 1;
}

void wc_stop_loop(void) { g_wc_sched = 0; }

static struct deferred* q_push(void) {
  struct deferred* d;
  pthread_mutex_lock(&q_mtx);
  int n = (qt + 1) % QN;
  if (n == qh) {                    /* full: wait for the pump */
    pthread_mutex_unlock(&q_mtx);
    pl_sleep_ms(1);
    pthread_mutex_lock(&q_mtx);
    n = (qt + 1) % QN;
    if (n == qh) { pthread_mutex_unlock(&q_mtx); return NULL; }
  }
  d = &q[qt];
  qt = n;
  pthread_mutex_unlock(&q_mtx);
  return d;
}

void wc_defer(u32 fn, u32 arg) {
  struct deferred* d = q_push();
  if (!d) return;
  d->fn = fn;
  d->arg = arg;
  d->name = NULL;
}

/* a worker posted to the main thread's mailbox: drain it on the next pump */
void wc_defer_mailbox(void) {
  struct deferred* d = q_push();
  if (!d) return;
  d->name = NULL;
  d->fn = MAILBOX_FN;
  d->arg = 0;
}

/* queue an embind-registered host callback (worker threads produce these) */
void wc_defer_embind(const char* name, int nargs, ...) {
  struct deferred* d = q_push();
  if (!d) return;
  d->name = name;
  d->nargs = nargs;
  va_list ap;
  va_start(ap, nargs);
  for (int i = 0; i < nargs && i < 5; i++) d->a[i] = va_arg(ap, u32);
  va_end(ap);
}

void wc_impl_emscripten_async_call(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0; (void)a3;
  wc_defer(a1, a2);
}

/* do one unit of engine work: a deferred callback, else one loop iteration.
 * returns 1 if anything ran. */
int wc_pump_deferred(void);

/* the glue's schedule_main_loop starts a self-perpetuating message loop; the
 * flag stays set until the engine tears the loop down, so every pump iterates */
static __thread int g_in_loop;
/* a main-thread futex wait parks in the middle of an iteration; the engine
 * still has to advance (Chrome suspends the JSPI stack and the loop keeps
 * going).  Allow bounded re-entry so timers and other tasks run. */
int g_wait_depth;

int wc_pump_once(void) {
  static unsigned long np;
  if (g_wc_verbose && (++np % 20000) == 0) fprintf(stderr, "[pump] %lu sched=%d\n", np, g_wc_sched);
  if (wc_pump_deferred()) return 1;
  if (!g_wc_sched) return 0;
  if (g_in_loop && (g_wait_depth <= 0 || g_wait_depth >= 32)) return 0;
  { static int d; if (g_in_loop && d++ < 8) fprintf(stderr, "[nest] depth=%d\n", g_wait_depth); }
  int saved = g_in_loop;
  g_in_loop = 1;
  w2c_WebClient_main_loop_iteration(g_inst);
  g_in_loop = saved;
  return 1;
}

/* run queued callbacks; returns how many ran */
int wc_pump_deferred(void) {
  int n = 0;
  for (;;) {
    struct deferred d;
    pthread_mutex_lock(&q_mtx);
    if (qh == qt) { pthread_mutex_unlock(&q_mtx); break; }
    d = q[qh];
    qh = (qh + 1) % QN;
    pthread_mutex_unlock(&q_mtx);
    if (d.name) {
      switch (d.nargs) {
        case 1: wc_embind_invoke(d.name, 1, d.a[0]); break;
        case 2: wc_embind_invoke(d.name, 2, d.a[0], d.a[1]); break;
        case 3: wc_embind_invoke(d.name, 3, d.a[0], d.a[1], d.a[2]); break;
        case 4: wc_embind_invoke(d.name, 4, d.a[0], d.a[1], d.a[2], d.a[3]); break;
        case 5: wc_embind_invoke(d.name, 5, d.a[0], d.a[1], d.a[2], d.a[3], d.a[4]); break;
      }
    } else if (d.fn == MAILBOX_FN) {
      w2c_WebClient_0x5Femscripten_check_mailbox(g_inst);
    } else if (d.fn < g_inst->w2c_0x5F_indirect_function_table.size) {
      wasm_rt_funcref_t* e = &g_inst->w2c_0x5F_indirect_function_table.data[d.fn];
      if (e->func) ((void (*)(void*, u32))e->func)(e->module_instance, d.arg);
    }
    n++;
  }
  return n;
}
