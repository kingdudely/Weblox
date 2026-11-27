// Atomics.waitAsync-style futex wait for the portable C++ host.
//
// Ported from v3-wasm2c/futex.c.  The import parks the current guest stack as
// a fiber slot when it is called on the main thread inside a suspended fibre
// (the JSPI case), and returns control to the fibre's creator.  The deferred
// pump resumes the slot when the word is notified or the timeout expires.
// Threads without a guest fibre (workers) block on a condition variable.
//
// Return codes match the browser glue: 0 woken, 1 not-equal, 2 timed out.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
int weblox_host_is_main(void);
void* weblox_fiber_park_begin(void);
void weblox_fiber_park(void* slot);
void weblox_fiber_resume_slot(void* slot, u32 code);
void weblox_fiber_slot_free(void* slot);
u32 weblox_fiber_slot_ret(void* slot);
int weblox_fiber_slot_is_root(void* slot);
}

namespace weblox::futex {
namespace {

constexpr int kWaiters = 128;

struct Waiter {
  u32 addr = 0;
  void* slot = nullptr;        // parked guest point, nullptr for a blocked thread
  double deadline = 0.0;       // 0 == infinite
  bool used = false;
};

// A fibre is another stack on the same thread, so a plain mutex would
// deadlock when two fibres meet here.  std::recursive_mutex lets the second
// fibre through; no other thread writes this table while it is held.
std::recursive_mutex g_mtx;
std::condition_variable_any g_cv;
Waiter g_waiters[kWaiters];

std::mutex g_ready_mtx;
std::deque<std::pair<void*, u32>> g_ready;

u32 read_word(u32 addr) {
  u32 value = 0;
  if (auto* p = guest::ptr(addr, 4)) std::memcpy(&value, p, 4);
  return value;
}

void ready_push(void* slot, u32 code) {
  std::lock_guard<std::mutex> lock(g_ready_mtx);
  g_ready.emplace_back(slot, code);
}

}  // namespace

// Wake up to `count` waiters on `addr`, queueing guest suspensions for the
// next pump turn and signalling threads blocked on the condition variable.
u32 notify(u32 addr, u32 count) {
  int woken = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (int i = 0; i < kWaiters && woken < static_cast<int>(count); ++i) {
      Waiter& w = g_waiters[i];
      if (!w.used || w.addr != addr) continue;
      void* slot = w.slot;
      w = Waiter{};
      if (slot) ready_push(slot, 0);
      ++woken;
    }
  }
  if (woken) g_cv.notify_all();
  std::fprintf(stderr, "[futex] notify addr=%x count=%u woken=%d\n",
               addr, count, woken);
  return static_cast<u32>(woken);
}

// Called by the deferred pump on the main thread.  Never resume a slot while
// holding g_mtx: resumed guest code may wait on the same futex again.
void pump() {
  for (;;) {
    void* slot = nullptr;
    u32 code = 0;
    {
      std::lock_guard<std::mutex> lock(g_ready_mtx);
      if (g_ready.empty()) break;
      slot = g_ready.front().first;
      code = g_ready.front().second;
      g_ready.pop_front();
    }
    if (slot) {
      std::fprintf(stderr, "[futex] resume slot=%p code=%u\n", slot, code);
      weblox_fiber_resume_slot(slot, code);
    }
  }

  const double now = static_cast<double>(weblox::platform::now_ms());
  std::vector<void*> expired;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (int i = 0; i < kWaiters; ++i) {
      Waiter& w = g_waiters[i];
      if (w.used && w.slot && w.deadline && w.deadline <= now) {
        expired.push_back(w.slot);
        w = Waiter{};
      }
    }
  }
  for (void* slot : expired) {
    std::fprintf(stderr, "[futex] timeout slot=%p\n", slot);
    weblox_fiber_resume_slot(slot, 2);
  }
}

}  // namespace weblox::futex

extern "C" {

u32 w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi(struct w2c_env*, u32 addr,
                                                    u32 expected, f64 timeout) {
  const u32 current = weblox::futex::read_word(addr);
  if (current != expected) return 1;
  const bool infinite = !(timeout > 0.0) || timeout >= 1e12;
  const double deadline =
      infinite ? 0.0
               : static_cast<double>(weblox::platform::now_ms()) + timeout;
  std::fprintf(stderr, "[futex] wait addr=%x want=%x now=%x timeout=%.1f\n",
               addr, expected, current, timeout);

  int index = -1;
  {
    std::lock_guard<std::recursive_mutex> lock(weblox::futex::g_mtx);
    for (int i = 0; i < weblox::futex::kWaiters; ++i)
      if (!weblox::futex::g_waiters[i].used) { index = i; break; }
    if (index >= 0) {
      weblox::futex::g_waiters[index] = weblox::futex::Waiter{
          addr, nullptr, deadline, true};
    }
  }

  // Main-thread wasm runs on a host root fibre.  A wait there suspends
  // exactly like JSPI: park the stack/unwind state and return to the event
  // loop until the word changes (or the deadline is pumped).
  if (weblox_host_is_main()) {
    void* slot = weblox_fiber_park_begin();
    if (slot && index >= 0) {
      {
        std::lock_guard<std::recursive_mutex> lock(weblox::futex::g_mtx);
        weblox::futex::g_waiters[index].slot = slot;
      }
      std::fprintf(stderr, "[futex] park addr=%x slot=%p (JSPI suspend)\n",
                   addr, slot);
      weblox_fiber_park(slot);  // returns here only when the pump resumes us
      std::lock_guard<std::recursive_mutex> lock(weblox::futex::g_mtx);
      const bool still = weblox::futex::g_waiters[index].used;
      weblox::futex::g_waiters[index] = weblox::futex::Waiter{};
      const u32 result = weblox_fiber_slot_ret(slot);
      if (still || !weblox_fiber_slot_is_root(slot)) weblox_fiber_slot_free(slot);
      std::fprintf(stderr, "[futex] wake addr=%x code=%u\n", addr, result);
      return result;
    }
    if (slot) weblox_fiber_slot_free(slot);
    std::fprintf(stderr,
                 "[futex] main stack has no root fibre: falling back to a "
                 "bounded poll; wire weblox::fibers::run_on_root\n");
  }

  // No parked slot available (workers, or an unwired host stack): block on
  // the condvar.  The main thread uses a short poll so deferred work and
  // mailbox messages keep flowing even if no notify arrives.
  for (;;) {
    {
      std::unique_lock<std::recursive_mutex> lock(weblox::futex::g_mtx);
      if (weblox::futex::read_word(addr) != expected) {
        if (index >= 0) weblox::futex::g_waiters[index] = weblox::futex::Waiter{};
        return 1;
      }
      const double now = static_cast<double>(weblox::platform::now_ms());
      if (!infinite && now >= deadline) {
        if (index >= 0) weblox::futex::g_waiters[index] = weblox::futex::Waiter{};
        return 2;
      }
      if (infinite) {
        if (weblox_host_is_main()) {
          weblox::futex::g_cv.wait_for(lock, std::chrono::milliseconds(1));
        } else {
          weblox::futex::g_cv.wait(lock);
        }
      } else {
        const auto remaining = std::chrono::milliseconds(
            static_cast<long long>(deadline - now) + 1);
        const auto slice = weblox_host_is_main() && remaining > std::chrono::milliseconds(1)
                               ? std::chrono::milliseconds(1)
                               : remaining;
        weblox::futex::g_cv.wait_for(lock, slice);
      }
    }
  }
}

void weblox_futex_pump(void) { weblox::futex::pump(); }

u32 weblox_futex_notify(u32 addr, u32 count) {
  return weblox::futex::notify(addr, count);
}

}  // extern "C"
