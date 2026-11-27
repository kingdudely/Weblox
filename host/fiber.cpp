// Native ucontext fibres + the wrapped `weblox::run_loop` driver that resumes
// a yielded guest turn.  host/loop.cpp is read-only, so the driver is replaced
// at link time with -Wl,--wrap=<mangled run_loop> (see host/Makefile).  The
// existing -Wl,--wrap=_wasm_function_176 turn hook in loop.cpp is kept intact:
// every fresh turn still goes through __wrap__wasm_function_176.
#define _GNU_SOURCE 1
#include "fiber.h"
#include "imports.h"
#include "rna_bridge.h"
#include "vmctx.h"

#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <setjmp.h>
#include <thread>

// Same declarations as host/loop.cpp (imports.h is read-only).
namespace wb {
struct DrainStats {
  uint32_t drains;
  uint32_t guest_calls;
  uint32_t readied;
  uint32_t traps;
};
DrainStats drain_stats();
uint32_t mailbox_drain(void* vmctx);
void mailbox_drain_if_pending(void* vmctx);
}  // namespace wb

namespace weblox {
namespace fiber {
namespace {

struct State {
  ucontext_t driver_ctx{};
  ucontext_t fiber_ctx{};
  void* stack = nullptr;
  std::size_t stack_size = 0;
  const std::function<int()>* turn = nullptr;
  bool active = false;
  bool suspended = false;
  bool turn_done = false;
  bool woken_flag = false;
  bool resumed_flag = false;
  int turn_result = 0;
};

thread_local State g_state;

constexpr std::size_t kStackBytes = 32u * 1024u * 1024u;

bool on_fiber_stack(const State& s) {
  char probe = 0;
  const uintptr_t p = reinterpret_cast<uintptr_t>(&probe);
  const uintptr_t lo = reinterpret_cast<uintptr_t>(s.stack);
  const uintptr_t hi = lo + s.stack_size;
  return s.stack && p >= lo && p < hi;
}

void ensure_stack(State& s) {
  if (s.stack) return;
  const long page = ::sysconf(_SC_PAGESIZE);
  const std::size_t guard = page > 0 ? static_cast<std::size_t>(page) : 4096u;
  void* base = ::mmap(nullptr, guard + kStackBytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    std::fprintf(stderr, "[fiber] mmap failed; guest turns stay on driver stack\n");
    std::fflush(stderr);
    return;
  }
  ::mprotect(base, guard, PROT_NONE);  // catch runaway guest recursion
  s.stack = static_cast<uint8_t*>(base) + guard;
  s.stack_size = kStackBytes;
}

void fiber_trampoline() {
  State& s = g_state;
  int result = 0;
  if (s.turn) {
    result = (*s.turn)();
  }
  s.turn_result = result;
  s.turn_done = true;
  // Returning from makecontext's entry function switches to uc_link
  // (the driver context saved by the most recent swapcontext into this
  // fibre), so no explicit swapcontext is needed on normal completion.
}

// Driver-side guard for completion pumping.  Distinct from main.cpp's
// ExportTrapGuard: a fault in host-side hook work must not longjmp into a
// suspended (non-running) fibre frame.
thread_local sigjmp_buf g_hook_jmp;
thread_local volatile sig_atomic_t g_hook_active = 0;
thread_local volatile sig_atomic_t g_hook_signal = 0;

void hook_trap_handler(int sig, siginfo_t*, void*) {
  if (g_hook_active) {
    g_hook_signal = static_cast<sig_atomic_t>(sig);
    g_hook_active = 0;
    siglongjmp(g_hook_jmp, sig);
  }
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, nullptr);
  ::raise(sig);
}

class HookGuard {
 public:
  HookGuard() {
    struct sigaction sa{};
    sa.sa_sigaction = &hook_trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &sa, &old_[i]);
  }
  ~HookGuard() {
    for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &old_[i], nullptr);
  }
  HookGuard(const HookGuard&) = delete;
  HookGuard& operator=(const HookGuard&) = delete;

  int call(void (*fn)(void*), void* vmctx) {
    g_hook_signal = 0;
    if (sigsetjmp(g_hook_jmp, 1) != 0) return g_hook_signal;
    g_hook_active = 1;
    fn(vmctx);
    g_hook_active = 0;
    return 0;
  }

 private:
  static constexpr int kSignals[4] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS};
  struct sigaction old_[4];
};

void pump_completions(void* vmctx) { weblox::rna_guest_tick(vmctx); }
void drain_mailboxes(void* vmctx) { wb::mailbox_drain_if_pending(vmctx); }

void guarded(void (*fn)(void*), void* vmctx, const char* what) {
  HookGuard guard;
  const int sig = guard.call(fn, vmctx);
  if (sig) host::note_error(what);
}

}  // namespace

// Forwarders used by the wrapped driver (which lives outside the anonymous
// namespace above).
void pump_driver(void* vmctx) { pump_completions(vmctx); }
void drain_driver(void* vmctx) { drain_mailboxes(vmctx); }
void guarded_driver(void (*fn)(void*), void* vmctx, const char* what) {
  guarded(fn, vmctx, what);
}

bool enabled() { return g_state.active && !g_state.turn_done; }
bool suspended() { return g_state.active && g_state.suspended; }

void yield() {
  State& s = g_state;
  // A re-entrant guest call made by the driver (RNA callback delivery) is on
  // the driver stack, not on the fibre.  Never switch that context.
  if (!s.active || s.suspended || !on_fiber_stack(s)) return;
  s.suspended = true;
  swapcontext(&s.fiber_ctx, &s.driver_ctx);
  // Resumed here; the driver has already set woken_flag for this slice.
  s.suspended = false;
  s.resumed_flag = true;
}

bool resuming() { return g_state.resumed_flag; }

void set_woken(bool woken) { g_state.woken_flag = woken; }
bool woken() { return g_state.woken_flag; }

void start(const std::function<int()>& turn) {
  State& s = g_state;
  ensure_stack(s);
  s.turn = &turn;
  s.turn_done = false;
  s.turn_result = 0;
  s.suspended = false;
  s.resumed_flag = false;
  s.woken_flag = false;
  s.active = true;
  if (!s.stack) {
    // mmap failed: fall back to running the turn on the caller's stack.
    s.turn_result = (*s.turn)();
    s.turn_done = true;
    return;
  }
  ::getcontext(&s.fiber_ctx);
  s.fiber_ctx.uc_stack.ss_sp = s.stack;
  s.fiber_ctx.uc_stack.ss_size = s.stack_size;
  s.fiber_ctx.uc_link = &s.driver_ctx;
  ::makecontext(&s.fiber_ctx, &fiber_trampoline, 0);
  ::swapcontext(&s.driver_ctx, &s.fiber_ctx);
}

void resume(bool woken_state) {
  State& s = g_state;
  if (!s.active || !s.suspended) return;
  s.woken_flag = woken_state;
  s.resumed_flag = false;
  ::swapcontext(&s.driver_ctx, &s.fiber_ctx);
}

bool turn_completed() { return g_state.active && g_state.turn_done; }
int turn_result() { return g_state.turn_result; }

void shutdown() {
  State& s = g_state;
  if (s.stack) {
    const long page = ::sysconf(_SC_PAGESIZE);
    const std::size_t guard = page > 0 ? static_cast<std::size_t>(page) : 4096u;
    ::munmap(static_cast<uint8_t*>(s.stack) - guard, guard + s.stack_size);
    s.stack = nullptr;
    s.stack_size = 0;
  }
  s.active = false;
  s.suspended = false;
  s.turn_done = false;
  s.turn = nullptr;
}

}  // namespace fiber
}  // namespace weblox

// Reimplementation of weblox::run_loop with the same observable behaviour
// (trace lines, completion pump, periodic RNA report, 2 ms cadence) except
// that a guest turn runs on a fibre and can be resumed instead of re-entered.
// The symbol name is the Itanium mangling of the declaration in main.cpp.
extern "C" int __wrap__ZN6weblox8run_loopERN4host9HostStateENSt6chrono10time_pointINS3_3_V212steady_clockENS3_8durationIlSt5ratioILl1ELl1000000000EEEEEERKSt8functionIFivEEPNS_9RnaBridgeERm(
    host::HostState& state, std::chrono::steady_clock::time_point deadline,
    const std::function<int()>& turn, weblox::RnaBridge* bridge,
    uint64_t& turns_out) {
  int trap_signal = 0;
  uint64_t resumes = 0;
  // The deadline is a turn boundary, not a reason to abandon a wasm call
  // that is parked on the fibre: asset fetches inside turn 1 can already
  // have consumed the requested window by the time slot 5 first yields.
  // Keep resuming a suspended turn until it returns, bounded by a hard
  // extension so a guest that never wakes cannot hang the driver.
  const auto hard_deadline = deadline + std::chrono::seconds(10);
  if (wb::trace_enabled()) {
    std::fprintf(stderr, "[fiber] run_loop entered (wrapped iteration + fibre)\n");
    std::fflush(stderr);
  }
  while (trap_signal == 0) {
    const bool parked = weblox::fiber::suspended();
    const auto now = std::chrono::steady_clock::now();
    if (now >= hard_deadline) break;
    if (!parked && now >= deadline) break;
    const bool done_before = weblox::fiber::turn_completed();
    bool completed = false;
    if (!done_before && !weblox::fiber::suspended()) {
      // Fresh turn: __wrap__wasm_function_176 in loop.cpp still pumps and
      // drains around the real guest iteration.
      if (wb::trace_enabled() && turns_out < 3) {
        std::fprintf(stderr, "[fiber] turn %llu begin\n",
                     static_cast<unsigned long long>(turns_out + 1));
        std::fflush(stderr);
      }
      weblox::fiber::start(turn);
      completed = weblox::fiber::turn_completed();
    } else if (weblox::fiber::suspended()) {
      // The guest is parked inside slot 5.  Pump host completions on the
      // driver stack before resuming, exactly as the page event loop does.
      const uint64_t cb_before =
          bridge ? bridge->stats().callbacks_delivered +
                       bridge->stats().callbacks_failed
                 : 0;
      const uint32_t ready_before = wb::drain_stats().readied;
      weblox::fiber::guarded_driver(&weblox::fiber::pump_driver, state.vmctx,
                                    "fiber hook: RNA completion pump trapped");
      weblox::fiber::guarded_driver(&weblox::fiber::drain_driver, state.vmctx,
                                    "fiber hook: guest drain trapped");
      const uint64_t cb_after =
          bridge ? bridge->stats().callbacks_delivered +
                       bridge->stats().callbacks_failed
                 : 0;
      const uint32_t ready_after = wb::drain_stats().readied;
      // Only a real host completion counts as "ok".  Slot 5 re-checks the
      // guest word first and reports "not-equal" if a writer changed it;
      // with neither event it reports "timed-out" for this slice.
      const bool woken = (cb_after != cb_before) ||
                         (ready_after != ready_before);
      weblox::fiber::resume(woken);
      completed = weblox::fiber::turn_completed();
    }

    if (!done_before && completed) {
      trap_signal = weblox::fiber::turn_result();
      ++turns_out;
      if (wb::trace_enabled() && turns_out <= 3) {
        std::fprintf(stderr, "[fiber] turn %llu end trap=%d\n",
                     static_cast<unsigned long long>(turns_out), trap_signal);
        std::fflush(stderr);
      }
    }

    if (turns_out > 0 && turns_out % 100 == 0 && bridge) {
      const weblox::RnaStats s = bridge->stats();
      std::printf("[rna-main] turns=%llu connects=%llu dgrams=%llu/%llu "
                  "ring=%llu drop=%llu cb=%llu/%llu status=%lld\n",
                  static_cast<unsigned long long>(turns_out),
                  static_cast<unsigned long long>(s.connects),
                  static_cast<unsigned long long>(s.datagrams_sent),
                  static_cast<unsigned long long>(s.datagrams_recv),
                  static_cast<unsigned long long>(s.ring_slots_written),
                  static_cast<unsigned long long>(s.ring_drops),
                  static_cast<unsigned long long>(s.callbacks_delivered),
                  static_cast<unsigned long long>(s.callbacks_failed),
                  static_cast<long long>(s.connect_status));
      std::fflush(stdout);
    }
    if (completed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } else {
      ++resumes;
      if (wb::trace_enabled() && resumes % 1000 == 0) {
        std::fprintf(stderr, "[fiber] resumes=%llu suspended=%d\n",
                     static_cast<unsigned long long>(resumes),
                     weblox::fiber::suspended() ? 1 : 0);
        std::fflush(stderr);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  weblox::fiber::shutdown();
  return trap_signal;
}
