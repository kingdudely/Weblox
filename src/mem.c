#include "host.h"

void* weblox_mem_base(void) { return (void*)g_mem.data; }

uint64_t weblox_mem_size(void) { return g_mem.size; }
