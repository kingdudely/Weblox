// Shared-memory atomics implemented with SDL3 mutexes and conditions.
//
// The main thread cannot sleep blind: another task on the same thread may own
// a lock this thread needs, and the guest event loop must keep turning.  So a
// waiting main-thread fiber pumps SDL events and services the deferred queue
// INLINE, on its own stack, and never launches a new root.  Worker threads
// just wait on the condition variable.
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

static SDL_Mutex* g_lock;
static waiter* g_waiters;

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

u32 weblox_atomic_wait_js(wasm_rt_shared_memory_t* memory, u32 address,
                          u32 expected, s64 timeout_ns) {
  return wait_for_value(memory, address, expected, timeout_ns, 4, 1);
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
  SDL_UnlockMutex(g_lock);
  if (woken)
    fprintf(stderr, "[atom] notify addr=%u woke=%u\n", address, woken);
  return woken;
}
