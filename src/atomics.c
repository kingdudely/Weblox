// Shared-memory atomics implemented with SDL3 mutexes and conditions.
//
// Worker threads block on the condition variable: nobody else can run on their
// stack, so an Atomics.wait is an ordinary blocking wait there.  The main
// thread cannot block: another task on the same thread may own a lock it needs
// and the guest event loop must keep turning.  A main-thread JSPI wait therefore
// parks the calling fibre and returns to the host pump; the waiter is woken by
// Atomics.notify, by a deadline, or by the guest word changing.
#include "atomics.h"
#include "deferred.h"
#include "fibers.h"
#include "guest.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct waiter {
  struct waiter* next;
  wasm_rt_shared_memory_t* memory;
  u32 address;
  SDL_Condition* ready;
  int signaled;
} waiter;

// A parked main-thread fibre.  `fiber` is an opaque handle from fibers.c and
// only ever resumed from the host stack by weblox_atomic_pump().
typedef struct js_waiter {
  struct js_waiter* next;
  wasm_rt_shared_memory_t* memory;
  u32 address;
  u32 expected;
  u64 deadline;      /* SDL ticks, 0 = infinite */
  void* fiber;
  int used;
} js_waiter;

typedef struct ready_item {
  struct ready_item* next;
  void* fiber;
  u32 code;
} ready_item;

static SDL_Mutex* g_lock;
static waiter* g_waiters;
static js_waiter* g_js_waiters;
static ready_item* g_ready_head;
static ready_item* g_ready_tail;

int weblox_atomic_init(void) {
  if (!g_lock) g_lock = SDL_CreateMutex();
  if (!g_lock) return 0;
  if (!SDL_WasInit(SDL_INIT_EVENTS)) SDL_InitSubSystem(SDL_INIT_EVENTS);
  return 1;
}

void weblox_atomic_free(void) {
  SDL_DestroyMutex(g_lock);
  g_lock = NULL;
  g_waiters = NULL;
  while (g_js_waiters) {
    js_waiter* next = g_js_waiters->next;
    free(g_js_waiters);
    g_js_waiters = next;
  }
  while (g_ready_head) {
    ready_item* next = g_ready_head->next;
    free(g_ready_head);
    g_ready_head = next;
  }
  g_ready_tail = NULL;
}

static void check_range(const wasm_rt_shared_memory_t* memory, u32 address,
                        unsigned width) {
  if (!memory) wasm_rt_trap(WASM_RT_TRAP_OOB);
  if ((u64)address + width > memory->size) wasm_rt_trap(WASM_RT_TRAP_OOB);
  if (address % width) wasm_rt_trap(WASM_RT_TRAP_UNALIGNED);
}

static u64 load_word(const unsigned char* bytes, unsigned width) {
  if (width == 4)
    return __atomic_load_n((const u32*)(const void*)bytes, __ATOMIC_SEQ_CST);
  return __atomic_load_n((const u64*)(const void*)bytes, __ATOMIC_SEQ_CST);
}

// g_lock is held by the caller.  Ready items are drained by the host pump with
// the lock released, because resuming a fibre runs wasm that may notify again.
static int ready_push(void* fiber, u32 code) {
  ready_item* item = calloc(1, sizeof *item);
  if (!item) return 0;
  item->fiber = fiber;
  item->code = code;
  if (g_ready_tail)
    g_ready_tail->next = item;
  else
    g_ready_head = item;
  g_ready_tail = item;
  return 1;
}

static u32 wait_for_value(wasm_rt_shared_memory_t* memory, u32 address,
                          u64 expected, s64 timeout_ns, unsigned width,
                          int service_callbacks) {
  check_range(memory, address, width);
  unsigned char* bytes = guest_ptr(address, width);
  if (!bytes) wasm_rt_trap(WASM_RT_TRAP_OOB);
  if (!weblox_atomic_init()) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);

  const int infinite = timeout_ns < 0;
  u64 deadline = infinite ? 0 : SDL_GetTicksNS() + (u64)timeout_ns;
  SDL_LockMutex(g_lock);
  u64 current = load_word(bytes, width);
  if (current != expected || timeout_ns == 0) {
    SDL_UnlockMutex(g_lock);
    return current != expected ? 1u : 2u;
  }

  waiter wait;
  memset(&wait, 0, sizeof wait);
  wait.memory = memory;
  wait.address = address;
  wait.ready = SDL_CreateCondition();
  if (!wait.ready) {
    SDL_UnlockMutex(g_lock);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  waiter** link = &g_waiters;
  while (*link) link = &(*link)->next;
  *link = &wait;

  int interrupted = 0;
  wasm_rt_trap_t callback_trap = WASM_RT_TRAP_NONE;
  while (!wait.signaled) {
    if (service_callbacks) {
      SDL_UnlockMutex(g_lock);
      SDL_PumpEvents();
      interrupted = SDL_HasEvent(SDL_EVENT_QUIT) ||
                    SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
      if (!interrupted) {
        w2c_WebClient* inst = weblox_fiber_instance();
        for (int i = 0; i < 64; ++i) {
          if (!weblox_deferred_service(inst, &callback_trap)) break;
          if (callback_trap != WASM_RT_TRAP_NONE) break;
        }
      }
      SDL_LockMutex(g_lock);
      if (interrupted || wait.signaled ||
          callback_trap != WASM_RT_TRAP_NONE)
        break;
    }

    if (infinite && !service_callbacks) {
      SDL_WaitCondition(wait.ready, g_lock);
    } else {
      u64 now = SDL_GetTicksNS();
      if (!infinite && now >= deadline) break;
      u64 ms = service_callbacks
                   ? 4u
                   : (infinite ? 10u
                               : (deadline - now + SDL_NS_PER_MS - 1) /
                                     SDL_NS_PER_MS);
      if (ms == 0) ms = 1;
      SDL_WaitConditionTimeout(wait.ready, g_lock,
                               (Sint32)SDL_min(ms, (u64)INT_MAX));
    }
  }

  link = &g_waiters;
  while (*link && *link != &wait) link = &(*link)->next;
  if (*link) *link = wait.next;
  SDL_UnlockMutex(g_lock);
  SDL_DestroyCondition(wait.ready);

  if (callback_trap != WASM_RT_TRAP_NONE) {
    fprintf(stderr, "[atom] deferred callback trapped: %s\n",
            wasm_rt_strerror(callback_trap));
    wasm_rt_trap(callback_trap);
  }
  if (interrupted) {
    fputs("[atom] event wait interrupted by window close\n", stderr);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);
  }
  return wait.signaled ? 0u : 2u;
}

u32 weblox_atomic_wait32(wasm_rt_shared_memory_t* memory, u32 address,
                         u32 expected, u64 timeout) {
  return wait_for_value(memory, address, expected, (s64)timeout, 4, 0);
}

u32 weblox_atomic_wait64(wasm_rt_shared_memory_t* memory, u32 address,
                         u64 expected, u64 timeout) {
  return wait_for_value(memory, address, expected, (s64)timeout, 8, 0);
}

// Main-thread JSPI wait.  The guest stack is parked, so the host event loop
// keeps running while the wait is outstanding.  Return codes match JS: 0 woken,
// 1 not-equal at entry, 2 timed out.
u32 weblox_atomic_wait_js(wasm_rt_shared_memory_t* memory, u32 address,
                          u32 expected, s64 timeout_ns) {
  check_range(memory, address, 4);
  unsigned char* bytes = guest_ptr(address, 4);
  if (!bytes) wasm_rt_trap(WASM_RT_TRAP_OOB);
  if (!weblox_atomic_init()) wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);

  u64 current = load_word(bytes, 4);
  if (current != expected) return 1u;
  if (timeout_ns == 0) return 2u;

  // Only a fibre can be parked.  A wait reached from a host-stack callback
  // cannot suspend without abandoning that callback's C stack, so an infinite
  // one reports a timeout instead of spinning the main thread until the
  // external budget kills the process.
  if (weblox_fiber_on_main_thread() && weblox_fiber_on_host_stack()) {
    if (timeout_ns < 0) {
      fprintf(stderr, "[futex] unparkable host wait addr=%u -> timeout\n",
              address);
      return 2u;
    }
    return wait_for_value(memory, address, expected, timeout_ns, 4, 1);
  }
  if (!weblox_fiber_on_main_thread())
    return wait_for_value(memory, address, expected, timeout_ns, 4, 0);

  js_waiter wait;
  memset(&wait, 0, sizeof wait);
  wait.memory = memory;
  wait.address = address;
  wait.expected = expected;
  wait.deadline = timeout_ns < 0 ? 0 : SDL_GetTicksNS() + (u64)timeout_ns;
  wait.fiber = weblox_fiber_current_handle();
  wait.used = 1;
  SDL_LockMutex(g_lock);
  js_waiter** link = &g_js_waiters;
  while (*link) link = &(*link)->next;
  *link = &wait;
  SDL_UnlockMutex(g_lock);
  fprintf(stderr, "[futex] park addr=%u want=%u deadline=%llu fiber=%p\n",
          address, expected, (unsigned long long)wait.deadline, wait.fiber);

  u32 code = weblox_fiber_park();

  // A notify consumes the waiter; a deadline/value wake does too.  In either
  // case remove any leftover entry now that this stack is running again.
  SDL_LockMutex(g_lock);
  js_waiter** cursor = &g_js_waiters;
  while (*cursor && *cursor != &wait) cursor = &(*cursor)->next;
  if (*cursor) *cursor = wait.next;
  SDL_UnlockMutex(g_lock);
  fprintf(stderr, "[futex] resumed addr=%u code=%u\n", address, code);
  return code;
}

u32 weblox_atomic_notify(wasm_rt_shared_memory_t* memory, u32 address,
                         u32 count) {
  check_range(memory, address, 4);
  if (!g_lock) weblox_atomic_init();
  u32 woken = 0;
  SDL_LockMutex(g_lock);
  for (waiter* wait = g_waiters; wait && woken < count; wait = wait->next) {
    if (!wait->signaled && wait->address == address &&
        wait->memory == memory) {
      wait->signaled = 1;
      SDL_SignalCondition(wait->ready);
      ++woken;
    }
  }
  for (js_waiter* wait = g_js_waiters; wait && woken < count;
       wait = wait->next) {
    if (wait->used && wait->address == address && wait->memory == memory) {
      if (ready_push(wait->fiber, 0)) {  // woken by notify
        wait->used = 0;
        fprintf(stderr, "[futex] notify-woke addr=%u fiber=%p\n", address,
                wait->fiber);
        ++woken;
      }
    }
  }
  SDL_UnlockMutex(g_lock);
  if (woken)
    fprintf(stderr, "[atom] notify addr=%u woke=%u\n", address, woken);
  return woken;
}

// Called by the host pump after every turn.  Resuming a fibre runs wasm, so
// nothing is resumed from a fibre (fibres may only mco_yield) and nothing is
// resumed while the atomics mutex is held (wasm may notify again).
void weblox_atomic_pump(void) {
  if (!g_lock) return;
  if (!weblox_fiber_on_main_thread() || !weblox_fiber_on_host_stack()) return;

  SDL_LockMutex(g_lock);
  ready_item* ready = g_ready_head;
  g_ready_head = g_ready_tail = NULL;
  SDL_UnlockMutex(g_lock);

  while (ready) {
    ready_item* next = ready->next;
    fprintf(stderr, "[futex] pump wake fiber=%p code=%u\n", ready->fiber,
            ready->code);
    weblox_fiber_resume_parked(ready->fiber, ready->code);
    free(ready);
    ready = next;
  }

  u64 now = SDL_GetTicksNS();
  ready_item* due = NULL;
  ready_item* due_tail = NULL;
  SDL_LockMutex(g_lock);
  for (js_waiter* wait = g_js_waiters; wait; wait = wait->next) {
    if (!wait->used) continue;
    u32 code = 0;
    int wake = 0;
    if (wait->deadline && now >= wait->deadline) {
      code = 2;  // timed out
      wake = 1;
    } else {
      // The published engine has no-op wasm Atomics.notify stubs, so a guest
      // word changed by a worker is the only wake signal for many waits.
      unsigned char* bytes = guest_ptr(wait->address, 4);
      u32 value = bytes ? (u32)load_word(bytes, 4) : wait->expected;
      if (value != wait->expected) {
        code = 0;  // woken by the guest word changing
        wake = 1;
      }
    }
    if (!wake) continue;
    wait->used = 0;
    ready_item* item = calloc(1, sizeof *item);
    if (!item) {
      wait->used = 1;
      continue;
    }
    item->fiber = wait->fiber;
    item->code = code;
    if (due_tail)
      due_tail->next = item;
    else
      due = item;
    due_tail = item;
  }
  SDL_UnlockMutex(g_lock);
  while (due) {
    ready_item* next = due->next;
    fprintf(stderr, "[futex] pump wake fiber=%p code=%u\n", due->fiber,
            due->code);
    weblox_fiber_resume_parked(due->fiber, due->code);
    free(due);
    due = next;
  }
}
