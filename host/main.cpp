// Smoke test: prove the Wasmtime VMContext ABI works by calling one guest
// export from a plain C++ process linked against wasm2obj's out.full.o.
#include "vmctx.h"

#include <cstdio>
#include <cstring>
#include <string>

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
