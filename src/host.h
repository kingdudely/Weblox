// Shared declarations for the C host.  WC.h is a C header, so no extern "C"
// wrapper (or C++ compatibility shim) is needed anywhere in this project.
#ifndef WEBLOX_HOST_H
#define WEBLOX_HOST_H

#include "WC.h"

extern wasm_rt_shared_memory_t g_mem;
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;
extern w2c_WebClient* weblox_instance;

void* weblox_mem_base(void);
uint64_t weblox_mem_size(void);

#endif
