/* Minimal Stage-B smoke test for a wasm2c engine library.
 *
 * It only instantiates the generated module (ctors, start function) using
 * automatically generated import stubs.  No guest-specific indices, addresses
 * or manual imports: everything comes from the generated WC.h.
 */
#include "WC.h"

#include <stdio.h>
#include <string.h>

extern wasm_rt_shared_memory_t g_mem;
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;

int main(void) {
  wasm_rt_init();
  wasm_rt_allocate_memory_shared(&g_mem,
                                 wasm2c_WebClient_min_env_memory,
                                 wasm2c_WebClient_max_env_memory,
                                 wasm2c_WebClient_is64_env_memory,
                                 wasm2c_WebClient_pagesize_env_memory);

  w2c_WebClient inst;
  memset(&inst, 0, sizeof inst);
  wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);
  printf("SMOKE_OK mem=%llu\n", (unsigned long long)g_mem.size);
  wasm2c_WebClient_free(&inst);
  wasm_rt_free_memory_shared(&g_mem);
  return 0;
}
