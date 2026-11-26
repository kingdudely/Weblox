/* impl/futex.c - Atomics.waitAsync without JSPI.
 *
 * In the browser the wait is an Atomics.waitAsync: the promise keeps the wasm
 * call *parked* and the event loop runs on.  A blocked host thread cannot do
 * that (another task on the same thread may hold the lock it wants), so the
 * calling stack is parked as a fibre slot and resumed from the pump when the
 * word changes or the timeout expires.  Return codes match the glue: 0 woken,
 * 1 not-equal, 2 timed out.
 *
 * Threads without a root fibre (workers) block on a condition variable.
 */
#include "../host/host.h"
#include "../host/plat.h"
#include <pthread.h>
#include <time.h>

extern int g_fx;
#define WAITERS 128

struct waiter {
  u32 addr;
  void* slot;              /* parked point when running on the main thread */
  double deadline;         /* 0 = none */
  int used;
};

/* A fibre is a second stack on the same thread, so two fibres can meet in
 * here; a plain mutex would deadlock (glibc cannot tell it is the same
 * thread).  Recursive makes the second fibre's lock succeed, and since it is
 * one thread there is no writer/writer race to lose. */
static pthread_mutex_t g_mtx;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
__attribute__((constructor)) static void mtx_init(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_mtx, &a);
}
static struct waiter g_w[WAITERS];

__thread int g_is_main;

int wc_pump_once(void);
extern int g_wait_depth;

/* slots the pump should resume: {slot, value} */
#define READY_MAX 1024
static struct { void* slot; u32 code; } g_ready[READY_MAX];
static int g_qh, g_qt;
static pthread_mutex_t g_qmtx = PTHREAD_MUTEX_INITIALIZER;

static void ready_push(void* slot, u32 code) {
  pthread_mutex_lock(&g_qmtx);
  int n = (g_qt + 1) % READY_MAX;
  if (n != g_qh) { g_ready[g_qt].slot = slot; g_ready[g_qt].code = code; g_qt = n; }
  pthread_mutex_unlock(&g_qmtx);
}

u32 wc_futex_notify(u32 addr, u32 count) {
  pthread_mutex_lock(&g_mtx);
  int n = 0;
  for (int i = 0; i < WAITERS && n < (int)count; i++)
    if (g_w[i].used && g_w[i].addr == addr) {
      if (g_w[i].slot) {
        void* s = g_w[i].slot;
        g_w[i].used = 0;
        ready_push(s, 0);
      } else {
        g_w[i].used = 0;
      }
      n++;
    }
  pthread_mutex_unlock(&g_mtx);
  if (g_fx) fprintf(stderr, "[fx] wake addr=%x n=%d tid=%lx\n", addr, n, (unsigned long)pthread_self());
  if (n) pthread_cond_broadcast(&g_cv);
  return (u32)n;
}

extern void wc_fiber_resume_slot(void* t, u32 code);
extern int  wc_fiber_live(void);
extern void* wc_fiber_park_begin(void);
extern void  wc_fiber_park(void* s);
extern void  wc_fiber_slot_free(void* s);
extern u32   wc_fiber_slot_ret(void* s);
extern int   wc_fiber_slot_is_root(void* s);
extern int   wc_fiber_current_is_root(void);

/* resume parked waits whose timeout expired, then everything the wakers
 * queued.  Called by the pump on the main thread. */
void wc_futex_pump(void) {
  double now = pl_now_ms();
  for (;;) {
    void* slot = NULL;
    u32 code = 0;
    pthread_mutex_lock(&g_qmtx);
    if (g_qh != g_qt) {
      slot = g_ready[g_qh].slot; code = g_ready[g_qh].code;
      g_qh = (g_qh + 1) % READY_MAX;
    }
    pthread_mutex_unlock(&g_qmtx);
    if (!slot) break;
    wc_fiber_resume_slot(slot, code);
  }
  /* collect first: resuming runs wasm, which may take g_mtx again */
  void* todo[16];
  int nt = 0;
  pthread_mutex_lock(&g_mtx);
  for (int i = 0; i < WAITERS && nt < 16; i++)
    if (g_w[i].used && g_w[i].slot && g_w[i].deadline && g_w[i].deadline <= now) {
      todo[nt++] = g_w[i].slot;
      g_w[i].used = 0;
    }
  pthread_mutex_unlock(&g_mtx);
  for (int i = 0; i < nt; i++) wc_fiber_resume_slot(todo[i], 2);
}

extern int g_trace_calls;
u32 wc_impl_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi(struct w2c_env* a0, u32 a1, u32 a2, f64 a3) {
  (void)a0;
  if (g_trace_calls > 0 || g_fx)
    fprintf(stderr, "[fx] wait addr=%x want=%x now=%x root=%d\n", a1, a2,
            *(volatile u32*)wc_ptr(a1), wc_fiber_current_is_root());
  volatile u32* word = (volatile u32*)wc_ptr(a1);
  if (*word != a2) return 1;
  int infinite = !(a3 > 0.0) || a3 >= 1e12;
  double deadline = infinite ? 0.0 : pl_now_ms() + a3;

  pthread_mutex_lock(&g_mtx);
  int slot = -1;
  for (int i = 0; i < WAITERS; i++) if (!g_w[i].used) { slot = i; break; }
  if (slot >= 0) { g_w[slot].used = 1; g_w[slot].addr = a1; g_w[slot].deadline = deadline; g_w[slot].slot = NULL; }
  pthread_mutex_unlock(&g_mtx);

  if (g_is_main && wc_fiber_live()) {
    void* me = wc_fiber_park_begin();
    if (g_fx && !me) fprintf(stderr, "[fx] park refused\n");
    if (me && slot >= 0) {
      pthread_mutex_lock(&g_mtx);
      g_w[slot].slot = me;
      pthread_mutex_unlock(&g_mtx);
      wc_fiber_park(me);
      pthread_mutex_lock(&g_mtx);
      int still = g_w[slot].used;
      g_w[slot].used = 0;
      pthread_mutex_unlock(&g_mtx);
      if (still) wc_fiber_slot_free(me);   /* woken by the timeout scan */
      u32 r = wc_fiber_slot_ret(me);
      if (!wc_fiber_slot_is_root(me)) wc_fiber_slot_free(me);
      return r;
    }
  }

  for (;;) {
    if (*word != a2) { if (slot >= 0) g_w[slot].used = 0; return 1; }
    if (!infinite && pl_now_ms() >= deadline) { if (slot >= 0) g_w[slot].used = 0; return 2; }

    /* the main thread is the one that advances the engine: keep pumping while
     * parked, and never sleep unbounded (a notify can land during the pump) */
    if (g_is_main) {
      wc_pump_once();
      if (*word != a2) continue;
    }

    if (g_is_main) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 1000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_mutex_lock(&g_mtx);
      pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
      pthread_mutex_unlock(&g_mtx);
    } else {
      pthread_mutex_lock(&g_mtx);
      pthread_cond_wait(&g_cv, &g_mtx);
      pthread_mutex_unlock(&g_mtx);
    }
  }
}
