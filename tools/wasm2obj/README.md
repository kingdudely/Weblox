# wasm2obj — Cranelift straight to a native `.o`

**Verdict: yes.** The patched `WebClient.raknet.wasm` can be compiled by
Cranelift into a plain x86-64 ELF relocatable object, with **no wasmtime
runtime engine / `Engine` / `Instance` / `Store`**. It took ~4 minutes on 8
cores and produced a 342 MB `.o` with 154,552 compiled wasm functions.

```
$ file out.full.o
ELF 64-bit LSB relocatable, x86-64, version 1 (SYSV), not stripped
```

Build/run:

```sh
cd tools/wasm2obj
cargo build --release
./target/release/wasm2obj --all --out out.o /tmp/opencode/WebClient.raknet.wasm
# bounded demo (does not compile every function; unresolved callees get stubs):
./target/release/wasm2obj --max-funcs 16 --out small.o <wasm>
```

## 1. API research (what actually works)

| Route | Result |
| --- | --- |
| `cranelift-wasm` + `cranelift-object` `ObjectModule::finish()` | **Dead end.** `cranelift-wasm` has no 0.137.2 release; it was last published as **0.112.3** (depends on wasmparser 0.217 / cranelift 0.112). Its `code_translator.rs` has no `try_table` handling at all, and its `wasm_features()` returns `WasmFeatures::default()`. It cannot parse/translate this module. `cranelift-object` 0.137.2 *does* exist, but needs a hand-written `FuncEnvironment`/`ModuleEnvironment` and still wouldn't understand exnref. |
| Vendored `wasmtime-internal-cranelift` 50.0.2 + `wasmtime-environ` compiler APIs | **Works.** No runtime engine is constructed. The needed public APIs are: `builder(None)` → `CompilerBuilder::set_tunables/build`, `ModuleEnvironment::translate`, `Compiler::compile_function`, `Compiler::compiled_function_relocation_targets`, `Compiler::compile_trampoline`, `Compiler::object(ObjectKind::Module)`, `Compiler::append_code`, then `object::write::Object::write()`. |

The tool follows Wasmtime's own `compile.rs` pipeline but stops after
`append_code`: it deliberately does **not** call `serialize_info`, `ObjectBuilder`,
or anything from the `wasmtime` crate. The compiler's `Object` is written as-is
(the private Wasmtime OSABI/e_flags are reset to `OsAbi(0)`/`FileFlags(0)` so
the result is an ordinary SysV ELF). Defined symbols are promoted from
`SymbolScope::Compilation` to `SymbolScope::Linkage` so C++ can refer to them.

```text
translate (wasmparser + ModuleEnvironment)
  -> compile_function per defined func  (rayon, 8 threads)
  -> compile_trampoline for referenced builtins
  -> compiler.object(ObjectKind::Module)
  -> compiler.append_code(...)
  -> Object::write() -> out.o
```

## 2. Measured results (patched 102,267,785-byte module)

Tool build time (cold, release, all deps): **3 m 50 s**; incremental rebuild
after edits: 2–12 s. Debug build of the first working tool: 1 m 29 s cold.

| Phase | Time |
| --- | --- |
| read wasm | 0.14 s |
| parse + validate + translate | **0.118 s** |
| compile 154,552 functions (rayon, `opt_level=none`, inlining off) | **250.2 s** (first run 227.6 s) |
| builtins + `append_code` + write object | **9.0 s** (first run 10.5 s) |
| **total** | **261.6 s** (first run 240.9 s) |

A sequential attempt reached 39,000 functions in 300 s and was killed;
adding rayon was the only change needed to finish in-bounds.

Output object (full run):

* **342,188,600 bytes**, `ELF 64-bit LSB relocatable, x86-64, SysV`.
* Sections: `.text` (~280 MB), `.eh_frame`, `.wasmtime.stackmap`,
  `.wasmtime.traps`, `.wasmtime.exceptions`, `.symtab`, `.strtab`, `.shstrtab`.
* **154,560 global symbols** (`T`): 154,552 `_wasm_function_N` plus 8
  `wasmtime_builtin_*` trampolines: `memory_fill`, `memory_copy`,
  `memory_atomic_wait32`, `memory_atomic_notify`, `throw_ref`,
  `table_get_lazy_init_func_ref`, `gc_alloc_raw`, `get_instance_id`.
* **0 undefined symbols, 0 static relocations.** Imports are *not* ELF
  symbols: compiled code calls imported functions indirectly through pointers
  in the VMContext. A linker will not complain about the 341 imports, but the
  host must fill those pointers in before calling anything.

Bounded smoke run (debug build, `--max-funcs 4`): translation 0.99 s, compile +
stubbing 218.3 s, object 11,592,144 bytes / 4,134 functions, only local symbols
(before the global-symbol fix). Do not use those stubs for execution.

## 3. Feature acceptances / errors seen

* Features enabled: `WasmFeatures::WASM3 | SHARED_EVERYTHING_THREADS |
  LEGACY_EXCEPTIONS`. Threads + atomics, SIMD, exnref/`try_table`, tail calls,
  bulk memory and shared memory were all **accepted** — zero validation or
  translation rejections. Counts reported: 154,552 defined functions, 341
  function imports + 1 memory import, 1 memory, 84 exported functions.
* **Real blocker #1 (fixed):** `Tunables::default_host()` sets
  `collector: None`; the first function using exnref/GC panicked with
  `Unsupported("support for GC types disabled at configuration time")` at
  `wasmtime-internal-cranelift/src/func_environ/gc.rs:1656`. Set
  `tunables.collector = Some(Collector::DeferredReferenceCounting)`.
* Build plumbing: `wasmtime-environ` needs the `std` feature and
  `wasmtime-internal-core` needs `anyhow` for `?`/`.context()` on its `Error`.
* `--all` initially fell through to the 16-function default; fixed.
* `cranelift-wasm` 0.112.3 route: `try_table` unsupported (no code), so it is
  not a viable fallback even with matching old crates.

## 4. What a C++ host must supply

The object is Wasmtime-ABI machine code, not a freestanding C ABI. Linking is
trivial; **making it run is the hard part**.

### VMContext layout (source of truth, do not guess offsets)

* `wasmtime-environ-50.0.2/src/vmoffsets.rs` (`VMOffsets`) — every offset used
  by generated code, parameterised by pointer size and tunables.
* `wasmtime-environ-50.0.2/src/vmctxtypes.rs`, `vmtypes.rs` — the struct fields:
  instance pointer, stack limit, builtin-function array, imported-function
  array, memory base/bound/current length, table pointers, GC heap pointers,
  vmctx itself, array-call trampoline, last-error/epoch/fuel slots.
* Disassembly confirms the shape: `_wasm_function_0` loads `0x8(%rdi)` →
  instance → stack limit at `0x18`; builtin trampolines load a host function
  pointer from `0x10(%rdi)` → builtins array + offset from
  `BuiltinFunctionIndex`. `rdi` is always the VMContext pointer.

Allocate a VMContext with exactly this layout (or generate a C header by
walking `VMOffsets` in a small Rust helper) and fill in: stack limit, memory
base/length/bound, table bases/lengths, imported function pointers, builtin
libcall pointers, GC heap base, and instance pointer.

### libcalls / builtins

The eight trampolines in the object call through the builtins array. The host
must provide real implementations for at least: `memory.copy`, `memory.fill`,
`memory.atomic.wait32`, `memory.atomic.notify`, `throw_ref`,
`table.get_lazy_init_func_ref`, `gc_alloc_raw`, `get_instance_id` (plus any
others that `BuiltinFunctionIndex::all()` enables for your tunables — the
object only contains the ones actually referenced by this module). Traps use
the `.wasmtime.traps` table and signal handling; without a Wasmtime-style
signal handler a trapping guest will crash instead of returning an error.

### Imports

341 function imports (Emscripten/env, JS, GL, fetch, etc.) are **not** undefined
symbols; generated code loads a function pointer from the imported-function
array in the VMContext. Fill all 341 slots with C++/C functions using the Wasm
ABI (vmctx in `rdi`, wasm args in registers). No `-l` flag will satisfy them.
This is the same import surface already implemented in `v3-wasm2c/*.c` and
`v2-wasmtime/src/imports/`; those signatures can be reused.

### Exports / calling from C++

All `_wasm_function_N` are now global `T` symbols, e.g.:

```cpp
extern "C" void _wasm_function_149984(void* vmctx, /* wasm args */); // malloc
```

`N` is the **defined-function index**, not the wasm function index. This module
has 341 imported functions, so `N = wasm_func_index - 341`. The 84 exports are
listed in the wasm export section: `__wasm_call_ctors`(341), `malloc`(150325),
`free`(150327), `realloc`(150329), `__main_argc_argv`(488),
`main_loop_iteration`(517), `rbx_fs_*`, `pthread_self`(150209), etc. Use
`wasm-objdump -x` or wasmparser to regenerate the full map.

For a normal C ABI entry point, also compile `FuncKey::ArrayToWasmTrampoline`
for the exported function (Wasmtime's `compile_trampoline`) and name it as the
extern "C" symbol; the current tool only emits the raw wasm-ABI functions.

### Bottom line

The .o is real, plain, and linkable. The missing piece is not a compiler — it
is the ABI-compatible VMContext + libcalls + 341 import stubs. That is roughly
the same glue `v3-wasm2c` already implements, adapted to Wasmtime's VMOffsets.
