// Minimal Wasmtime-ABI host glue: VMContext setup, host-owned shared linear
// memory, builtin libcalls, import stubs and trap-to-error signal handling.
#define _GNU_SOURCE 1
#include "vmctx.h"
#include "imports.h"
#include "runtime_data.h"
#include "table.h"

#include <csignal>
#include <setjmp.h>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <limits>
#include <utility>

#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

namespace host {

namespace {

HostState* g_state = nullptr;
volatile sig_atomic_t g_in_wasm = 0;
volatile sig_atomic_t g_trap_signal = 0;
sigjmp_buf g_trap_jmp;

// ---- bounded HOST_TRACE plumbing -------------------------------------------
// Generated code calls builtins in hot loops; trace output is capped per
// distinct key so a single run stays bounded and readable.
bool host_trace_enabled() {
  static const bool enabled = [] {
    const char* s = std::getenv("HOST_TRACE");
    return s && *s && std::strcmp(s, "0") != 0;
  }();
  return enabled;
}

bool trace_budget(const char* key, int cap) {
  static struct { const char* key; int n; } slots[64];
  for (auto& s : slots) {
    if (s.key && std::strcmp(s.key, key) == 0) {
      if (s.n >= cap) return false;
      ++s.n;
      return true;
    }
  }
  for (auto& s : slots) {
    if (!s.key) { s.key = key; s.n = 1; return true; }
  }
  return false;
}

#define TRACEF(key, cap, fmt, ...)                                            \
  do {                                                                        \
    if (host_trace_enabled() && trace_budget((key), (cap)))                   \
      std::fprintf(stderr, "[trace] " fmt, ##__VA_ARGS__);                    \
  } while (0)

// ---- real GC heap (see host/GC.md) -----------------------------------------
// The module was compiled with wasmtime-environ's
// Collector::DeferredReferenceCounting (see gen_vmctx.rs). Wasm GC refs are
// u32 *byte offsets* into VMStoreContext.gc_heap.base; a bump allocator with
// a DRC header is enough for the engine's bootstrap allocations (no collection
// is ever performed).
uint8_t* g_gc_heap_base = nullptr;
size_t g_gc_heap_size = 0;
size_t g_gc_bump = 0;

// VMDrcHeapData (wasmtime-environ vmtypes.rs): zeroed == empty OASR list.
struct DrcHeapData {
  uint32_t oasr_head;
  uint32_t oasr_len;
  uint32_t oasr_len_after_gc;
  uint32_t reserved;
};
static DrcHeapData g_drc_heap_data{};

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

// DRC exception propagation needs a native unwinder this host does not have.
// Returning false ("not caught") lets the generated try_call fall through to
// its trap path, which the signal handler converts into a controlled error.
extern "C" uint32_t builtin_throw_ref(void*, uint64_t exnref) {
  TRACEF("throw_ref", 8, "throw_ref exnref=%#llx -> not caught\n",
         static_cast<unsigned long long>(exnref));
  note_error("throw_ref called (exception propagation not implemented)");
  return 0; // false = not caught
}

// Wasmtime's libcall returns a *mut VMFuncRef (or null for a null table
// entry). The active element segment was eagerly materialised in setup(), so
// a null entry here really is a null funcref -- never fabricate one.
extern "C" void* builtin_table_get_lazy_init_func_ref(void* vmctx, uint64_t table, uint64_t index) {
  (void)table;
  auto* tab = reinterpret_cast<VMTableDefinition*>(at(vmctx, VMCTX_OFF_TABLES));
  if (!tab || index >= tab->current_elements) {
    TRACEF("table_lazy", 8, "table.get_lazy_init_func_ref index=%llu out of bounds\n",
           static_cast<unsigned long long>(index));
    note_error("table.get_lazy_init_func_ref index out of bounds");
    return nullptr;
  }
  auto* entries = reinterpret_cast<VMFuncRef**>(tab->base);
  void* fr = entries[index];
  TRACEF("table_lazy", 8, "table.get_lazy_init_func_ref index=%llu -> %p\n",
         static_cast<unsigned long long>(index), fr);
  return fr;
}

// u32 heap index (2-byte aligned, non-zero) of a fresh object. The host writes
// the VMDrcHeader so the DRC read/write barriers emitted by Cranelift see a
// valid ref_count/OASR link. No collection is performed; see host/GC.md.
extern "C" uint32_t builtin_gc_alloc_raw(void*, uint64_t kind, uint64_t type_index,
                                         uint64_t size, uint64_t align) {
  uint64_t a = align < 8 ? 8 : align;  // DRC header is 8-byte aligned
  uint64_t base = (g_gc_bump + a - 1) & ~(a - 1);
  if (base == 0) base = a;             // 0 is the null reference
  uint64_t end = base + size;
  if (!g_gc_heap_base || end > g_gc_heap_size || size < 4) {
    note_error("gc_alloc_raw: bounded GC heap exhausted");
    return 0;  // trap sentinel (Falsy) -- honest OOM, never fabricate a ref
  }
  uint8_t* obj = g_gc_heap_base + base;
  std::memset(obj, 0, static_cast<size_t>(size));
  const uint32_t k = static_cast<uint32_t>(kind);  // kind in top 6 bits
  const uint32_t ty = static_cast<uint32_t>(type_index);
  std::memcpy(obj + 0, &k, 4);       // VMGcHeader.kind (+ reserved bits)
  std::memcpy(obj + 4, &ty, 4);      // VMGcHeader.ty (VMSharedTypeIndex)
  if (size >= 24) {
    const uint64_t refcount = 1;     // DRC: host owns one strong ref
    const uint32_t object_size = static_cast<uint32_t>(size);
    std::memcpy(obj + 8, &refcount, 8);
    std::memcpy(obj + 20, &object_size, 4);  // VMDrcHeader.object_size
  }
  g_gc_bump = static_cast<size_t>(end);
  TRACEF("gc_alloc_raw", 12,
         "gc_alloc_raw kind=%#x ty=%u size=%llu align=%llu -> heap_index=%llu\n",
         k, ty, static_cast<unsigned long long>(size),
         static_cast<unsigned long long>(align),
         static_cast<unsigned long long>(base));
  return static_cast<uint32_t>(base);
}

// Instance ids only matter for exception tags; any non-zero stable id works
// for this single-instance host.
extern "C" uint32_t builtin_get_instance_id(void*) {
  TRACEF("instance_id", 4, "get_instance_id -> 1\n");
  return 1;
}

// The `raise` libcall is invoked by gc_alloc_raw's trap path (builtin slot 41)
// and by `throw` continuations. Wasmtime unwinds natively; we have no unwinder,
// so ud2 is the honest conversion to a controlled trap signal.
extern "C" uint64_t builtin_raise(void*) {
  TRACEF("raise", 16, "raise builtin -> ud2 (no native unwinder)\n");
  __builtin_trap();
  return 0;
}
extern "C" uint64_t builtin_trap(void*, uint64_t code) {
  TRACEF("trap", 16, "trap builtin code=%llu -> ud2\n",
         static_cast<unsigned long long>(code));
  __builtin_trap();
  return 0;
}

// DRC force_gc is a no-op: we never collect, and bump allocation never relies
// on reclamation during bootstrap. Returning true avoids a trap sentinel.
extern "C" uint32_t builtin_force_gc(void*) {
  TRACEF("force_gc", 4, "force_gc -> no-op (no collector)\n");
  return 1;
}

// Bounded trace stub for every builtin slot without a real implementation.
// `N` is the builtin index from vmctx_offsets.h / builtin.rs.
template <unsigned N>
static uint64_t trace_builtin(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  TRACEF("unknown_builtin", 128, "unimplemented builtin[%u] called\n", N);
  note_error("unimplemented builtin called");
  return 0;
}

template <std::size_t... I>
static void install_trace_builtins(HostState& state, std::index_sequence<I...>) {
  (void)std::initializer_list<int>{
      (state.builtin_slots[I] = reinterpret_cast<void*>(&trace_builtin<I>), 0)...};
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

  // Builtins array: every slot starts as a bounded tracing stub so HOST_TRACE=1
  // can identify any builtin the bootstrap hits next; real ones override.
  install_trace_builtins(state, std::make_index_sequence<BUILTIN_FUNCTIONS_ARRAY_LEN>{});
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
  // Indices from builtin.rs with this module's feature set (see GC.md):
  // 40 = trap, 41 = raise, 45 = force_gc (gc-drc).
  state.builtin_slots[40] = reinterpret_cast<void*>(&builtin_trap);
  state.builtin_slots[41] = reinterpret_cast<void*>(&builtin_raise);
  state.builtin_slots[45] = reinterpret_cast<void*>(&builtin_force_gc);
  void* builtins_ptr = state.builtin_slots;
  std::memcpy(at(state.vmctx, VMCTX_OFF_BUILTIN_FUNCTIONS), &builtins_ptr, sizeof(builtins_ptr));
  void* epoch_ptr = &state.epoch;
  std::memcpy(at(state.vmctx, VMCTX_OFF_EPOCH_PTR), &epoch_ptr, sizeof(epoch_ptr));

  // Real GC heap: 256 MiB of zeroed anonymous memory. Wasm GC refs are u32
  // byte offsets into this region (VMStoreContext.gc_heap.base), so start the
  // bump pointer at 8 to reserve the null reference and keep 8-byte alignment.
  constexpr size_t kGcHeapSize = size_t{256} << 20;
  g_gc_heap_base = static_cast<uint8_t*>(
      ::mmap(nullptr, kGcHeapSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (g_gc_heap_base == MAP_FAILED) {
    std::fprintf(stderr, "[wasm-host] GC heap mmap failed\n");
    return false;
  }
  g_gc_heap_size = kGcHeapSize;
  g_gc_bump = 8;
  state.store_storage.gc_heap.base = g_gc_heap_base;
  state.store_storage.gc_heap.current_length = kGcHeapSize;
  std::fprintf(stderr, "[wasm-host] gc heap: %p %zu bytes (bump allocator, DRC headers)\n",
               static_cast<void*>(g_gc_heap_base), kGcHeapSize);

  // VMContext.gc_heap_data is a DRC heap-data pointer; generated read/write
  // barriers load the OASR head and lengths from it (all zero = empty list).
  static uint32_t type_ids[65536];
  // Cranelift resolves a call_indirect's expected signature by loading
  // type_ids[module_type_index] from the VMContext, then comparing it with
  // VMFuncRef.type_index.  Wasmtime's real instance stores a shared type id
  // here; for this self-contained host identity is enough as long as both
  // sides use the same numbering (see gen_table.py).
  for (uint32_t i = 0; i < 65536; ++i) type_ids[i] = i;
  void* gc_ptr = &g_drc_heap_data;
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
  // The active element segment is baked into table.cpp by gen_table.py; index 0
  // stays null (the segment starts at offset 1).  Imported functions use the
  // wasm-call trampoline from the import array instead of a guest symbol.
  const uint32_t kTableEntries = wb_table_size;
  state.table_storage = static_cast<uint8_t*>(
      std::calloc(kTableEntries, sizeof(void*)));
  state.table_funcrefs = static_cast<VMFuncRef*>(
      std::calloc(kTableEntries, sizeof(VMFuncRef)));
  if (!state.table_storage || !state.table_funcrefs) {
    std::fprintf(stderr, "[wasm-host] table allocation failed\n");
    return false;
  }
  auto** entries = reinterpret_cast<VMFuncRef**>(state.table_storage);
  uint32_t initialized = 0;
  for (uint32_t i = 0; i < wb_table_init_count; ++i) {
    const WBTableEntry& e = wb_table_init[i];
    VMFuncRef& fr = state.table_funcrefs[e.table_index];
    fr.array_call = nullptr;  // call_indirect uses wasm_call only
    fr.wasm_call = e.wasm_call ? e.wasm_call
                               : wb::import_wasm_call(e.func_index + 1);
    fr.type_index = e.type_index;
    fr.reserved = 0;
    fr.vmctx = state.vmctx;
    if (!fr.wasm_call) {
      std::fprintf(stderr, "[wasm-host] unresolved table func %u\n", e.func_index);
      return false;
    }
    entries[e.table_index] = &fr;
    ++initialized;
  }
  auto* table = reinterpret_cast<VMTableDefinition*>(at(state.vmctx, VMCTX_OFF_TABLES));
  table->base = state.table_storage;
  table->current_elements = kTableEntries;
  std::fprintf(stderr, "[wasm-host] table: %u entries, %u initialized\n",
               kTableEntries, initialized);

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
