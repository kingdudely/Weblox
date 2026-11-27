#include "mem.h"
#include "wasm-rt.h"

extern wasm_rt_shared_memory_t g_mem;

void* weblox_mem_base(void) { return (void*)g_mem.data; }
unsigned long long weblox_mem_size(void) {
  return (unsigned long long)g_mem.size;
}
