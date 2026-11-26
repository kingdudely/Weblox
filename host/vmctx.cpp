// Minimal Wasmtime-ABI host glue: VMContext setup, host-owned shared linear
// memory, builtin libcalls, import stubs and trap-to-error signal handling.
#define _GNU_SOURCE 1
#include "vmctx.h"
#include "imports.h"
#include "runtime_data.h"

#include <csignal>
#include <setjmp.h>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <limits>

#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

namespace host {

namespace {

HostState* g_state = nullptr;
volatile sig_atomic_t g_in_wasm = 0;
volatile sig_atomic_t g_trap_signal = 0;
sigjmp_buf g_trap_jmp;

// async-signal-safe-ish diagnostics: write() only, hand-rolled hex.
void safe_write(const char* s, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(2, s + done, n - done);
    if (w <= 0) break;
    done += static_cast<size_t>(w);
  }
}
void safe_puts(const char* s) { safe_write(s, std::strlen(s)); }
void safe_hex(uintptr_t v) {
  char buf[2 + 16];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; ++i) {
    unsigned nib = static_cast<unsigned>((v >> (60 - 4 * i)) & 0xf);
    buf[2 + i] = "0123456789abcdef"[nib];
  }
  safe_write(buf, sizeof(buf));
}

void trap_handler(int sig, siginfo_t* info, void* uctx) {
  uintptr_t pc = 0;
  if (auto* uc = static_cast<ucontext_t*>(uctx)) {
#if defined(__x86_64__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#endif
  }
  safe_puts("[wasm-host] TRAP signal=");
  char num[8];
  int n = 0;
  int v = sig;
  if (v == 0) num[n++] = '0';
  while (v > 0) {
    num[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  }
  while (n > 0) safe_write(num + --n, 1);
  safe_puts(" pc=");
  safe_hex(pc);
  safe_puts(" fault_addr=");
  safe_hex(reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr));
  safe_puts("\n");

  if (g_in_wasm) {
    g_trap_signal = static_cast<sig_atomic_t>(sig);
    g_in_wasm = 0;
    siglongjmp(g_trap_jmp, sig);
  }
  // Not a wasm trap: restore default and re-raise rather than corrupt state.
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, nullptr);
  ::raise(sig);
}

bool install_signal_handlers() {
  struct sigaction sa{};
  sa.sa_sigaction = trap_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGFPE, SIGILL, SIGBUS}) {
    if (sigaction(sig, &sa, nullptr) != 0) {
      std::fprintf(stderr, "[wasm-host] sigaction(%d) failed\n", sig);
      return false;
    }
  }
  return true;
}

// ---- the eight builtins actually referenced by out.full.o -----------------

// Wasmtime builtins take absolute host pointers, already bounds-checked by
// Cranelift (see wasmtime-environ's builtin.rs).  A data-segment source is a
// host pointer, not an offset into linear memory.
extern "C" void builtin_memory_copy(void*, uint64_t dst, uint64_t src, uint64_t len) {
  std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src),
               static_cast<size_t>(len));
}

extern "C" void builtin_memory_fill(void*, uint64_t dst, uint64_t val, uint64_t len) {
  std::memset(reinterpret_cast<void*>(dst), static_cast<int>(val & 0xff),
              static_cast<size_t>(len));
}

extern "C" uint64_t builtin_memory_atomic_notify(void*, uint64_t memidx, uint64_t addr,
                                                 uint64_t count) {
  (void)memidx;
  return wb::imports_futex_notify(static_cast<uint32_t>(addr),
                                  static_cast<uint32_t>(count));
}

extern "C" uint64_t builtin_memory_atomic_wait32(void*, uint64_t memidx, uint64_t addr,
                                                 uint64_t expected, uint64_t timeout) {
  (void)memidx; (void)addr; (void)expected; (void)timeout;
  return 2; // "timed out" immediately: never blocks the smoke test
}

extern "C" uint32_t builtin_throw_ref(void*, uint64_t exnref) {
  (void)exnref;
  note_error("throw_ref called (exception propagation not implemented)");
  return 0; // false = not caught
}

extern "C" void* builtin_table_get_lazy_init_func_ref(void* vmctx, uint64_t table, uint64_t index) {
  (void)table;
  auto* tab = reinterpret_cast<VMTableDefinition*>(at(vmctx, VMCTX_OFF_TABLES));
  static VMFuncRef fallback{};
  fallback.vmctx = vmctx;
  fallback.type_index = 1;
  if (!tab || index >= tab->current_elements) {
    note_error("table.get_lazy_init_func_ref index out of bounds");
    return &fallback;
  }
  auto* entries = reinterpret_cast<VMFuncRef**>(tab->base);
  return entries[index] ? entries[index] : &fallback;
}

extern "C" uint32_t builtin_gc_alloc_raw(void*, uint64_t kind, uint64_t type_index,
                                         uint64_t size, uint64_t align) {
  (void)kind; (void)type_index; (void)size; (void)align;
  note_error("gc_alloc_raw called (GC not implemented in skeleton)");
  return 0;
}

extern "C" uint32_t builtin_get_instance_id(void*) { return 0; }

// Catch-all for builtin slots whose trampoline is not present in this object.
extern "C" uint64_t builtin_stub(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  note_error("unimplemented builtin called");
  return 0;
}

// ---- 341 generic import stubs (to be replaced by the real import worker) --

extern "C" uint64_t wasm_import_stub(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t) {
  if (g_state) ++g_state->import_call_count;
  return 0;
}

}  // namespace

void note_error(const char* what) {
  if (g_state) ++g_state->error_count;
  std::fprintf(stderr, "[wasm-host] controlled error: %s\n", what);
}

bool setup(HostState& state) {
  static_assert(sizeof(VMMemoryDefinition) == VM_MEMORY_DEFINITION_SIZE);
  static_assert(sizeof(VMTableDefinition) == VM_TABLE_DEFINITION_SIZE);
  static_assert(sizeof(VMFunctionImport) == VM_FUNCTION_IMPORT_SIZE);
  static_assert(sizeof(VMFuncRef) == VM_FUNC_REF_SIZE);
  static_assert(sizeof(VMMemoryImport) == 24);
  static_assert(offsetof(VMStoreContext, stack_limit) == VMSTORE_OFF_STACK_LIMIT);
  static_assert(offsetof(VMStoreContext, gc_heap) == VMSTORE_OFF_GC_HEAP);
  static_assert(offsetof(VMStoreContext, last_wasm_exit_trampoline_fp) ==
                VMSTORE_OFF_LAST_WASM_EXIT_TRAMPOLINE_FP);
  static_assert(sizeof(VMStoreContext) == VMSTORE_SIZE);
  static_assert(VMCTX_NUM_IMPORTED_FUNCTIONS == 341);
  static_assert(VMCTX_NUM_IMPORTED_MEMORIES == 1);
  static_assert(VMCTX_NUM_DEFINED_MEMORIES == 0);
  static_assert(VMCTX_NUM_DEFINED_TABLES == 1);

  g_state = &state;
  // VMContext is followed by one runtime-data base pointer and one length
  // per wasm data segment.  Wasmtime's generated code indexes them directly
  // (segment 0 is the TLS image copied by __wasm_init_tls).
  constexpr size_t kRtCount = WB_RUNTIME_DATA_COUNT;
  constexpr size_t kVmctxAlloc = VMCTX_SIZE_OF_VMCTX + kRtCount * 8 + kRtCount * 4;
  state.vmctx = static_cast<uint8_t*>(std::calloc(kVmctxAlloc, 1));
  if (!state.vmctx) {
    std::fprintf(stderr, "[wasm-host] vmctx allocation failed\n");
    return false;
  }
  auto** rt_bases = reinterpret_cast<void**>(
      state.vmctx + VMCTX_OFF_RUNTIME_DATA_BASES);
  auto* rt_lengths = reinterpret_cast<uint32_t*>(rt_bases + kRtCount);
  for (size_t i = 0; i < kRtCount; ++i) {
    rt_bases[i] = const_cast<unsigned char*>(wb_runtime_data + wb_runtime_data_offsets[i]);
    rt_lengths[i] = wb_runtime_data_lengths[i];
  }

  // Host-owned shared linear memory: 4 GiB reservation + guard pages. The
  // pages are virtual until touched; committed up front so bounds-check-free
  // generated accesses cannot fault.
  constexpr size_t kGuard = 1u << 20;
  constexpr size_t kMemSize = size_t{4} << 30;
  state.memory_reservation = kMemSize + 2 * kGuard;
  void* reservation = ::mmap(nullptr, state.memory_reservation, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) {
    std::fprintf(stderr, "[wasm-host] memory reservation failed\n");
    return false;
  }
  state.memory_base = static_cast<uint8_t*>(reservation) + kGuard;
  if (::mmap(state.memory_base, kMemSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
    std::fprintf(stderr, "[wasm-host] memory commit failed\n");
    return false;
  }
  state.memory_def_storage = {state.memory_base, kMemSize};

  // Initialise linear memory from the wasm data segments (Wasmtime's
  // __wasm_init_memory normally does this).  Passive segments are TLS/table
  // images and are exposed through the runtime-data bases instead.
  for (size_t i = 0; i < WB_RUNTIME_DATA_COUNT; ++i) {
    const uint32_t dst = wb_runtime_data_dests[i];
    if (dst == 0xFFFFFFFFu) continue;
    const size_t n = wb_runtime_data_lengths[i];
    if (static_cast<size_t>(dst) > kMemSize || n > kMemSize - dst) {
      note_error("data segment out of bounds");
      continue;
    }
    if (n) std::memcpy(state.memory_base + dst,
                       wb_runtime_data + wb_runtime_data_offsets[i], n);
  }

  // VMStoreContext: a generous stack limit (smaller than any real rsp), no
  // epoch/fuel interrupts, no async guard range.
  state.store_storage = {};
  state.store_storage.fuel_consumed = std::numeric_limits<int64_t>::min() / 2;
  state.store_storage.epoch_deadline = std::numeric_limits<uint64_t>::max();
  state.store_storage.stack_limit = 0x1000;

  std::memcpy(at(state.vmctx, VMCTX_OFF_MAGIC), "core", 4);
  void* store_ptr = &state.store_storage;
  std::memcpy(at(state.vmctx, VMCTX_OFF_STORE_CONTEXT), &store_ptr, sizeof(store_ptr));

  // Builtins array: every slot is a safe stub, the eight real ones override.
  for (auto& slot : state.builtin_slots) slot = reinterpret_cast<void*>(&builtin_stub);
#define SET_BUILTIN(name, fn) \
  state.builtin_slots[BUILTIN_INDEX_##name] = reinterpret_cast<void*>(&fn)
  SET_BUILTIN(MEMORY_COPY, builtin_memory_copy);
  SET_BUILTIN(MEMORY_FILL, builtin_memory_fill);
  SET_BUILTIN(MEMORY_ATOMIC_NOTIFY, builtin_memory_atomic_notify);
  SET_BUILTIN(MEMORY_ATOMIC_WAIT32, builtin_memory_atomic_wait32);
  SET_BUILTIN(THROW_REF, builtin_throw_ref);
  SET_BUILTIN(TABLE_GET_LAZY_INIT_FUNC_REF, builtin_table_get_lazy_init_func_ref);
  SET_BUILTIN(GC_ALLOC_RAW, builtin_gc_alloc_raw);
  SET_BUILTIN(GET_INSTANCE_ID, builtin_get_instance_id);
#undef SET_BUILTIN
  void* builtins_ptr = state.builtin_slots;
  std::memcpy(at(state.vmctx, VMCTX_OFF_BUILTIN_FUNCTIONS), &builtins_ptr, sizeof(builtins_ptr));
  void* epoch_ptr = &state.epoch;
  std::memcpy(at(state.vmctx, VMCTX_OFF_EPOCH_PTR), &epoch_ptr, sizeof(epoch_ptr));

  // GC heap data and type-id table: the skeleton never allocates GC objects,
  // but generated code may load the pointers unconditionally.
  static uint8_t gc_heap_dummy[4096];
  static uint32_t type_ids[65536];
  void* gc_ptr = gc_heap_dummy;
  void* types_ptr = type_ids;
  std::memcpy(at(state.vmctx, VMCTX_OFF_GC_HEAP_DATA), &gc_ptr, sizeof(gc_ptr));
  std::memcpy(at(state.vmctx, VMCTX_OFF_TYPE_IDS), &types_ptr, sizeof(types_ptr));

  // Imported memory (the module's only memory is env.memory).
  auto* mem_import = reinterpret_cast<VMMemoryImport*>(at(state.vmctx, VMCTX_OFF_IMPORTED_MEMORIES));
  mem_import->from = &state.memory_def_storage;
  mem_import->vmctx = state.vmctx;
  mem_import->index = 0;

  // 341 imported functions: generated wasm-ABI implementations + this vmctx.
  wb::imports_set_memory(state.memory_base, state.memory_def_storage.current_length);
  auto* imports = reinterpret_cast<wb::FunctionImportSlot*>(at(state.vmctx, VMCTX_OFF_IMPORTED_FUNCTIONS));
  wb::imports_install(imports, VMCTX_NUM_IMPORTED_FUNCTIONS, state.vmctx);

  // Defined table: entries are VMFuncRef pointers in Wasmtime's table layout.
  constexpr size_t kTableEntries = 161491; // from the module's table limits
  state.table_storage = static_cast<uint8_t*>(
      std::calloc(kTableEntries, sizeof(void*)));
  if (!state.table_storage) {
    std::fprintf(stderr, "[wasm-host] table allocation failed\n");
    return false;
  }
  static VMFuncRef table_funcref{};
  table_funcref.vmctx = state.vmctx;
  table_funcref.type_index = 1;
  auto** entries = reinterpret_cast<VMFuncRef**>(state.table_storage);
  for (size_t i = 0; i < kTableEntries; ++i) entries[i] = &table_funcref;
  auto* table = reinterpret_cast<VMTableDefinition*>(at(state.vmctx, VMCTX_OFF_TABLES));
  table->base = state.table_storage;
  table->current_elements = kTableEntries;

  return install_signal_handlers();
}

int call_ctors(HostState& state) {
  if (sigsetjmp(g_trap_jmp, 1) != 0) return g_trap_signal;
  g_in_wasm = 1;
  // The module's wasm start function (index 344) initialises passive data
  // segments (TLS/thread-registry images) via memory.init.  __wasm_call_ctors
  // assumes it has already run.
  _wasm_function_3(state.vmctx, state.vmctx);
  _wasm_function_0(state.vmctx, state.vmctx);
  g_in_wasm = 0;
  return 0;
}

uint32_t call_malloc(HostState& state, uint32_t size, int& trap_signal) {
  volatile uint32_t result = 0;
  if (sigsetjmp(g_trap_jmp, 1) != 0) {
    trap_signal = g_trap_signal;
    return 0;
  }
  g_in_wasm = 1;
  result = _wasm_function_149984(state.vmctx, state.vmctx, size);
  g_in_wasm = 0;
  trap_signal = 0;
  return result;
}

int call_stack_roundtrip(HostState& state, uint32_t* base_out, uint32_t* end_out) {
  constexpr uint32_t kBase = 0x12345000u;
  constexpr uint32_t kEnd = 0x12346000u;
  if (sigsetjmp(g_trap_jmp, 1) != 0) return g_trap_signal;
  g_in_wasm = 1;
  _wasm_function_150011(state.vmctx, state.vmctx, kBase, kEnd);
  uint32_t base = _wasm_function_499(state.vmctx, state.vmctx);
  uint32_t end = _wasm_function_500(state.vmctx, state.vmctx);
  g_in_wasm = 0;
  *base_out = base;
  *end_out = end;
  // Restore the defaults so later calls see a clean VMContext.
  _wasm_function_150011(state.vmctx, state.vmctx, 0, 0);
  return (base == kBase && end == kEnd) ? 0 : -1;
}

int call_trap_test(HostState& state) {
  if (sigsetjmp(g_trap_jmp, 1) != 0) return g_trap_signal;
  g_in_wasm = 1;
  _wasm_function_2(state.vmctx, state.vmctx);
  g_in_wasm = 0;
  return 0;
}

}  // namespace host
