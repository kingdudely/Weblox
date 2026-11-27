// Emscripten pthread mailbox: one thread asks another to run a JS-side task.
//
// The ring is a condition variable instead of Atomics.notify on a shared word.
// The main thread never blocks (Chrome delivers the ring as a message and
// returns to wasm immediately), so it just pumps the deferred queue.
#include "deferred.h"
#include "fibers.h"
#include "guest.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define MAILBOX_LIMIT 64

typedef struct mailbox {
  u32 id;
  SDL_Mutex* lock;
  SDL_Condition* cond;
  int used;
  int sig;
} mailbox;

static mailbox g_boxes[MAILBOX_LIMIT];
static SDL_Mutex* g_registry;
static u32 g_main_id;

int weblox_mailbox_init(void) {
  if (!g_registry) g_registry = SDL_CreateMutex();
  return g_registry != NULL;
}

void weblox_mailbox_set_main(u32 id) {
  g_main_id = id;
  fprintf(stderr, "[mailbox] main id=%u\n", id);
}

static mailbox* mb_for(u32 id, int create) {
  if (!weblox_mailbox_init()) return NULL;
  SDL_LockMutex(g_registry);
  mailbox* free_slot = NULL;
  for (int i = 0; i < MAILBOX_LIMIT; ++i) {
    if (g_boxes[i].used && g_boxes[i].id == id) {
      SDL_UnlockMutex(g_registry);
      return &g_boxes[i];
    }
    if (!g_boxes[i].used && !free_slot) free_slot = &g_boxes[i];
  }
  if (create && free_slot) {
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->lock = SDL_CreateMutex();
    free_slot->cond = SDL_CreateCondition();
    if (free_slot->lock && free_slot->cond) {
      free_slot->used = 1;
      free_slot->id = id;
    } else {
      SDL_DestroyMutex(free_slot->lock);
      SDL_DestroyCondition(free_slot->cond);
      free_slot = NULL;
    }
  } else {
    free_slot = NULL;
  }
  SDL_UnlockMutex(g_registry);
  return free_slot;
}

int weblox_mailbox_signal(u32 id) {
  mailbox* box = mb_for(id, 0);
  if (!box) return 0;
  SDL_LockMutex(box->lock);
  box->sig = 1;
  SDL_SignalCondition(box->cond);
  SDL_UnlockMutex(box->lock);
  return 1;
}

void weblox_mailbox_cleanup(u32 id) {
  if (!weblox_mailbox_init()) return;
  SDL_LockMutex(g_registry);
  for (int i = 0; i < MAILBOX_LIMIT; ++i) {
    if (!g_boxes[i].used || g_boxes[i].id != id) continue;
    SDL_DestroyCondition(g_boxes[i].cond);
    SDL_DestroyMutex(g_boxes[i].lock);
    memset(&g_boxes[i], 0, sizeof g_boxes[i]);
    break;
  }
  SDL_UnlockMutex(g_registry);
}

void w2c_env_0x5Femscripten_thread_mailbox_await(struct w2c_env* env, u32 p) {
  (void)env;
  mailbox* box = mb_for(p, 1);
  if (!box) return;
  fprintf(stderr, "[mailbox] await id=%u main=%d\n", p,
          weblox_fiber_on_main_thread());
  if (weblox_fiber_on_main_thread()) {
    // Atomics.waitAsync style: arm the wait, run a loop turn, return at once.
    w2c_WebClient* inst = weblox_fiber_instance();
    weblox_deferred_pump(inst);
    SDL_LockMutex(box->lock);
    box->sig = 0;
    SDL_UnlockMutex(box->lock);
    return;
  }
  SDL_LockMutex(box->lock);
  while (!box->sig) SDL_WaitCondition(box->cond, box->lock);
  box->sig = 0;
  SDL_UnlockMutex(box->lock);
}

void w2c_env_0x5Femscripten_notify_mailbox_postmessage(struct w2c_env* env,
                                                       u32 target, u32 self) {
  (void)env;
  fprintf(stderr, "[mailbox] notify target=%u self=%u\n", target, self);
  if (!target || target == g_main_id || weblox_fiber_on_main_thread()) {
    weblox_deferred_mailbox();
    return;
  }
  if (target == self) {
    weblox_mailbox_signal(target);
    return;
  }
  weblox_mailbox_signal(target);
}
