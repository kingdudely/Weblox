// Emscripten pthread mailbox delivery for the portable C++ host.
//
// Ported from v3-wasm2c/thread.c's mailbox half.  In the browser a worker
// rings another thread's mailbox word and posts a message; the target runs
// `_emscripten_check_mailbox` from a later event-loop task.  Here the ring is
// a condition variable for workers and a deferred item for the main thread.
//
// The module-side export is called through the generated ABI name only:
//     w2c_WebClient_0x5Femscripten_check_mailbox(inst)
// and is exposed to the host as weblox::mailbox::check(inst) (also available
// as extern "C" weblox_mailbox_check).

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>

extern "C" {
extern w2c_WebClient* g_wc_instance;
int weblox_deferred_run_one(w2c_WebClient* inst);
void weblox_defer_mailbox(void);
}

namespace weblox::mailbox {

// Guest pthread struct offsets from the C reference (relative to the guest
// pthread pointer, never absolute guest addresses).
constexpr u32 kPthreadStartedOffset = 112;

struct Box {
  u32 owner = 0;
  bool used = false;
  bool sig = false;
  std::mutex mtx;
  std::condition_variable cv;
};

namespace {

std::mutex g_registry_mtx;
std::deque<Box> g_boxes;
std::atomic<u32> g_main_pthread{0};

Box* box_for(u32 p, bool create) {
  std::lock_guard<std::mutex> lock(g_registry_mtx);
  for (auto& b : g_boxes)
    if (b.used && b.owner == p) return &b;
  if (create) {
    g_boxes.emplace_back();
    Box& b = g_boxes.back();
    b.used = true;
    b.owner = p;
    return &b;
  }
  return nullptr;
}

template <class F>
bool guarded(F&& fn) {
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET target;
  if (!wasm_rt_try(target)) {
    wasm_rt_set_unwind_target(&target);
    fn();
    wasm_rt_set_unwind_target(outer);
    return true;
  }
  wasm_rt_set_unwind_target(outer);
  std::fprintf(stderr, "[mailbox] wasm exception tag=%p bytes=%u\n",
               wasm_rt_exception_tag(), wasm_rt_exception_size());
  return false;
}

}  // namespace

thread_local bool t_is_main = true;

void check(w2c_WebClient* inst) {
  if (!inst) return;
  // Remember the main thread's guest pthread pointer on first use so worker
  // notifications can tell whether the target is the main thread.
  if (!g_main_pthread.load(std::memory_order_acquire)) {
    const u32 self = w2c_WebClient_pthread_self(inst);
    g_main_pthread.store(self, std::memory_order_release);
  }
  guarded([&] { w2c_WebClient_0x5Femscripten_check_mailbox(inst); });
}

void init_main(w2c_WebClient* inst) {
  if (!inst || !t_is_main) return;
  if (!g_main_pthread.load(std::memory_order_acquire)) {
    const u32 self = w2c_WebClient_pthread_self(inst);
    g_main_pthread.store(self, std::memory_order_release);
    std::fprintf(stderr, "[mailbox] main pthread=%x\n", self);
  }
}

bool is_main() { return t_is_main; }
void set_worker() { t_is_main = false; }

}  // namespace weblox::mailbox

extern "C" {

int weblox_host_is_main(void) {
  return weblox::mailbox::is_main() ? 1 : 0;
}

void weblox_mailbox_set_worker(void) { weblox::mailbox::set_worker(); }

void weblox_mailbox_init_main(w2c_WebClient* inst) {
  weblox::mailbox::init_main(inst);
}

void weblox_mailbox_check(w2c_WebClient* inst) { weblox::mailbox::check(inst); }

void w2c_env_0x5Femscripten_thread_mailbox_await(struct w2c_env*, u32 p) {
  weblox::mailbox::Box* b = weblox::mailbox::box_for(p, true);
  if (!b) return;
  weblox::guest::put_u32(p + weblox::mailbox::kPthreadStartedOffset, 1);  // "busy"
  std::fprintf(stderr, "[mailbox] await p=%x main=%d\n", p,
               weblox::mailbox::is_main() ? 1 : 0);
  if (weblox::mailbox::is_main()) {
    // Atomics.waitAsync is non-blocking: run what the browser event loop
    // would run (deferred callbacks and check_mailbox) and return at once.
    weblox_deferred_run_one(g_wc_instance);
    std::lock_guard<std::mutex> lock(b->mtx);
    b->sig = false;
    return;
  }
  std::unique_lock<std::mutex> lock(b->mtx);
  b->cv.wait(lock, [&] { return b->sig; });
  b->sig = false;
}

void w2c_env_0x5Femscripten_notify_mailbox_postmessage(struct w2c_env*, u32 target,
                                                        u32 self) {
  std::fprintf(stderr, "[mailbox] notify target=%x self=%x\n", target, self);
  const u32 main = weblox::mailbox::g_main_pthread.load(std::memory_order_acquire);
  if (main && target == main) {
    weblox_defer_mailbox();
    return;
  }
  weblox::mailbox::Box* b = weblox::mailbox::box_for(target, true);
  if (!b) return;
  {
    std::lock_guard<std::mutex> lock(b->mtx);
    b->sig = true;
  }
  b->cv.notify_all();
}

}  // extern "C"
