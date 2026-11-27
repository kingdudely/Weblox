// Thread-safe deferred queue and the host's per-turn pump.
//
// Ported from v3-wasm2c/loop.c (deferred queue, mailbox drain, wc_pump_once).
// The engine's `emscripten_async_call` import queues a guest function index
// plus its argument; a worker ringing the main thread's mailbox queues a
// mailbox item instead.  Both are dispatched here, outside the wasm call
// that queued them, under the same wasm_rt_try guard the main loop uses.
//
// Public API for the main loop (one unit of work per turn):
//     namespace weblox::deferred { int pump(w2c_WebClient* inst); }
// An extern "C" alias weblox_deferred_pump(inst) is provided for hosts that
// prefer not to forward-declare the namespace.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "fiber_root.h"
#include "guest.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
extern w2c_WebClient* g_wc_instance;
void weblox_futex_pump(void);
void weblox_mailbox_check(w2c_WebClient* inst);
void weblox_mailbox_init_main(w2c_WebClient* inst);
}

namespace weblox::deferred {
namespace {

enum class Kind : unsigned char { Function, Mailbox };

struct Item {
  Kind kind = Kind::Function;
  u32 fn = 0;
  u32 arg = 0;
};

std::mutex g_mutex;
std::deque<Item> g_queue;

// A deferred callback may itself hit a futex wait.  Run each turn on a root
// fibre so that wait suspends instead of blocking the host stack.  Handles for
// roots that parked are kept until they finish; completed ones are reclaimed
// on the next pump.
constexpr std::size_t kMaxPendingRoots = 64;
thread_local std::vector<weblox::fibers::RootHandle> g_pending_roots;

void sweep_roots() {
  for (auto it = g_pending_roots.begin(); it != g_pending_roots.end();) {
    if (weblox::fibers::root_done(*it)) {
      weblox::fibers::root_free(*it);
      it = g_pending_roots.erase(it);
    } else {
      ++it;
    }
  }
}

// The browser wraps every task in try/catch.  Without an unwind target,
// wasm_rt_throw longjmps through a null pointer.
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
  std::fprintf(stderr, "[deferred] wasm exception tag=%p bytes=%u\n",
               wasm_rt_exception_tag(), wasm_rt_exception_size());
  return false;
}

void push(Kind kind, u32 fn, u32 arg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_queue.push_back(Item{kind, fn, arg});
}

bool pop(Item& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.empty()) return false;
  out = g_queue.front();
  g_queue.pop_front();
  return true;
}

bool pending() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return !g_queue.empty();
}

int run_item(w2c_WebClient* inst) {
  Item item;
  if (!inst || !pop(item)) return 0;
  if (item.kind == Kind::Mailbox) {
    std::fprintf(stderr, "[mailbox] pump: drain pending messages\n");
    guarded([&] { weblox_mailbox_check(inst); });
    return 1;
  }
  auto& table = inst->w2c_0x5F_indirect_function_table;
  // Emscripten leaves holes in the table for lazily initialised functions;
  // never call an out-of-range or null funcref.
  if (item.fn >= table.size || !table.data || !table.data[item.fn].func) {
    std::fprintf(stderr, "[deferred] skip fn=%u (table size=%u)\n",
                 item.fn, table.size);
    return 1;
  }
  wasm_rt_funcref_t& e = table.data[item.fn];
  std::fprintf(stderr, "[deferred] call fn=%u arg=%u\n", item.fn, item.arg);
  guarded([&] {
    reinterpret_cast<void (*)(void*, u32)>(e.func)(e.module_instance, item.arg);
  });
  return 1;
}

}  // namespace

void detail_push(u32 fn, u32 arg) { push(Kind::Function, fn, arg); }
void detail_push_mailbox() { push(Kind::Mailbox, 0, 0); }
int detail_run_one(w2c_WebClient* inst) { return run_item(inst); }

// Run one queued callback (or one mailbox drain) plus any futex wakeups that
// are ready on this thread.  Returns the number of units processed.
int pump(w2c_WebClient* inst) {
  w2c_WebClient* i = inst ? inst : g_wc_instance;
  sweep_roots();
  weblox_mailbox_init_main(i);
  weblox_futex_pump();
  // Already on a root fibre (e.g. re-entered from a guest mailbox await):
  // spawning another root here would stack contexts on the same thread.
  if (weblox::fibers::on_root()) return run_item(i);
  if (pending() && g_pending_roots.size() < kMaxPendingRoots) {
    auto handle = weblox::fibers::run_on_root(i, [i] { run_item(i); });
    if (handle) {
      g_pending_roots.push_back(handle);
      return 1;  // the item ran (or parked) on its own fibre
    }
  }
  return run_item(i);
}

}  // namespace weblox::deferred

extern "C" {

void w2c_env_emscripten_async_call(struct w2c_env*, u32 fn, u32 arg, u32) {
  std::fprintf(stderr, "[deferred] queue fn=%u arg=%u\n", fn, arg);
  weblox::deferred::detail_push(fn, arg);
}

int weblox_deferred_pump(w2c_WebClient* inst) {
  return weblox::deferred::pump(inst);
}

// Mailbox notifications from worker threads do not carry a guest callback:
// they ask the main thread to run the module's check_mailbox export.
void weblox_defer_mailbox(void) {
  weblox::deferred::detail_push_mailbox();
}

int weblox_deferred_run_one(w2c_WebClient* inst) {
  return weblox::deferred::detail_run_one(inst ? inst : g_wc_instance);
}

}  // extern "C"
