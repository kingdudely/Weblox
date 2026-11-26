// Smoke test: prove the Wasmtime VMContext ABI works by calling one guest
// export from a plain C++ process linked against wasm2obj's out.full.o.
#include "vmctx.h"
#include "imports.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dlfcn.h>

// wasm function indices from the module export section (first defined
// function index is 341, so _wasm_function_N == wasm index N + 341):
//   _emscripten_thread_init = 151421 -> _wasm_function_151080
//   _emscripten_tls_init    = 149940 -> _wasm_function_149599
//   pthread_self           = 150209 -> _wasm_function_149868
// These are global symbols in out.full.o but the linked executable makes
// them local, so dlsym(RTLD_DEFAULT) cannot see them: call them directly.
extern "C" {
uint32_t _wasm_function_149599(void*, void*);
uint32_t _wasm_function_149868(void*, void*);
void wb_call_thread_init(void*, uint32_t);
}

// Wasmtime's wasm-call ABI is callee-pops for stack arguments (`ret $16`),
// unlike the SysV C ABI.  Calling a 6-arg wasm function through a C prototype
// corrupts rsp, so use a tiny asm shim that matches the wasm convention.
asm(
    ".text\n"
    ".globl wb_call_thread_init\n"
    "wb_call_thread_init:\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  mov %rdi, %rax\n"          // callee vmctx
    "  mov %esi, %edx\n"          // wasm arg 0: pthread_ptr
    "  mov %rax, %rsi\n"          // caller vmctx
    "  mov %rax, %rdi\n"          // callee vmctx
    "  mov $1, %ecx\n"            // wasm arg 1: is_main
    "  mov $1, %r8d\n"            // wasm arg 2
    "  mov $1, %r9d\n"            // wasm arg 3: atomics
    "  pushq $0\n"                // wasm arg 5
    "  pushq $262144\n"           // wasm arg 4: stack size
    "  call _wasm_function_151080\n"  // callee pops the 16 bytes above
    "  mov %rbp, %rsp\n"
    "  pop %rbp\n"
    "  ret\n");

static void main_thread_init(void* vmctx, uint32_t ptr) {
  std::fprintf(stderr, "[pthr] guest thread_init(ptr=%#x) + tls_init\n", ptr);
  wb_call_thread_init(vmctx, ptr);
  _wasm_function_149599(vmctx, vmctx);
  const uint32_t me = _wasm_function_149868(vmctx, vmctx);
  if (me) wb::imports_set_main_thread(me);
}

// Emscripten pthread worker entry: re-enter the wasm function named by the
// table index `start` with a per-thread VMContext.  This is the hook the
// ported __pthread_create_js import requires.
static void* g_main_vmctx = nullptr;

static void thread_entry(uint32_t start, uint32_t arg, uint32_t pthread_ptr) {
  char sym[64];
  std::snprintf(sym, sizeof(sym), "_wasm_function_%u", start - 341u);
  using WasmFn = void (*)(void*, void*, uint32_t);
  auto* fn = reinterpret_cast<WasmFn>(dlsym(RTLD_DEFAULT, sym));
  if (!fn) {
    std::fprintf(stderr, "[pthr] no wasm symbol for start=%u (%s)\n", start, sym);
    return;
  }
  void* vm = std::malloc(VMCTX_SIZE_OF_VMCTX);
  if (!vm) return;
  std::memcpy(vm, g_main_vmctx, VMCTX_SIZE_OF_VMCTX);
  // Give this fibre a fresh stack guard.  vmctx+8 is VMStoreContext; its
  // stack_limit lives at +24 (offsets from vmctx_offsets.h).
  uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  uint64_t limit = static_cast<uint64_t>(sp - (64u << 20));
  auto** store = reinterpret_cast<void**>(static_cast<uint8_t*>(vm) + VMCTX_OFF_STORE_CONTEXT);
  if (*store) {
    std::memcpy(static_cast<uint8_t*>(*store) + 24, &limit, sizeof(limit));
  }
  std::fprintf(stderr, "[pthr] re-enter start=%u (%s) arg=%u ptr=%#x\n", start, sym, arg, pthread_ptr);
  fn(vm, vm, arg);
  std::free(vm);
}

int main(int argc, char** argv) {
  // --skip-ctors is useful because __wasm_call_ctors runs Emscripten's full
  // startup with hundreds of host imports; the stub imports cannot satisfy it.
  bool skip_ctors = argc > 1 && std::string(argv[1]) == "--skip-ctors";
  bool test_trap = argc > 1 && std::string(argv[1]) == "--test-trap";
  host::HostState state;
  if (!host::setup(state)) {
    std::fprintf(stderr, "[wasm-host] RESULT=FAIL setup failed\n");
    return 1;
  }
  g_main_vmctx = state.vmctx;
  wb::imports_set_thread_entry(&thread_entry);
  wb::imports_set_main_thread_init(&main_thread_init);
  std::printf("[wasm-host] vmctx=%p size=%u memory=%p length=%zu imports=%u builtins=%u\n",
              static_cast<void*>(state.vmctx), VMCTX_SIZE_OF_VMCTX,
              static_cast<void*>(state.memory_base), state.memory_def_storage.current_length,
              VMCTX_NUM_IMPORTED_FUNCTIONS, BUILTIN_FUNCTIONS_ARRAY_LEN);
  std::fflush(stdout);

  // Primary ABI proof: real wasm exports write and read VMContext globals.
  uint32_t stack_base = 0, stack_end = 0;
  int roundtrip = host::call_stack_roundtrip(state, &stack_base, &stack_end);
  if (roundtrip == 0) {
    std::printf("[wasm-host] ABI ROUNDTRIP OK: emscripten_stack_set_limits -> "
                "get_stack_base=%u get_stack_end=%u\n", stack_base, stack_end);
  } else {
    std::printf("[wasm-host] ABI ROUNDTRIP FAILED: %d (base=%u end=%u)\n",
                roundtrip, stack_base, stack_end);
  }
  std::fflush(stdout);

  if (test_trap) {
    int sig = host::call_trap_test(state);
    std::printf("[wasm-host] exported __trap: %s\n",
                sig ? "converted to controlled error" : "unexpectedly returned");
    std::fflush(stdout);
  }

  if (skip_ctors || test_trap) {
    std::printf("[wasm-host] __wasm_call_ctors: skipped (--skip-ctors/--test-trap)\n");
  } else {
    int ctor_trap = host::call_ctors(state);
    if (ctor_trap == 0) {
      std::printf("[wasm-host] __wasm_call_ctors (_wasm_function_0): returned normally\n");
    } else {
      std::printf("[wasm-host] __wasm_call_ctors (_wasm_function_0): trapped via signal %d "
                  "(converted to controlled error)\n", ctor_trap);
    }
  }
  std::printf("[wasm-host] import calls=%llu controlled errors=%u\n",
              static_cast<unsigned long long>(state.import_call_count),
              static_cast<unsigned>(state.error_count));
  std::fflush(stdout);

  int malloc_trap = 0;
  uint32_t ptr = host::call_malloc(state, 16, malloc_trap);
  if (malloc_trap == 0) {
    std::printf("[wasm-host] malloc(16) (_wasm_function_149984): returned %u (0x%x)\n", ptr, ptr);
  } else {
    std::printf("[wasm-host] malloc(16): trapped via signal %d (controlled)\n", malloc_trap);
  }
  bool ok = (roundtrip == 0 && malloc_trap == 0);
  std::printf("[wasm-host] RESULT=%s\n", (ok ? "OK" : "FAIL"));
  return ok ? 0 : 2;
}
