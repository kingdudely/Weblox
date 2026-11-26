// Pthread worker re-entry for the plain C++ host.
//
// Emscripten's `__pthread_create_js` hands us a *table index* (start), not a
// wasm function index.  The worker must run on its own VMContext (fresh wasm
// globals, own table of VMFuncRefs, own stack limits) while sharing the module
// memory with the main thread.  See host/PTHREAD.md.
#pragma once

#include <cstdint>

namespace wb {

// Register the main host state; must be called once after host::setup().
void pthread_register(void* main_vmctx);

// ThreadEntry hook installed into imports.cpp by main():
//   start    = index into __indirect_function_table
//   arg      = first wasm argument of the start routine
//   pthread_ptr = guest address of the pthread structure
void pthread_entry(std::uint32_t start, std::uint32_t arg,
                   std::uint32_t pthread_ptr);

}  // namespace wb
