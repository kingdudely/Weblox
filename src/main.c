// C11 entry point.  The generated engine is plain C, so this host speaks its
// ABI directly: no extern "C" wrappers, no C++ runtime, no compatibility hacks.

#include "host.h"

#ifdef WEBLOX_HAVE_SDL3
#include "fibers.h"
#include "deferred.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// g_mem, g_env and g_wasi are defined by the generated stubs.c.
w2c_WebClient* weblox_instance;

#ifdef WEBLOX_HAVE_SDL3
static void main_turn(void* arg) {
  w2c_WebClient_0x5F_main_argc_argv((w2c_WebClient*)arg, 0, 0);
}
#endif

#ifdef _WIN32
// WC.h declares these module constants without __declspec(dllimport), so
// clang-cl reads the import table instead of the DLL's data.  Resolve by name.
static const void* dll_constant(const char* name) {
  HMODULE module = GetModuleHandleA("engine-windows-x64.dll");
  FARPROC address = module ? GetProcAddress(module, name) : NULL;
  if (!address) {
    fprintf(stderr, "missing DLL constant %s\n", name);
    abort();
  }
  return (const void*)address;
}
#endif

static void allocate_memory(void) {
#ifdef _WIN32
#ifdef WEBLOX_SOURCE_MODE
  const u64 max_pages = wasm2c_WebClient_max_env_memory;
  const u64 page_size = wasm2c_WebClient_pagesize_env_memory;
  const u8 is64 = wasm2c_WebClient_is64_env_memory;
#else
  const u64 max_pages = *(const u64*)dll_constant("wasm2c_WebClient_max_env_memory");
  const u64 page_size =
      *(const u32*)dll_constant("wasm2c_WebClient_pagesize_env_memory");
  const u8 is64 = *(const u8*)dll_constant("wasm2c_WebClient_is64_env_memory");
#endif
#else
  const u64 max_pages = wasm2c_WebClient_max_env_memory;
  const u64 page_size = wasm2c_WebClient_pagesize_env_memory;
  const u8 is64 = wasm2c_WebClient_is64_env_memory;
#endif
#ifdef _WIN32
  // WABT reserves 8 GiB with guard pages on Windows; that fails on runners.
  // Reserve the 4 GiB range directly and commit all of it: data segments live
  // near 2 GiB.
  const u64 reserve_bytes = max_pages * page_size;
  unsigned char* base = (unsigned char*)VirtualAlloc(
      NULL, (SIZE_T)reserve_bytes, MEM_RESERVE, PAGE_NOACCESS);
  if (!base || !VirtualAlloc(base, (SIZE_T)reserve_bytes, MEM_COMMIT,
                             PAGE_READWRITE)) {
    fprintf(stderr, "VirtualAlloc failed for guest memory\n");
    abort();
  }
  g_mem.data = (void*)base;
  g_mem.data_end = base + reserve_bytes;
  g_mem.pages = max_pages;
  g_mem.max_pages = max_pages;
  g_mem.size = reserve_bytes;
  g_mem.page_size = (u32)page_size;
  g_mem.is64 = is64 != 0;
  InitializeCriticalSection(&g_mem.mem_lock);
#else
  // mmap reserves 4 GiB lazily; commit nothing until the guest touches it.
  wasm_rt_allocate_memory_shared(&g_mem, max_pages, max_pages, is64,
                                 (u32)page_size);
#endif
}

static void free_memory(void) {
#ifdef _WIN32
  DeleteCriticalSection(&g_mem.mem_lock);
  VirtualFree((void*)g_mem.data, 0, MEM_RELEASE);
#else
  wasm_rt_free_memory_shared(&g_mem);
#endif
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("CPP_BEGIN\n");
  wasm_rt_init();
  printf("RT_INIT\n");
  allocate_memory();
  printf("MEM=%llu\n", (unsigned long long)g_mem.size);
#ifdef WEBLOX_HAVE_SDL3
  weblox_fibers_init();
#endif

  w2c_WebClient inst;
  memset(&inst, 0, sizeof inst);
  wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);
  weblox_instance = &inst;
  printf("SMOKE_OK mem=%llu\n", (unsigned long long)g_mem.size);

  const char* run_ms = getenv("WEBLOX_RUN_MS");
  if (run_ms && *run_ms && weblox_instance) {
#ifdef WEBLOX_HAVE_SDL3
    w2c_WebClient_0x5F_wasm_call_ctors(&inst);
    weblox_thread_maybe_init_main(&inst);
    void* root = weblox_run_on_root(&inst, main_turn, &inst);
    (void)root;
    int turns = 0;
    for (int i = 0; i < atoi(run_ms); ++i) {
      weblox_deferred_pump(&inst);
      ++turns;
    }
    printf("RUN_TURNS=%d\n", turns);
#else
    fprintf(stderr, "WEBLOX_RUN_MS needs SDL3/fiber support\n");
#endif
  }

  wasm2c_WebClient_free(&inst);
  free_memory();
  return 0;
}
