# GC / builtin work for the C++ host bootstrap (`feat/cpp-bootstrap`)

## Goal and result

The join run previously trapped with SIGILL immediately after
`gc_alloc_raw called` / `unimplemented builtin called` and `connects=0`.
This branch replaces the stubbed builtins with honest minimal implementations
backed by a real, bounded GC heap. The SIGILL is gone: the joined session now
reaches `initialize()`, allocates its first GC object successfully, and then
traps on **native exception propagation** (`throw_ref`), which this host does
not implement. It still does not reach `main_loop_iteration`, and RNA still
reports `connects=0`.

## What is implemented, and from which Wasmtime 50 source

The module was compiled with `Collector::DeferredReferenceCounting` (DRC):
`host/gen_vmctx.rs:23` sets `tunables.collector =
Some(Collector::DeferredReferenceCounting)`. Regenerating the ABI header with
that tunable gives `BUILTIN_FUNCTIONS_ARRAY_LEN == 51` and the builtin indices
used in `host/vmctx_offsets.h`.

Sources (all wasmtime 50.0.2 in `~/.cargo/registry/src/index.crates.io-…/`
plus `vendor/wasmtime-internal-cranelift` in this worktree):

| Source | What it established |
| --- | --- |
| `wasmtime-environ/src/builtin.rs` (`foreach_builtin_function`, `trap_sentinel`) | `gc_alloc_raw(kind, shared_type_index, size, align) -> u32` is a **falsy-trap** libcall; `throw_ref -> bool` is falsy-trap; `get_instance_id -> u32` cannot trap; `table_get_lazy_init_func_ref -> pointer` cannot trap. Indices 40=`trap`, 41=`raise`, 45=`force_gc`. |
| `wasmtime-50.0.2/src/runtime/vm/libcalls.rs` (`gc_alloc_raw`, `get_instance_id`, `throw_ref`, `table_get_lazy_init_func_ref`) | The `u32` returned by `gc_alloc_raw` is a raw `VMGcRef` (a 2-byte-aligned, non-zero **byte index** into the GC heap), not a linear-memory pointer. `get_instance_id` returns the instance id. `throw_ref` calls `store.set_pending_exception()` and returns an `Err` so the generated `try_call` unwinds natively. |
| `wasmtime-50.0.2/src/runtime/vm/gc/gc_ref.rs`, `wasmtime-environ/src/gc.rs` | `VMGcKind` occupies the top 6 bits (`ExnRef = 0b000001<<26 = 0x04000000`, `StructRef = 0x2c000000`, …); `VMGcHeader = { kind: u32, ty: u32 }`, 8 bytes; null ref is 0, i31 discriminant is bit 0 (so heap indices must be even). |
| `wasmtime-environ/src/vmtypes.rs` (`VMDrcHeader`, `VMDrcHeapData`) | `VMDrcHeader` = `VMGcHeader` (0/4), `ref_count: u64` @8, `next_oasr: u32` @16, `object_size: u32` @20, 24 bytes / align 8. `VMDrcHeapData` = OASR head/lengths, all zero = empty list, reached via `vmctx+VMCTX_OFF_GC_HEAP_DATA (32)`. |
| `vendor/wasmtime-internal-cranelift/src/func_environ/gc.rs` (`get_gc_heap_base`, `prepare_gc_ref_access`) | Generated field accesses compute `gc_heap.base + heap_index + field_offset` and bounds-check against `gc_heap.current_length` (`VMStoreContext` +32/+40). |
| `vendor/wasmtime-internal-cranelift/src/func_environ/gc/drc.rs` (read/write barriers, `emit_gc_raw_alloc`, `emit_maybe_force_gc`) | The emitted DRC barriers read/write `VMDrcHeader.ref_count` and the OASR list; `force_gc` is called when OASR length ≥ 1024. These are inline in the object, not libcalls. |
| `objdump` of `wasmtime_builtin_gc_alloc_raw` in `out.full.o` | The trampoline calls `builtins[24]`, then `test eax,eax; je <builtins[41]; ud2` — confirming the falsy trap path and that slot 41 is the re-raise/trap path. |

### Implemented in `host/vmctx.cpp`

* A real 256 MiB anonymous GC heap (`mmap`), with
  `VMStoreContext.gc_heap = {base, 268435456}`. `builtin_gc_alloc_raw` aligns the
  bump pointer to `max(align, 8)`, reserves index 0 for null, writes the
  `VMDrcHeader` (kind, shared type index, `ref_count = 1`, empty OASR link,
  `object_size`) and returns the even, non-zero heap index. It never returns 0
  for a valid request; only genuine exhaustion of the bounded heap traps.
* `VMCTX_OFF_GC_HEAP_DATA` points at a zeroed `VMDrcHeapData`, so the generated
  DRC read barrier sees an empty over-approximated-stack-roots list.
* `builtin_table_get_lazy_init_func_ref` now returns the actual `VMFuncRef` from
  the eagerly populated table (`table.h`/`table.cpp`) or null — it no longer
  fabricates a funcref with type index 1.
* `get_instance_id` returns a stable non-zero id (1). `force_gc` is an honest
  no-op returning true (no collection is ever performed). `trap`/`raise`
  execute `ud2`, which the existing signal handlers convert into a controlled
  trap instead of a misleading "unimplemented builtin" error. `throw_ref` logs
  and returns false (not caught) — see blocker below.
* `HOST_TRACE=1` adds bounded, per-key capped trace lines for every builtin
  slot. Every unimplemented slot gets a distinct tracing stub carrying its
  builtin index, so the *next* builtin that traps can be identified from one
  run. Inputs are capped (e.g. 12 `gc_alloc_raw` lines, 128 unknown-builtin
  lines) so a joined run cannot flood the log.

## Verification (bounded, one join attempt)

```
make -j4                                       -> demo (link warning about
                                                  .eh_frame_hdr is harmless)
timeout 60s ./demo                             -> exit 0, RESULT=OK, ctors OK,
                                                  controlled errors=0
HOST_TRACE=1 timeout 60s ./demo                -> exit 0, no builtin trace lines
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  timeout 90s ./demo --rna-probe               -> exit 0, connects=1, status=200,
                                                  callbacks=1/0, trap=0
HOST_TRACE=1 WC_PLACE=189707 \
  WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo           -> exit 0 (controlled trap),
                                                  connects=0
```

The join run (`host/join_run.log`, untracked) now reaches:

```
[log ] [WASM-TIMING] initialize() entered: +1673.2ms from preInit
[trace] get_instance_id -> 1
[trace] gc_alloc_raw kind=0x4000000 ty=894 size=40 align=8 -> heap_index=8
[trace] throw_ref exnref=0x8 -> not caught
[wasm-host] controlled error: throw_ref called (exception propagation not implemented)
[trace] raise builtin -> ud2 (no native unwinder)
[rna-main] __main_argc_argv: trapped via signal 4
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
```

`kind=0x4000000` is `VMGcKind::ExnRef`: the first GC allocation in
`initialize()` is a 40-byte exception object (shared type index 894), which is
immediately `throw_ref`-ed. This is the new, precisely identified blocker.

## Limits / what is deliberately not implemented

* Bump allocator, **no collection, no free list, no refcount decrements** beyond
  the DRC barriers emitted in guest code. 256 MiB is ample for bootstrap but a
  long session will exhaust it; the honest OOM path returns 0 (trap). Objects
  are never reclaimed, so a real session eventually needs a DRC collector.
* The exception object's tag/instance fields are written by generated code into
  the zeroed allocation; `get_instance_id` always returns 1, which is valid for
  a single instance but not a general identity.
* `throw_ref` cannot propagate. Wasmtime implements exceptions by setting a
  pending exception and **native stack unwinding** through `try_call` sites
  whose exception tables live in the object; `wasmtime-internal-unwinder` walks
  those frames and dispatches to the matching catch block. This host has no
  unwinder, no exception-table registration and no `last_wasm_exit_*` /
  `stack_chain` maintenance, so returning from `throw_ref` always reaches the
  `raise`/`ud2` path. A generic `try_call` return value cannot redirect to a
  catch handler. This is a **fundamentally missing exception implementation**,
  not a missing field — stop here rather than fake a caught exception.
* `force_gc` being a no-op means the OASR list grows without bound; at 1024
  entries generated code calls the no-op and keeps going. Safe for a short
  bootstrap window, not for a full session.

## Engine bootstrap still missing in `host/main.cpp` (not ported: unverifiable
## within the one-join-attempt budget)

* `guest_argv` **is** already equivalent to `v2-wasmtime/src/main.rs:756`:
  `malloc(16)` + `"/content/.keep\0"` + an argv array at `[p, 0]`.
* The main-loop driver already calls `_wasm_function_176`
  (`main_loop_iteration`) each turn and pumps deferred callbacks through
  `wb::imports_set_guest_tick()` (imports 4/5 call
  `weblox::RnaBridge::guest_tick()`). No extra pump is needed for one
  completion per turn; multi-step handlers may still need the page's deferred
  callback queue.
* `prepare()` (`v2-wasmtime/src/main.rs:770`) is **not** ported:
  `set_pretend_platform`, `set_roblox_base_url`, `set_channel`,
  `set_browser_tracker_id`, `set_account_locales`, `set_preloaded_flags`,
  `set_place_id`, VFS `rbx_fs_register_asset` / `rbx_fs_preload_asset` /
  `rbx_fs_add_file` / `rbx_fs_set_base_url`. These bindings are discoverable
  with `wb::embind_find(name)` (`imports.h`) and callable with the global
  `wb_rna_invoke` shim already linked from `rna_bridge.cpp`, so porting is
  mechanically feasible — but with exceptions unimplemented it cannot be
  verified in a join run, and the missing flags/VFS/place id are a plausible
  trigger for the exception thrown at the start of `initialize()`. Port them
  **after** an exception unwinder exists, then re-run the bounded join test.
