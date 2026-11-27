// Make the C11 wasm-rt.h usable from C++ without changing its ABI.
//
// wasm-rt.h defines WASM_RT_C11_AVAILABLE only for C, but the host must see
// the same struct layout as the generated C library (notably
// wasm_rt_shared_memory_t).  C++ has no standard `_Atomic`, but the qualifier
// does not change pointer size or alignment, so erase it for compilers that
// need it.  clang accepts `_Atomic` natively and libc++ probes it with
// `__has_keyword(_Atomic)`, so defining the macro there breaks libc++.
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
#ifndef __clang__
#define _Atomic
#endif
#include "wasm-rt.h"
#ifndef __clang__
#undef _Atomic
#endif
#endif
