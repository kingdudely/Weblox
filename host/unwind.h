// Minimal native unwinder for Wasmtime 50 Cranelift exception tables.
//
// The compiled object carries a `.wasmtime.exceptions` section in Wasmtime's
// serialized ExceptionTable format (see wasmtime-internal-unwinder 50.0.2,
// exception_table.rs).  Wasmtime's own `throw_ref` libcall stores the exit PC
// and trampoline FP in VMStoreContext, walks the Cranelift FP chain, finds a
// try_call handler and resumes with two payload registers (rax/rdx).
//
// This file implements just enough of that to dispatch a `throw_ref` that
// originates in guest code to a guest `catch` landing pad.  It does not
// replace Cranelift codegen: the landing pads are already in out.full.o.
#pragma once

#include <cstdint>

namespace host {

struct UnwindHandler {
  uintptr_t pc;  // absolute address of the guest landing pad
  uintptr_t sp;  // stack pointer to restore (SP at the try_call site)
  uintptr_t fp;  // frame pointer of the catching frame
  uint32_t tag;  // 0xffffffff = catch-all, otherwise module-local tag index
};

// Locate the exception table section in the running executable (once).
// Returns true when the section was found and parsed.
bool unwind_init();

// Search the active Cranelift frames, starting from VMStoreContext's
// last_wasm_exit_pc/last_wasm_exit_fp, for a handler for the pending
// exception `exnref`.  On success, never returns: control is transferred to
// the guest handler with rax = exnref and rdx = 0.  Returns false when no
// handler matches, in which case the caller must take the honest trap path.
bool unwind_throw(void* vmctx, uint32_t exnref);

// Diagnostics: bounded by HOST_TRACE; safe to call from builtins.
void unwind_trace(const char* fmt, ...);

}  // namespace host
