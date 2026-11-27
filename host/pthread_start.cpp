// Pthread worker re-entry: resolve Emscripten's start *table index*, build a
// private VMContext/table for the worker, run the guest TLS init sequence, and
// call the selected funcref.  See host/PTHREAD.md for the ABI evidence.
#define _GNU_SOURCE 1
#include "pthread_start.h"

#include "vmctx.h"
#include "imports.h"
#include "runtime_data.h"
#include "table.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>

namespace wb {
namespace {

bool debug_enabled() {
  static const bool on = [] {
    const char* s = std::getenv("WC_PTHR_DEBUG");
    return s && *s && std::strcmp(s, "0") != 0;
  }();
  return on;
}
#define PDBG(...)                                       \
  do {                                                  \
    if (debug_enabled()) {                              \
      std::fprintf(stderr, "[pthr-dbg] " __VA_ARGS__);   \
      std::fflush(stderr);                              \
    }                                                   \
  } while (0)

void* g_main_vmctx = nullptr;
// table index -> index in wb_table_init.  table.cpp is ordered by table index,
// but build the map explicitly so the lookup does not depend on that.
uint32_t* g_slot_map = nullptr;
uint32_t g_slot_map_size = 0;

// Guest exports (defined function index = wasm function index - 341).
// Cranelift emits these as GLOBAL HIDDEN in out.full.o and they become LOCAL
// in the linked executable, so dlsym(RTLD_DEFAULT) cannot find them: bind
// them directly instead.
extern "C" {
void _wasm_function_306(void*, void*);                          // _embind_initialize_bindings
void _wasm_function_498(void*, void*, uint32_t);                // set_stack_ptr
void _wasm_function_501(void*, void*, uint32_t, uint32_t);      // set_stack_limits
void _wasm_function_149599(void*, void*);                       // _emscripten_tls_init
uint32_t _wasm_function_149868(void*, void*);                   // pthread_self
void wb_call_worker_thread_init(void*, uint32_t);
}

// Wasmtime 50 wasm-call ABI: rdi = callee vmctx, rsi = caller vmctx, wasm
// arguments start at rdx; the 5th/6th stack arguments are callee-popped.
// Workers are not the main thread: (pthread, 0, 1, 1, 0, 0).
asm(
    ".text\n"
    ".globl wb_call_worker_thread_init\n"
    ".type wb_call_worker_thread_init, @function\n"
    "wb_call_worker_thread_init:\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  mov %rdi, %rax\n"
    "  mov %esi, %edx\n"          // arg0: pthread_ptr
    "  mov %rax, %rsi\n"          // caller vmctx
    "  mov %rax, %rdi\n"          // callee vmctx
    "  xor %ecx, %ecx\n"          // arg1: is_main = 0
    "  mov $1, %r8d\n"            // arg2: is_runtime_thread
    "  mov $1, %r9d\n"            // arg3: can_block
    "  pushq $0\n"                // arg5: init_stack
    "  pushq $0\n"                // arg4: stack_size
    "  call _wasm_function_151080\n"  // _emscripten_thread_init
    "  mov %rbp, %rsp\n"
    "  pop %rbp\n"
    "  ret\n"
    ".size wb_call_worker_thread_init, .-wb_call_worker_thread_init\n");

// ---- controlled traps on a worker -----------------------------------------
// Signal dispositions are process-wide.  Three separate guards in the host
// (vmctx.cpp, main.cpp's ExportTrapGuard, imports.cpp/loop.cpp's hook guards)
// install and *restore* handlers; the last one to restore wins even while
// detached workers are still running.  The old worker guard only covered the
// guest call window, so a fault delivered after that window reached
// vmctx.cpp's handler, which restored SIG_DFL and re-raised -- killing the
// whole process (exit 139) instead of just the worker.
//
// The fix has two parts:
//   1. Every worker thread owns two sigsetjmp targets (an inner one around the
//      guest call and an outer one for the thread's whole lifetime), and the
//      handler NEVER re-raises on a worker thread.
//   2. sigaction() for these four signals is interposed once the join driver
//      is registered: other host guards' actions are recorded as fallbacks but
//      never replace ours, so no restore can expose a SIG_DFL re-raise while
//      detached workers are still running.  Non-worker threads (main) are
//      delegated to the recorded action verbatim, so main.cpp's read-only
//      ExportTrapGuard keeps working unchanged.
constexpr int kSignals[4] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS};
constexpr int kNumSignals = 4;

thread_local sigjmp_buf tl_inner_jmp;
thread_local sigjmp_buf tl_outer_jmp;
thread_local volatile sig_atomic_t tl_inner_active = 0;
thread_local volatile sig_atomic_t tl_outer_active = 0;
thread_local volatile sig_atomic_t tl_signal = 0;
thread_local volatile sig_atomic_t tl_is_worker = 0;
thread_local long tl_tid = 0;

std::mutex g_signal_mutex;
// Displaced actions for non-worker threads; written under g_signal_mutex,
// read by the handler.  A torn read can only select an older valid action.
struct sigaction g_fallback[kNumSignals];
// Guards nest: installing a foreign action pushes the previous fallback, and
// restoring our action pops it.  Without the stack, a finished HookGuard's
// handler would stay as the fallback for main.cpp's ExportTrapGuard.
constexpr int kFallbackDepth = 16;
struct sigaction g_fallback_stack[kNumSignals][kFallbackDepth];
int g_fallback_depth[kNumSignals] = {};
std::atomic<int> g_live_workers{0};
std::atomic<bool> g_intercept_active{false};
pthread_t g_main_thread{};
bool g_main_recorded = false;
thread_local int tl_delegate_logs = 0;

void worker_trap_handler(int sig, siginfo_t*, void* uctx);
void install_worker_handlers();

// ---- async-signal-safe logging --------------------------------------------
void safe_write(const char* s, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(2, s + done, n - done);
    if (w <= 0) break;
    done += static_cast<size_t>(w);
  }
}
void safe_puts(char*& p, char* end, const char* s) {
  while (*s && p < end) *p++ = *s++;
}
void safe_dec(char*& p, char* end, unsigned long v) {
  char tmp[24];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v > 0) { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; }
  while (n > 0 && p < end) *p++ = tmp[--n];
}
void safe_hex(char*& p, char* end, uintptr_t v) {
  static const char hex[] = "0123456789abcdef";
  if (p + 2 > end) return;
  *p++ = '0'; *p++ = 'x';
  for (int i = 15; i >= 0 && p < end; --i)
    *p++ = hex[(v >> (4 * i)) & 0xf];
}

uintptr_t fault_pc(void* uctx) {
  if (auto* uc = static_cast<ucontext_t*>(uctx)) {
#if defined(__x86_64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.pc);
#endif
  }
  return 0;
}

// One write() so concurrent workers cannot interleave halves of a line (the
// old vmctx.cpp diagnostics were visibly garbled in /tmp/join-abi.log).
void log_worker_trap(int sig, uintptr_t pc, const char* outcome) {
  char buf[256];
  char* p = buf;
  char* end = buf + sizeof(buf);
  safe_puts(p, end, "[pthr-guard] tid=");
  safe_dec(p, end, static_cast<unsigned long>(tl_tid));
  safe_puts(p, end, " sig=");
  safe_dec(p, end, static_cast<unsigned long>(sig));
  safe_puts(p, end, " pc=");
  safe_hex(p, end, pc);
  safe_puts(p, end, " ");
  safe_puts(p, end, outcome);
  safe_puts(p, end, "\n");
  safe_write(buf, static_cast<size_t>(p - buf));
}

bool is_our_handler(const struct sigaction& sa) {
  return (sa.sa_flags & SA_SIGINFO)
             ? reinterpret_cast<void*>(sa.sa_sigaction) ==
                   reinterpret_cast<void*>(&worker_trap_handler)
             : false;
}

// Called when a guest throw has no landing pad on this worker.  Raising ud2
// here would be caught by whichever process-wide handler happens to be
// installed; on a worker that can be a re-raising handler.  Jump straight to
// this thread's guard instead.  Returns false for non-worker threads so the
// caller can keep the honest trap path there.
bool deliver_controlled_trap(int sig) {
  if (!tl_is_worker) return false;
  log_worker_trap(sig, 0, "-> controlled guest trap (no re-raise)");
  tl_signal = static_cast<sig_atomic_t>(sig);
  if (tl_inner_active) {
    tl_inner_active = 0;
    siglongjmp(tl_inner_jmp, sig);
  }
  if (tl_outer_active) {
    tl_outer_active = 0;
    siglongjmp(tl_outer_jmp, sig);
  }
  return false;
}

void worker_trap_handler(int sig, siginfo_t* info, void* uctx) {
  if (tl_tid == 0) tl_tid = static_cast<long>(::syscall(SYS_gettid));
  if (tl_is_worker) {
    // No signal disposition is changed and nothing is re-raised: the only
    // destination is a sigsetjmp frame on THIS thread's stack.
    log_worker_trap(sig, fault_pc(uctx), "-> controlled worker trap (no re-raise)");
    tl_signal = static_cast<sig_atomic_t>(sig);
    if (tl_inner_active) {
      tl_inner_active = 0;
      siglongjmp(tl_inner_jmp, sig);
    }
    if (tl_outer_active) {
      tl_outer_active = 0;
      siglongjmp(tl_outer_jmp, sig);
    }
    // Last resort (a fault during thread teardown): end this pthread only.
    ::pthread_exit(reinterpret_cast<void*>(static_cast<uintptr_t>(sig)));
  }

  // A fault on a pthread that never went through worker_thread_enter() still
  // must not kill the process: the host's other guards re-raise with SIG_DFL.
  // Only main gets delegated to its own guard (ExportTrapGuard / vmctx.cpp).
  const bool on_main = g_main_recorded && ::pthread_equal(::pthread_self(), g_main_thread);
  if (!on_main) {
    if (tl_delegate_logs++ < 4)
      log_worker_trap(sig, fault_pc(uctx),
                      "-> unregistered non-main thread: ending this thread only");
    ::pthread_exit(nullptr);
  }

  int idx = 0;
  for (; idx < kNumSignals; ++idx) if (kSignals[idx] == sig) break;
  if (idx == kNumSignals) return;
  const struct sigaction& old = g_fallback[idx];
  if (old.sa_flags & SA_SIGINFO) {
    old.sa_sigaction(sig, info, uctx);
  } else if (old.sa_handler == SIG_IGN) {
    return;
  } else if (old.sa_handler == SIG_DFL) {
    // Only for threads that never entered a worker; main's own guards are
    // installed before any guest call in main.cpp.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    ::raise(sig);
  } else {
    old.sa_handler(sig);
  }
}

void install_worker_handlers() {
  std::lock_guard<std::mutex> lock(g_signal_mutex);
  for (int i = 0; i < kNumSignals; ++i) {
    struct sigaction cur{};
    if (sigaction(kSignals[i], nullptr, &cur) != 0) continue;
    if (is_our_handler(cur)) continue;  // keep the displaced action intact
    g_fallback[i] = cur;
    struct sigaction sa{};
    sa.sa_sigaction = &worker_trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(kSignals[i], &sa, nullptr);
  }
}

// ---- sigaction interposition ------------------------------------------------
// main.cpp, imports.cpp and loop.cpp each install a guard around a guest call
// and restore the previous action afterwards.  Those restores are process-wide
// and do not know about our detached workers.  From pthread_register() onward
// we keep our handler installed: a foreign action is remembered as the
// fallback for the main thread but is never allowed to replace ours.  This is
// also race-free, unlike polling for and re-installing a displaced handler.
using SigactionFn = int (*)(int, const struct sigaction*, struct sigaction*);
SigactionFn g_real_sigaction = nullptr;
pthread_once_t g_sigaction_once = PTHREAD_ONCE_INIT;

void init_real_sigaction() {
  g_real_sigaction = reinterpret_cast<SigactionFn>(dlsym(RTLD_NEXT, "sigaction"));
}

int signal_index(int sig) {
  for (int i = 0; i < kNumSignals; ++i) if (kSignals[i] == sig) return i;
  return -1;
}

extern "C" int sigaction(int sig, const struct sigaction* act,
                         struct sigaction* old) {
  ::pthread_once(&g_sigaction_once, &init_real_sigaction);
  if (!g_real_sigaction) return -1;
  const int idx = signal_index(sig);
  if (act && idx >= 0 && g_intercept_active.load(std::memory_order_acquire)) {
    if (is_our_handler(*act)) {
      // A guard is restoring the action it captured on entry: unwind its
      // fallback before letting the real sigaction run.
      std::lock_guard<std::mutex> lock(g_signal_mutex);
      if (g_fallback_depth[idx] > 0)
        g_fallback[idx] = g_fallback_stack[idx][--g_fallback_depth[idx]];
      return g_real_sigaction(sig, act, old);
    }
    struct sigaction cur{};
    if (g_real_sigaction(sig, nullptr, &cur) != 0) return -1;
    if (old) *old = cur;
    {
      std::lock_guard<std::mutex> lock(g_signal_mutex);
      if (g_fallback_depth[idx] < kFallbackDepth)
        g_fallback_stack[idx][g_fallback_depth[idx]++] = g_fallback[idx];
      g_fallback[idx] = *act;
    }
    return 0;  // deliberately leave our handler installed
  }
  return g_real_sigaction(sig, act, old);
}

void worker_thread_enter() {
  tl_is_worker = 1;
  tl_tid = static_cast<long>(::syscall(SYS_gettid));
  tl_signal = 0;
  g_live_workers.fetch_add(1, std::memory_order_release);
}

void worker_thread_leave() {
  tl_outer_active = 0;
  tl_inner_active = 0;
  tl_is_worker = 0;
  g_live_workers.fetch_sub(1, std::memory_order_release);
}

// A fresh VMContext: code, builtins, memory and GC heap are shared with the
// main instance, but wasm globals, func refs and the indirect table are owned
// by this worker.  Every pointer that refers back to the instance is rebased.
uint8_t* make_worker_vmctx() {
  constexpr size_t kRt = WB_RUNTIME_DATA_COUNT;
  const size_t alloc = VMCTX_SIZE_OF_VMCTX +
                       kRt * (sizeof(void*) + sizeof(uint32_t));
  uint8_t* volatile vm = static_cast<uint8_t*>(std::calloc(alloc, 1));
  if (!vm) return nullptr;
  std::memcpy(vm, g_main_vmctx, alloc);

  auto* main_store = *reinterpret_cast<void**>(
      static_cast<uint8_t*>(g_main_vmctx) + VMCTX_OFF_STORE_CONTEXT);
  auto* store = new host::VMStoreContext;
  *store = *static_cast<host::VMStoreContext*>(main_store);
  void* store_ptr = store;
  std::memcpy(vm + VMCTX_OFF_STORE_CONTEXT, &store_ptr, sizeof(store_ptr));
  // The copied store context carries the main thread's wasm exit PC/FP and
  // unwind stack chain. A worker must not inherit those — the unwinder would
  // walk the main thread's frames.
  host::reset_wasm_exit_state(vm);

  // A real worker instantiates a fresh module: the mutable TLS/pthread globals
  // (1..7) start at zero, while the immutable constants (8..14) and the stack
  // pointer (0, overwritten from the pthread descriptor) are module defaults.
  std::memset(vm + VMCTX_OFF_GLOBALS + 16, 0, 7 * 16);

  auto* slots = reinterpret_cast<wb::FunctionImportSlot*>(
      vm + VMCTX_OFF_IMPORTED_FUNCTIONS);
  for (uint32_t i = 0; i < VMCTX_NUM_IMPORTED_FUNCTIONS; ++i)
    slots[i].vmctx = vm;
  auto* mem = reinterpret_cast<host::VMMemoryImport*>(
      vm + VMCTX_OFF_IMPORTED_MEMORIES);
  mem->vmctx = vm;

  auto* funcrefs = reinterpret_cast<host::VMFuncRef*>(
      vm + VMCTX_OFF_FUNC_REFS);
  for (uint32_t i = 0; i < VMCTX_NUM_ESCAPED_FUNCS; ++i) {
    if (funcrefs[i].wasm_call) funcrefs[i].vmctx = vm;
  }
  auto* startup = reinterpret_cast<host::VMFuncRef*>(
      vm + VMCTX_OFF_STARTUP_FUNC_REF);
  if (startup->wasm_call) startup->vmctx = vm;

  auto** entries = static_cast<host::VMFuncRef**>(
      std::calloc(wb_table_size, sizeof(host::VMFuncRef*)));
  auto* refs = static_cast<host::VMFuncRef*>(
      std::calloc(wb_table_size, sizeof(host::VMFuncRef)));
  if (!entries || !refs) {
    std::free(entries);
    std::free(refs);
    delete store;
    std::free(vm);
    return nullptr;
  }
  for (uint32_t i = 0; i < wb_table_init_count; ++i) {
    const WBTableEntry& e = wb_table_init[i];
    host::VMFuncRef& fr = refs[e.table_index];
    fr.wasm_call = e.wasm_call ? e.wasm_call
                               : wb::import_wasm_call(e.func_index + 1);
    fr.type_index = e.type_index;
    fr.vmctx = vm;
    entries[e.table_index] = &fr;
  }
  auto* table = reinterpret_cast<host::VMTableDefinition*>(
      vm + VMCTX_OFF_TABLES);
  table->base = reinterpret_cast<uint8_t*>(entries);
  table->current_elements = wb_table_size;
  return vm;
}

void free_worker_vmctx(uint8_t* vm) {
  if (!vm) return;
  auto* store = *reinterpret_cast<host::VMStoreContext**>(
      vm + VMCTX_OFF_STORE_CONTEXT);
  auto* table = reinterpret_cast<host::VMTableDefinition*>(
      vm + VMCTX_OFF_TABLES);
  if (table->base) {
    auto** entries = reinterpret_cast<host::VMFuncRef**>(table->base);
    for (uint32_t i = 0; i < wb_table_size; ++i) {
      if (entries[i]) {
        std::free(entries[i] - i);  // refs array base
        break;
      }
    }
    std::free(table->base);
  }
  delete store;
  std::free(vm);
}

void worker_body(uint32_t start, uint32_t arg, uint32_t pthread_ptr) {
  PDBG("body enter start=%u arg=%u ptr=%#x\n", start, arg, pthread_ptr);
  uint8_t* volatile vm = make_worker_vmctx();  // volatile: survives siglongjmp
  PDBG("vm=%p\n", static_cast<void*>(vm));
  if (!vm) {
    std::fprintf(stderr, "[pthr] worker vmctx allocation failed\n");
    return;
  }
  uint32_t tls_self = 0, func_index = 0;
  void* fn = nullptr;
  int trap = 0;

  // Inner guard: only the guest-call window.  Cleared before cleanup so a
  // fault in free_worker_vmctx() lands in the outer guard instead.
  tl_signal = 0;
  if (sigsetjmp(tl_inner_jmp, 1) != 0) {
    trap = static_cast<int>(tl_signal);
    tl_inner_active = 0;
  } else {
    tl_inner_active = 1;
    // The parent allocated the worker stack in linear memory: +48 is its top,
    // +52 its size (v2-wasmtime/src/imports/thread.rs, v3-wasm2c/thread.c).
    auto* mi = reinterpret_cast<host::VMMemoryImport*>(
        vm + VMCTX_OFF_IMPORTED_MEMORIES);
    uint8_t* base = mi->from ? mi->from->base : nullptr;
    auto ld32 = [&](uint32_t p) -> uint32_t {
      uint32_t v = 0;
      if (base && static_cast<uint64_t>(p) + 4 <= mi->from->current_length)
        std::memcpy(&v, base + p, 4);
      return v;
    };
    const uint32_t top = ld32(pthread_ptr + 48);
    const uint32_t size = ld32(pthread_ptr + 52);
    PDBG("stack top=%#x size=%#x\n", top, size);
    if (top && size && top > size) {
      _wasm_function_501(vm, vm, top, top - size);
      PDBG("limits set\n");
      _wasm_function_498(vm, vm, top);
      PDBG("stack ptr set\n");
    }

    // Same order as the browser worker (WebClient.js cmd=2) and Rust's
    // start_worker(): thread init, TLS init, pthread_self, per-worker embind.
    wb_call_worker_thread_init(vm, pthread_ptr);
    PDBG("thread_init done\n");
    _wasm_function_149599(vm, vm);
    PDBG("tls_init done\n");
    tls_self = _wasm_function_149868(vm, vm);
    PDBG("pthread_self=%#x\n", tls_self);
    _wasm_function_306(vm, vm);
    PDBG("embind done\n");

    // `start` is an index into __indirect_function_table, not a function index.
    if (start >= g_slot_map_size || g_slot_map[start] == 0xFFFFFFFFu) {
      std::fprintf(stderr, "[pthr] start=%u is not an initialized table entry\n",
                   start);
    } else {
      const WBTableEntry& e = wb_table_init[g_slot_map[start]];
      func_index = e.func_index;
      fn = e.wasm_call ? e.wasm_call
                       : wb::import_wasm_call(e.func_index + 1);
      PDBG("entry fn=%p\n", fn);
      if (fn) {
        using Entry = void (*)(void*, void*, uint32_t);
        reinterpret_cast<Entry>(fn)(vm, vm, arg);
        PDBG("entry returned\n");
      }
    }
    tl_inner_active = 0;
  }

  if (trap) {
    std::fprintf(stderr,
                 "[pthr] worker tid=%ld ptr=%#x start=%u trapped via signal %d "
                 "(contained by worker guard)\n",
                 tl_tid, pthread_ptr, start, trap);
  } else {
    std::fprintf(stderr,
                 "[pthr] worker tid=%ld ptr=%#x start=%u -> func=%u (%s) "
                 "tls=%#x done\n",
                 tl_tid, pthread_ptr, start, func_index,
                 fn ? "called" : "MISSING", tls_self);
  }
  free_worker_vmctx(vm);
}

}  // namespace

// Used by host/unwind.cpp when `throw_ref` has no guest landing pad on a
// worker: convert it to this thread's controlled trap instead of letting the
// guest raise ud2 into an arbitrary process-wide handler.
bool worker_raise_controlled(int sig) { return deliver_controlled_trap(sig); }

void pthread_register(void* main_vmctx) {
  // Called from main() before any guest thread is spawned: remember the one
  // thread that is allowed to delegate faults to main.cpp's trap guard.
  g_main_thread = ::pthread_self();
  g_main_recorded = true;
  // Install our handler before any guest call, then keep it installed: every
  // later guard action is recorded as a fallback by sigaction() above.
  install_worker_handlers();
  g_intercept_active.store(true, std::memory_order_release);
  g_main_vmctx = main_vmctx;
  g_slot_map_size = wb_table_size;
  g_slot_map = static_cast<uint32_t*>(
      std::malloc(wb_table_size * sizeof(uint32_t)));
  if (!g_slot_map) {
    g_slot_map_size = 0;
    return;
  }
  for (uint32_t i = 0; i < g_slot_map_size; ++i)
    g_slot_map[i] = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < wb_table_init_count; ++i) {
    const WBTableEntry& e = wb_table_init[i];
    if (e.table_index < g_slot_map_size) g_slot_map[e.table_index] = i;
  }
}

void pthread_entry(uint32_t start, uint32_t arg, uint32_t pthread_ptr) {
  if (!g_main_vmctx) {
    std::fprintf(stderr, "[pthr] entry called before pthread_register\n");
    return;
  }
  std::thread([start, arg, pthread_ptr] {
    worker_thread_enter();
    // Outer guard covers the worker's whole lifetime, including vmctx
    // allocation, cleanup and thread teardown.  The inner guard in
    // worker_body() is tried first so cleanup can still run.
    tl_signal = 0;
    if (sigsetjmp(tl_outer_jmp, 1) == 0) {
      tl_outer_active = 1;
      worker_body(start, arg, pthread_ptr);
    } else {
      std::fprintf(stderr,
                   "[pthr] tid=%ld worker start=%u trapped outside guest call "
                   "via signal %d (contained by outer worker guard)\n",
                   tl_tid, start, static_cast<int>(tl_signal));
    }
    worker_thread_leave();
  }).detach();
}

}  // namespace wb
