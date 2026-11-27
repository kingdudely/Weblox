// Make the C11 wasm-rt.h usable from C++ without changing its ABI.
//
// wasm-rt.h defines WASM_RT_C11_AVAILABLE only for C, but the host must see
// the same struct layout as the generated C library (notably
// wasm_rt_shared_memory_t).  C++ has no `_Atomic` keyword, but the qualifier
// does not change pointer size or alignment, so erase it while including the
// C header.  The macro MUST be undefined again immediately: libc++ uses
// `_Atomic(x)` as a function-like macro and would break if it stayed defined.
//
// Include this BEFORE "WC.h" in every C++ translation unit that includes it.
#pragma once

#ifdef __cplusplus
#ifndef WASM_RT_C11_AVAILABLE
#define WASM_RT_C11_AVAILABLE
#endif
#ifdef _MSC_VER
#define _Thread_local __declspec(thread)
#endif
#define _Atomic
#include "wasm-rt.h"
#undef _Atomic
#endif
