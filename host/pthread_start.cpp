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
// vmctx.cpp's process-wide trap handler longjmps into a buffer owned by the
// main thread.  A worker trap must never do that.  Install a handler that
// longjmps to a thread-local buffer when a worker is active and otherwise
// delegates to the previous handler verbatim.
constexpr int kSignals[4] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS};
struct sigaction g_prev[4];
std::mutex g_signal_mutex;
std::atomic<int> g_signal_users{0};
thread_local sigjmp_buf tl_jmp;
thread_local volatile sig_atomic_t tl_active = 0;
thread_local volatile sig_atomic_t tl_signal = 0;

void worker_trap_handler(int sig, siginfo_t* info, void* uctx) {
  if (tl_active) {
    if (debug_enabled()) {
      char buf[160];
      uintptr_t pc = 0;
      if (auto* uc = static_cast<ucontext_t*>(uctx)) {
#if defined(__x86_64__)
        pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#endif
      }
      int n = std::snprintf(buf, sizeof(buf),
                            "[pthr-dbg] trap sig=%d pc=%p addr=%p\n", sig,
                            reinterpret_cast<void*>(pc),
                            info ? info->si_addr : nullptr);
      ssize_t ignored = ::write(2, buf, static_cast<size_t>(n > 0 ? n : 0));
      (void)ignored;
    }
    tl_signal = static_cast<sig_atomic_t>(sig);
    siglongjmp(tl_jmp, sig);
  }
  int idx = 0;
  for (; idx < 4; ++idx) if (kSignals[idx] == sig) break;
  if (idx == 4) return;
  const struct sigaction& old = g_prev[idx];
  if (old.sa_flags & SA_SIGINFO) {
    old.sa_sigaction(sig, info, uctx);
  } else if (old.sa_handler == SIG_IGN) {
    return;
  } else if (old.sa_handler == SIG_DFL) {
    sigaction(sig, &old, nullptr);
    raise(sig);
  } else {
    old.sa_handler(sig);
  }
}

class SignalScope {
 public:
  SignalScope() {
    std::lock_guard<std::mutex> lock(g_signal_mutex);
    if (g_signal_users.fetch_add(1) == 0) {
      for (int i = 0; i < 4; ++i) sigaction(kSignals[i], nullptr, &g_prev[i]);
      struct sigaction sa {};
      sa.sa_sigaction = &worker_trap_handler;
      sa.sa_flags = SA_SIGINFO | SA_NODEFER;
      sigemptyset(&sa.sa_mask);
      for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &sa, nullptr);
    }
  }
  ~SignalScope() {
    if (g_signal_users.fetch_sub(1) == 1) {
      std::lock_guard<std::mutex> lock(g_signal_mutex);
      for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &g_prev[i], nullptr);
    }
  }
};

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
  SignalScope signals;
  uint32_t tls_self = 0, func_index = 0;
  void* fn = nullptr;
  int trap = 0;

  tl_signal = 0;
  if (sigsetjmp(tl_jmp, 1) != 0) {
    trap = static_cast<int>(tl_signal);
  } else {
    tl_active = 1;
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
    tl_active = 0;
  }

  if (trap) {
    std::fprintf(stderr, "[pthr] worker ptr=%#x start=%u trapped via signal %d\n",
                 pthread_ptr, start, trap);
  } else {
    std::fprintf(stderr,
                 "[pthr] worker ptr=%#x start=%u -> func=%u (%s) tls=%#x done\n",
                 pthread_ptr, start, func_index, fn ? "called" : "MISSING",
                 tls_self);
  }
  free_worker_vmctx(vm);
}

}  // namespace

void pthread_register(void* main_vmctx) {
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
    worker_body(start, arg, pthread_ptr);
  }).detach();
}

}  // namespace wb
