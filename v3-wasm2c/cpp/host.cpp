// Minimal wasm2c host skeleton, written in C++ but speaking the generated
// module's plain C ABI.  No guest indices, no hardcoded guest pointers.
//
// The generated C library is compiled as C; we include its header inside
// extern "C" so C++ name mangling does not affect the ABI.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "loop.h"
#include "window.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
extern wasm_rt_shared_memory_t g_mem;
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;
w2c_WebClient* g_wc_instance;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  // Unbuffered so a crash before CPP_SMOKE_OK still leaves a trail in CI.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("CPP_BEGIN\n");

  wasm_rt_init();
  std::printf("CPP_RT_INIT\n");
  // The browser allocates the module maximum up front and never grows it.
  // On POSIX mmap reserves 4 GiB lazily, so that is safe.  WABT's Windows
  // path MEM_COMMITs the full initial size, which exceeds the runner's commit
  // limit and aborts; there we commit only the module's initial pages while
  // still reserving the full range.
#ifdef _WIN32
  const u64 pages = wasm2c_WebClient_min_env_memory;
#else
  const u64 pages = wasm2c_WebClient_max_env_memory;
#endif
  wasm_rt_allocate_memory_shared(
      &g_mem,
      pages,
      wasm2c_WebClient_max_env_memory,
      wasm2c_WebClient_is64_env_memory,
      wasm2c_WebClient_pagesize_env_memory);
  std::printf("CPP_MEM_ALLOC=%llu\n",
              static_cast<unsigned long long>(g_mem.size));

  w2c_WebClient inst;
  std::memset(&inst, 0, sizeof inst);
  std::printf("CPP_INSTANTIATE\n");
  wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);
  g_wc_instance = &inst;

  std::printf("CPP_SMOKE_OK mem=%llu\n",
              static_cast<unsigned long long>(g_mem.size));

  // Default run is a short smoke test.  WEBLOX_RUN_MS opts into a bounded
  // engine run so join-path experiments do not slow the CI smoke gate.
  const char* run_ms = std::getenv("WEBLOX_RUN_MS");
  if (run_ms && *run_ms) {
    int turns = weblox::loop::run(&inst,
                                  std::chrono::milliseconds(std::atoi(run_ms)));
    std::printf("RUN_TURNS=%d\n", turns);
  } else {
    weblox::Window window;
    if (window.init()) {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(100ms);
      window.pump();
      window.quit();
    }
  }

  wasm2c_WebClient_free(&inst);
  wasm_rt_free_memory_shared(&g_mem);
  return 0;
}
