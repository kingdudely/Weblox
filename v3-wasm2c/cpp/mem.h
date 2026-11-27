// C shim for guest linear memory.  WABT's shared-memory type uses C11
// _Atomic, which is not directly usable from C++, so C++ code goes through
// these two plain extern "C" functions.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void* weblox_mem_base(void);
unsigned long long weblox_mem_size(void);

#ifdef __cplusplus
}
#endif
