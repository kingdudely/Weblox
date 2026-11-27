// Guest drain hook for the C++ host's main-loop turn.
//
// `host/main.cpp` owns the bounded deadline loop and calls the real
// `main_loop_iteration` export once per turn; that file is out of scope for
// this change.  In the branch this hook was developed on, `run_loop()` is a
// separate driver (host/LOOP.md).  Here the equivalent insertion point is a
// linker wrap of the iteration call: the Makefile links this translation unit
// with `-Wl,--wrap=_wasm_function_176`, so every turn taken by the existing
// driver passes through `__wrap__wasm_function_176` first.
//
// Ordering matches v2-wasmtime/src/main.rs + imports/mailbox.rs: pump the
// deferred completions (RNA GuestTick), then drain every accepted mailbox
// ring by re-entering the guest's `_emscripten_check_mailbox`, then run the
// actual iteration.  A second drain after the turn catches rings posted by
// that turn.  All guest re-entry is on the thread that called the iteration;
// the RNA pumper thread and pthread workers never re-enter wasm here.  See
// host/DRAIN.md.
#define _GNU_SOURCE 1
#include "imports.h"
#include "rna_bridge.h"
#include "vmctx.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <setjmp.h>

extern "C" {
void _wasm_function_176(void*, void*);         // main_loop_iteration (wasm 517)
void __real__wasm_function_176(void*, void*);  // supplied by ld --wrap
}

// These two live in imports.cpp (imports.h is not modified by this change).
namespace wb {
struct DrainStats {
  uint32_t drains;
  uint32_t guest_calls;
  uint32_t readied;
  uint32_t traps;
};
DrainStats drain_stats();
uint32_t mailbox_drain(void* vmctx);
}  // namespace wb

namespace weblox {
namespace {

std::atomic<uint64_t> g_turns{0};
std::atomic<uint64_t> g_hooks{0};
std::atomic<uint64_t> g_readied{0};

// Trap guard for host-side hook work only.  The real guest iteration remains
// under the driver's own guard in main.cpp; a trapped drain is reported as a
// controlled error and does not stop the turn.  The jmp buffer is
// thread-local so a signal on a worker/pumper thread cannot jump here.
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

  // Returns 0 on normal return, or the trapping signal number.
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
void drain_mailboxes(void* vmctx) { wb::mailbox_drain(vmctx); }

void guarded(void (*fn)(void*), void* vmctx, const char* what) {
  HookGuard guard;
  const int sig = guard.call(fn, vmctx);
  if (sig) host::note_error(what);
  g_hooks.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace
}  // namespace weblox

namespace weblox {

// Bounded driver for the engine's main loop.  The caller supplies the guarded
// turn (`main_loop_iteration` under main.cpp's ExportTrapGuard); this function
// owns the deadline, the turn count and the periodic RNA progress report.
// Each turn still goes through __wrap__wasm_function_176 above, so the RNA
// completion pump and both mailbox drains run on this thread.  Returns 0 when
// the deadline was reached, or the trapping signal of the first bad turn.
int run_loop(host::HostState& state,
             std::chrono::steady_clock::time_point deadline,
             const std::function<int()>& turn, RnaBridge* bridge,
             uint64_t& turns_out) {
  (void)state;  // the turn captures the same HostState; kept in the API for
                // symmetry with the other guest-entry helpers.
  int trap_signal = 0;
  if (wb::trace_enabled()) {
    std::fprintf(stderr, "[loop] run_loop entered (wrapped iteration)\n");
    std::fflush(stderr);
  }
  while (std::chrono::steady_clock::now() < deadline && trap_signal == 0) {
    if (wb::trace_enabled() && turns_out < 3) {
      std::fprintf(stderr, "[loop] turn %llu begin\n",
                   static_cast<unsigned long long>(turns_out + 1));
      std::fflush(stderr);
    }
    trap_signal = turn();
    ++turns_out;
    if (wb::trace_enabled() && turns_out <= 3) {
      std::fprintf(stderr, "[loop] turn %llu end trap=%d\n",
                   static_cast<unsigned long long>(turns_out), trap_signal);
      std::fflush(stderr);
    }
    if (turns_out % 100 == 0 && bridge) {
      const RnaStats s = bridge->stats();
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
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return trap_signal;
}

}  // namespace weblox

extern "C" void __wrap__wasm_function_176(void* callee_vmctx, void* caller_vmctx) {
  if (callee_vmctx) {
    // The driver already pumps one RNA completion just before this call;
    // pumping here too is idempotent and matches LOOP.md's bracketed turn.
    weblox::guarded(&weblox::pump_completions, callee_vmctx,
                    "mailbox hook: RNA completion pump trapped");
    const uint64_t before = wb::drain_stats().readied;
    weblox::guarded(&weblox::drain_mailboxes, callee_vmctx,
                    "mailbox hook: guest drain trapped");
    weblox::g_readied.fetch_add(wb::drain_stats().readied - before,
                                std::memory_order_relaxed);
  }

  // The real guest turn is deliberately outside this hook's guard: the
  // driver's ExportTrapGuard owns it and reports iter_trap to the caller.
  __real__wasm_function_176(callee_vmctx, caller_vmctx);
  const uint64_t turns = weblox::g_turns.fetch_add(1, std::memory_order_relaxed) + 1;

  if (callee_vmctx) {
    const uint64_t before = wb::drain_stats().readied;
    weblox::guarded(&weblox::drain_mailboxes, callee_vmctx,
                    "mailbox hook: guest drain trapped after turn");
    weblox::g_readied.fetch_add(wb::drain_stats().readied - before,
                                std::memory_order_relaxed);
  }
  if (wb::trace_enabled() && turns % 200 == 0) {
    const wb::DrainStats s = wb::drain_stats();
    std::fprintf(stderr,
                 "[drain] turns=%llu hooks=%llu drains=%u guest_calls=%u "
                 "readied=%u traps=%u\n",
                 static_cast<unsigned long long>(turns),
                 static_cast<unsigned long long>(
                     weblox::g_hooks.load(std::memory_order_relaxed)),
                 s.drains, s.guest_calls,
                 static_cast<unsigned>(
                     weblox::g_readied.load(std::memory_order_relaxed)),
                 s.traps);
  }
}
