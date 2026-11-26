# Minimal C++ Wasmtime-ABI host (`host/`)

Proves that the 342 MB Cranelift-emitted object from
`tools/wasm2obj` (`/tmp/opencode/out.full.o`) can be linked into an ordinary
C++ executable and that **real wasm exports can be called through a
hand-assembled Wasmtime VMContext**. No `wasmtime::Engine`, `Store`, JIT or
runtime library is used at runtime.

## Files

| File | Role |
| --- | --- |
| `gen_vmctx.rs`, `Cargo.toml`, `Cargo.lock` | Rust generator: translates the wasm (parse only, no codegen) and prints exact `VMOffsets` from **wasmtime-environ 50.0.2** |
| `vmctx_offsets.h` | Generated header (checked in so no Rust is needed to build) |
| `vmctx.h`, `vmctx.cpp` | VMContext layout, 4 GiB guarded memory, 8 builtins, 341 import stubs, element-section funcref table, signal-to-error trap handling |
| `gen_table.py`, `table.h` | Generator + checked-in header for the wasm element section; `table.cpp` is generated at build time (16 MB, gitignored) |
| `main.cpp` | Smoke test: calls real exports |
| `Makefile` | clang++ build / bounded run; regenerates `table.cpp` from the wasm |

Offsets are **never hand-guessed**. Sources of truth (all in
`wasmtime-environ-50.0.2`): `src/vmctxtypes.rs` (`for_each_vmctx_type!`),
`src/vmtypes.rs`, `src/vmoffsets.rs` (`VMOffsets::new(HostPtr, &module)`),
`src/builtin.rs` (`BuiltinFunctionIndex`). Regenerate with:

```sh
cd host
cargo run --offline --release --bin gen_vmctx -- /tmp/opencode/WebClient.raknet.wasm > vmctx_offsets.h
```

Cross-check against the object's disassembly: `_wasm_function_0` does
`mov 0x8(%rdi),%r10; mov 0x18(%r10),%r10` → `store_context` at 8,
`stack_limit` at 24; `wasmtime_builtin_memory_copy` does
`mov 0x10(%rdi),%rax; mov 0x8(%rax),%rax; call *%rax` → builtins array at
16, `BuiltinFunctionIndex::memory_copy() == 1`.

## Key layout facts (generated, module-specific)

* `VMContext` size **3,046,200 B**, magic `"core"` at 0, store context at 8,
  builtins at 16, epoch pointer at 24, GC heap data at 32, type IDs at 40.
* 341 imported functions at 72, stride 32 (`VMFunctionImport`):
  `array_call` 0, `wasm_call` 8, `type_index` 16, `vmctx` 24.
* The module's single memory is **imported** (`num_defined_memories == 0`):
  `VMMemoryImport` at 48 points at a host-owned `VMMemoryDefinition`
  (`base` 0, `current_length` 8).
* 1 defined table at 10,984 (16 B entry: `base`, `current_elements`),
  15 globals at 11,008 (16 B each), 94,841 escaped funcrefs at 11,256.
* Builtin array length 51. The eight trampolines present in the object use
  indices: memory.copy 1, memory.fill 2, table.get_lazy_init_func_ref 7,
  memory.atomic.notify 9, memory.atomic.wait32 10, gc_alloc_raw 24,
  get_instance_id 43, throw_ref 44.

## What the host supplies

1. **Memory**: `mmap` of 4 GiB + 1 MiB guards on each side (`PROT_NONE`
   reservation, `MAP_FIXED` RW commit), `current_length = 4 GiB`.
2. **VMContext**: zeroed 3,046,200 B block; magic, store pointer, builtins,
   epoch counter, GC/type-id dummies, imported-memory descriptor, 341 import
   slots (generic wasm-ABI stub returning 0), table of funcref pointers.
   `stack_limit = 0x1000` so generated stack checks always pass.
3. **Builtins**: real `memory.copy`/`memory.fill` (bounds-checked `memmove`/
   `memset`), non-blocking `atomic.wait32`/`notify`, safe stubs for
   `throw_ref`, `table_get_lazy_init_func_ref`, `gc_alloc_raw`,
   `get_instance_id`. Remaining 43 slots are harmless stubs.
4. **Traps**: `sigaction` for SIGSEGV/SIGFPE/SIGILL/SIGBUS with `SA_SIGINFO`.
   Logs the faulting PC and address via `write(2)`, then `siglongjmp`s back to
   the caller. Faults outside a wasm call restore `SIG_DFL` and re-raise.

## Build

`make` first regenerates `table.cpp` from the module's element section:

```sh
cd /tmp/opencode/wt-cpp-host/host
python3 gen_table.py --wasm /tmp/opencode/WebClient.raknet.wasm --out-dir .
make
```

Only `table.h` is checked in; `table.cpp` (161,490 entries, ~16 MB of C++)
is a deterministic build artefact.  Cranelift's `_wasm_function_N` symbols are
GLOBAL HIDDEN in the object and become LOCAL in the executable, so the
generated source uses direct `extern "C"` references rather than `dlsym`.
`type_ids[i] = i` makes the VMContext-side type indices match the wasm type
indices stored in each funcref for `call_indirect`.

Exact linker command (clang++ 21 on this image; `clang++` is forced in the
Makefile):

```sh
clang++ -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -I. \
  -o demo main.cpp vmctx.cpp imports.cpp runtime_data.cpp table.cpp \
  /tmp/opencode/out.full.o -rdynamic -lpthread -ldl -lm -lcurl
```

`make demo` produced:

* linker diagnostic: `/usr/bin/ld: error in /tmp/opencode/out.full.o(.eh_frame);
  no .eh_frame_hdr table will be created` (non-fatal; exit status 0 — the
  object carries `.eh_frame` but no `.eh_frame_hdr`, so unwinding metadata is
  incomplete; none of the smoke tests need it).
* `demo`: **360,164,096 bytes** with the generated table; input object:
  **342,188,600 bytes**.

## Evidence (all runs bounded with `timeout 30s`)

`./demo --skip-ctors` (exit 0):

```text
[wasm-host] vmctx=0x7fcc2bb24010 size=3046200 memory=0x7fcb2b900000 length=4294967296 imports=341 builtins=51
[wasm-host] ABI ROUNDTRIP OK: emscripten_stack_set_limits -> get_stack_base=305418240 get_stack_end=305422336
[wasm-host] __wasm_call_ctors: skipped (--skip-ctors)
[wasm-host] import calls=0 controlled errors=0
[wasm-host] malloc(16) (_wasm_function_149984): returned 0 (0x0)
[wasm-host] RESULT=OK
```

`305418240 == 0x12345000` and `305422336 == 0x12346000`, the two values
written by the exported `emscripten_stack_set_limits` (`_wasm_function_150011`)
and read back by exported `get_stack_base` (`_wasm_function_499`) and
`get_stack_end` (`_wasm_function_500`). **This is a genuine wasm→host→wasm
round trip through VMContext globals; the VMContext ABI works.**

`./demo --test-trap` (exit 0) additionally produced, on stderr:

```text
[wasm-host] TRAP signal=4 pc=0x000055bc12c32f84 fault_addr=0x000055bc12c32f84
```

and on stdout:

```text
[wasm-host] exported __trap: converted to controlled error
...
[wasm-host] malloc(16) (_wasm_function_149984): returned 0 (0x0)
[wasm-host] RESULT=OK
```

Signal 4 = SIGILL from the guest's `ud2`; execution continued afterwards.
`malloc` returns 0 in this skeleton because Emscripten's allocator globals were
never initialised (no ctors) — but the call returns normally, which is the ABI
fact under test.

## Status of full `__wasm_call_ctors`

The historical failures (spinning cleanup list, then SIGILL in an indirect
call) are described in `CTORS_FINDINGS.md`.  With the real import table,
passive data segments and element-segment funcrefs installed, `timeout 30s
./demo` now exits 0 with `__wasm_call_ctors: returned normally` and
`RESULT=OK`.

No game session was started.
