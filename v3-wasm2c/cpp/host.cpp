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

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
extern wasm_rt_shared_memory_t g_mem;
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;
w2c_WebClient* g_wc_instance;
}

namespace {

#ifdef _WIN32
void* g_mem_base = nullptr;  // raw VirtualAlloc base, for release
#endif

// WABT reserves 8 GiB when guard pages are enabled.  That reservation fails
// on Windows runners, so set up the same 4 GiB address space directly there.
void allocate_memory() {
  const u64 max_pages = wasm2c_WebClient_max_env_memory;
  const u64 page_size = wasm2c_WebClient_pagesize_env_memory;
#ifdef _WIN32
  const u64 initial_pages = wasm2c_WebClient_min_env_memory;
  const u64 reserve_bytes = max_pages * page_size;
  auto* base = static_cast<unsigned char*>(
      VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE, PAGE_NOACCESS));
  if (!base) {
    std::fprintf(stderr, "VirtualAlloc reserve failed\n");
    std::abort();
  }
  if (!VirtualAlloc(base, initial_pages * page_size, MEM_COMMIT,
                    PAGE_READWRITE)) {
    std::fprintf(stderr, "VirtualAlloc commit failed\n");
    std::abort();
  }
  g_mem_base = base;
  g_mem.data = reinterpret_cast<decltype(g_mem.data)>(base);
  g_mem.data_end = reinterpret_cast<decltype(g_mem.data_end)>(
      base + reserve_bytes);
  g_mem.pages = initial_pages;
  g_mem.max_pages = max_pages;
  g_mem.size = initial_pages * page_size;
  g_mem.page_size = static_cast<u32>(page_size);
  g_mem.is64 = wasm2c_WebClient_is64_env_memory != 0;
  InitializeCriticalSection(&g_mem.mem_lock);
#else
  wasm_rt_allocate_memory_shared(&g_mem, max_pages, max_pages,
                                 wasm2c_WebClient_is64_env_memory,
                                 static_cast<u32>(page_size));
#endif
}

void free_memory() {
#ifdef _WIN32
  DeleteCriticalSection(&g_mem.mem_lock);
  VirtualFree(g_mem_base, 0, MEM_RELEASE);
#else
  wasm_rt_free_memory_shared(&g_mem);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  // Unbuffered so a crash before CPP_SMOKE_OK still leaves a trail in CI.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("CPP_BEGIN\n");

  wasm_rt_init();
  std::printf("CPP_RT_INIT\n");
  // The browser reserves the module maximum up front and never grows it.
  allocate_memory();
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
  free_memory();
  return 0;
}
