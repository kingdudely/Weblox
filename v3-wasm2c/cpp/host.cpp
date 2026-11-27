// Minimal wasm2c host skeleton, written in C++ but speaking the generated
// module's plain C ABI.  No guest indices, no hardcoded guest pointers.
//
// The generated C library is compiled as C; we include its header inside
// extern "C" so C++ name mangling does not affect the ABI.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "window.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
extern wasm_rt_shared_memory_t g_mem;
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  wasm_rt_init();
  wasm_rt_allocate_memory_shared(
      &g_mem,
      wasm2c_WebClient_min_env_memory,
      wasm2c_WebClient_max_env_memory,
      wasm2c_WebClient_is64_env_memory,
      wasm2c_WebClient_pagesize_env_memory);

  w2c_WebClient inst;
  std::memset(&inst, 0, sizeof inst);
  wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);

  std::printf("CPP_SMOKE_OK mem=%llu\n",
              static_cast<unsigned long long>(g_mem.size));

  // A future host will pump SDL events + the engine loop here.  For now the
  // skeleton proves the C++ side can own the generated instance.
  weblox::Window window;
  if (window.init()) {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(100ms);
    window.pump();
    window.quit();
  }

  wasm2c_WebClient_free(&inst);
  wasm_rt_free_memory_shared(&g_mem);
  return 0;
}
