// Minimal C++ Wasmtime-ABI host for tools/wasm2obj/out.full.o.
//
// All offsets in vmctx_offsets.h are *generated* by gen_vmctx.rs from
// wasmtime-environ 50.0.2's VMOffsets. Never hand-guess an offset here.
#pragma once

#include "vmctx_offsets.h"

#include <cstddef>
#include <cstdint>

extern "C" {
// Raw wasm-ABI exports: rdi = callee vmctx, rsi = caller vmctx, wasm args
// start in rdx (Wasmtime 50 wasm-call ABI).
void _wasm_function_0(void*, void*);                      // __wasm_call_ctors
void _wasm_function_3(void*, void*);                      // wasm start / init memory
uint32_t _wasm_function_149984(void*, void*, uint32_t);   // malloc
uint32_t _wasm_function_43(void*, void*);                 // tiny no-op (not exported)
void _wasm_function_2(void*, void*);                      // exported __trap (ud2)
void _wasm_function_150011(void*, void*, uint32_t, uint32_t); // set_stack_limits
uint32_t _wasm_function_499(void*, void*);                // get_stack_base
uint32_t _wasm_function_500(void*, void*);                // get_stack_end

// Builtin trampolines present in out.full.o. The object's trampolines call
// through the builtins array; these symbols are only declared so that the
// linker keeps the object and so we can cross-check indices if needed.
void wasmtime_builtin_memory_copy(void*);
void wasmtime_builtin_memory_fill(void*);
void wasmtime_builtin_memory_atomic_notify(void*);
void wasmtime_builtin_memory_atomic_wait32(void*);
void wasmtime_builtin_throw_ref(void*);
void wasmtime_builtin_table_get_lazy_init_func_ref(void*);
void wasmtime_builtin_gc_alloc_raw(void*);
void wasmtime_builtin_get_instance_id(void*);
}

namespace host {

// Layout mirrors wasmtime-environ 50.0.2 vmtypes.rs. Sizes are asserted both
// here and against the generated offsets in vmctx.cpp.
struct VMMemoryDefinition {
  uint8_t* base;
  size_t current_length;
};
struct VMTableDefinition {
  uint8_t* base;
  size_t current_elements;
};
struct VMFunctionImport {
  void* array_call;
  void* wasm_call;
  uint32_t type_index;
  uint32_t reserved;
  void* vmctx;
};
struct VMFuncRef {
  void* array_call;
  void* wasm_call;
  uint32_t type_index;
  uint32_t reserved;
  void* vmctx;
};
struct VMMemoryImport {
  VMMemoryDefinition* from;
  void* vmctx;
  uint32_t index;
  uint32_t reserved;
};

// VMStoreContext (repr(C, align(8))) with the fields generated code touches.
struct alignas(8) VMStoreContext {
  int64_t fuel_consumed;
  uint64_t epoch_deadline;
  uint64_t execution_version;
  uintptr_t stack_limit;
  VMMemoryDefinition gc_heap;
  uintptr_t last_wasm_exit_trampoline_fp;
  uintptr_t last_wasm_exit_pc;
  uintptr_t last_wasm_entry_sp;
  uintptr_t last_wasm_entry_fp;
  uintptr_t last_wasm_entry_trap_handler;
  uintptr_t stack_chain[2];
  void* store_data;
  uint8_t async_guard_range[2 * sizeof(void*)];
  uint32_t component_context[2];
  uintptr_t current_thread;
};

// The host-side state; VMContext itself is a raw, dynamically sized byte block.
struct HostState {
  uint8_t* vmctx = nullptr;
  VMStoreContext store_storage{};
  VMMemoryDefinition memory_def_storage{};
  void* builtin_slots[BUILTIN_FUNCTIONS_ARRAY_LEN]{};
  uint8_t* memory_base = nullptr;
  size_t memory_reservation = 0;
  uint8_t* table_storage = nullptr;
  VMFuncRef* table_funcrefs = nullptr;
  uint64_t epoch = 0;
  volatile uint32_t error_count = 0;
  uint64_t import_call_count = 0;
};

// Set up everything and return true on success. Logs details on failure.
bool setup(HostState& state);

// Signal-handler plumbing. Returns 0 on normal return, or the signal number
// when a wasm trap was converted into a longjmp.
int call_ctors(HostState& state);
uint32_t call_malloc(HostState& state, uint32_t size, int& trap_signal);

// Real-export round trip: emscripten_stack_set_limits -> get_stack_base/end.
// Returns 0 on success, or the signal number of a converted wasm trap.
int call_stack_roundtrip(HostState& state, uint32_t* base_out, uint32_t* end_out);

// Calls the exported __trap and returns the converted signal number.
int call_trap_test(HostState& state);

// Controlled-error bookkeeping used by builtins/import stubs.
void note_error(const char* what);

inline uint8_t* at(void* vmctx, size_t offset) {
  return static_cast<uint8_t*>(vmctx) + offset;
}
inline const uint8_t* at(const void* vmctx, size_t offset) {
  return static_cast<const uint8_t*>(vmctx) + offset;
}

}  // namespace host
