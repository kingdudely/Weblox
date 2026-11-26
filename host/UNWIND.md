# Minimal wasm exception propagation in the C++ host (`feat/cpp-unwind`)

## Goal and result

Determine whether `throw_ref` during `initialize()` can reach a guest `catch`
handler without replacing Cranelift codegen.  **Yes.**  The compiled object
already contains the `try_call` landing pads; the host only needed to read the
serialized exception table, walk the guest frame-pointer chain, and resume into
the landing pad with Wasmtime's payload register convention.

After this change, the `SIGILL`/controlled trap at `throw_ref` is gone.  In the
bounded join run the guest catches and rethrows exceptions repeatedly (13
resumptions into recorded landing pads), `initialize()` finishes, and later
engine phases (`RunDynamicInitializers`, `CrashReporter init`,
`gameGlobalInit`, `LoadFlags`, `checkMimallocIntegration`) run.  The run still
ends with a **later, different** `SIGSEGV` after
`env._emscripten_notify_mailbox_postmessage`, and `connects=0`: RNA's connect
path is not reached yet.  No caught exception was faked -- every dispatch below
used a handler record read out of the object's exception table.

## Evidence: what the object and Wasmtime 50 actually contain

`readelf -SW /tmp/opencode/out.full.o`:

```
[ 1] .text                 ...  size 0x10b9a000
[ 3] .wasmtime.stackmap    ...  size 0x0054dc04
[ 5] .wasmtime.exceptions  ...  size 0x01e8e58c
```

`readelf -sW` shows `_wasm_function_0` at symbol value 0, i.e. the compiled
code segment starts with that export.  In the linked PIE (`nm -n demo`) the
same symbol is at `0x496000`; therefore exception-table offsets are
`pc - &_wasm_function_0`.

The section parses exactly as `ExceptionTableBuilder::serialize` in
`wasmtime-internal-unwinder-50.0.2/src/exception_table.rs`:

```
u32 callsite_count = 1,122,303
u32 handler_count  = 1,547,724
u32[cs] callsites, u32[cs] frame_offsets, u32[cs] ranges,
u32[hs] tags, u32[hs] contexts, u32[hs] handlers
8 + 12*cs + 12*hs == 0x1e8e58c   (section size, exact)
```

`lookup_pc` is a binary search over `callsites` (return addresses, ascending);
`ranges[i-1]..ranges[i]` selects that callsite's handler records.  `tags` are
module-local tag indices or `0xffffffff` (catch-all), `contexts` are SP offsets
holding the dynamic vmctx (`0xffffffff` = none), `handlers` are code-section
offsets.  Stats: 1,525,154 catch-all handlers, 20,891 tag 1, 1,679 tag 0;
every handler has a context slot.

`objdump -d --start-address=0x10b99466 /tmp/opencode/out.full.o` shows the
host-facing trampoline for `throw_ref`:

```
mov 0x8(%rdi),%rax          ; VMStoreContext
mov %rbp,0x30(%rax)         ; last_wasm_exit_trampoline_fp
mov 0x8(%rbp),%rcx
mov %rcx,0x38(%rax)         ; last_wasm_exit_pc (guest return address)
call *builtins[44]          ; our builtin_throw_ref
test %al,%al; je builtins[41]; ud2   ; false -> raise -> trap
```

So the host builtin can start the unwinder exactly where Wasmtime does:
`exit_pc = last_wasm_exit_pc`, `fp = *(last_wasm_exit_trampoline_fp)`.

Dispatch path in Wasmtime 50 (`src/runtime/vm/throw.rs`,
`traphandlers.rs`, `wasmtime-internal-unwinder/src/throw.rs`,
`stackwalk.rs`, `arch/x86.rs`): walk FPs until a `try_call` handler matches;
restore `rsp = fp - frame_offset`, `rbp = fp`, put the two exception payloads
in `rax`/`rdx` (`cranelift-codegen-0.137.2/src/isa/x64/abi.rs`:
`exception_payload_regs = [rax, rdx]`), and jump to
`code_base + handler_offset`.  `wasmtime-internal-cranelift/src/func_environ/gc.rs`
(`translate_throwing_builtin`) confirms `throw_ref`'s callsite itself is a
`try_call` carrying the enclosing `catch` handlers, and the catch block takes
one payload (`TryCallExn(0)`), reduced to the 32-bit `exnref`.

## What is implemented

* `host/unwind.cpp` / `host/unwind.h` (new): at `setup()` time, locate
  `.wasmtime.exceptions` in `/proc/self/exe` and parse its six arrays.  The
  executable is a PIE, so the ELF section address is corrected with the load
  bias from `dl_iterate_phdr` (missing this caused an immediate segfault while
  reading the table; it is now verified by a successful `setup()`).
* `unwind_throw(vmctx, exnref)`:
  * reads `last_wasm_exit_pc` / `last_wasm_exit_trampoline_fp` from
    `VMStoreContext`, starts at `(pc, fp) = (exit_pc, *trampoline_fp)` and
    walks the Cranelift FP chain (`pc = [fp+8]`, `fp = [fp]`), bounded to 96
    frames and 64 MiB frame offsets;
  * binary-searches the exception table for each frame's PC and enumerates
    that callsite's handlers in order;
  * accepts a catch-all handler, or a tagged handler only when it can verify a
    match: the tag's vmctx read from the recorded SP context slot equals the
    throwing vmctx, and the module-local tag equals the exception object's
    defined tag.  The exception object's tag fields were identified from the
    bounded object dump (DRC object at heap index 8: words at +24/+28 are
    `1, 1` = instance 1, defined tag 1).  No tag match is guessed.
  * resumes with `__asm__("mov rsp; mov rbp; jmp *pc")` and
    `rax = exnref, rdx = 0`.
* `host/vmctx.cpp`: `builtin_throw_ref` now calls `unwind_throw` instead of
  unconditionally returning false; `builtin_raise` remains the honest trap
  for unhandled throws and GC OOM.  A bounded `HOST_TRACE` dump of the
  exception object records the tag evidence.
* `host/Makefile`: builds and links `unwind.cpp`.

## Verification (bounded runs, this worktree)

```
make -j4                         -> demo (pre-existing harmless .eh_frame warning)
timeout 60s ./demo               -> exit 0, RESULT=OK, ctors OK, errors=0
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo   -> exit 0, controlled errors=0
```

Join-run highlights (`host/join_unwind_final.log`, untracked):

```
[unwind] table: 1122303 callsites, 1547724 handlers at ... (32040332 bytes)
[trace] gc_alloc_raw kind=0x4000000 ty=894 size=40 align=8 -> heap_index=8
[trace] throw_ref exnref=0x8
[unwind] exception tag: instance=1 defined=1
[unwind] frame 1 pc=... callsite=654134 handler[901774] tag=0xffffffff ctx=0x8 target=0x8e8ffc3
[unwind] FOUND handler pc=... sp=... fp=...
...
[unwind] frame 1 pc=... callsite=26316 handler[29596] tag=0x1 ctx=0x28 target=0x93cc0e
[unwind] FOUND handler pc=...                       <- verified tagged catch
[log ] [WASM-TIMING] LoadFlags / checkMimallocIntegration ...   <- initialize() continued
[import] first call slot=340 env._emscripten_notify_mailbox_postmessage ...
[rna-main] __main_argc_argv: trapped via signal 11
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
[wasm-host] controlled errors=0
```

13 recorded landing pads were entered; 3 of them were tag-1 handlers matched
through the recorded dynamic context.  The old
`[wasm-host] controlled error: throw_ref called (exception propagation not
implemented)` / `raise -> ud2` sequence is absent.

Note: three earlier join attempts were diagnostic, not verification: the first
hit a typo in the ELF magic check (table unavailable), the second hit a PIE
load-bias bug (immediate segfault), and a third exposed an over-strict SP
sanity check that rejected valid frames.  The run quoted above is after all
three fixes and is the one used for the result.

## Limits

* The guest's `catch` handlers are entered, but the host does not maintain
  Wasmtime's `pending_exception` in a real store, stack maps, or DRC root
  scanning.  No collection is performed (see `host/GC.md`), so this is safe
  only for the bounded bootstrap window.
* Tag matching assumes a single instance (`get_instance_id() == 1`) and that
  the object's defined tag index equals the module-local index.  Both held in
  the observed run; a multi-instance or imported-tag module needs the full
  `Instance::get_exported_tag` resolution.
* The frame walk stops at 96 frames; there is no `last_wasm_entry_fp` bound
  because this host calls exports directly rather than through entry
  trampolines.  It relies on `-fno-omit-frame-pointer` (already in the
  Makefile) and Cranelift's preserved frame pointers.
* `builtin_raise` still traps when no handler is found; that is intentional
  (no unhandled-exception recovery to the host).
* The later `SIGSEGV` is downstream of exception handling (after mailbox
  postmessage, with no unwind activity in the log).  This change does not
  claim to fix it, and `connects=0` remains: RNA imports are not reached in
  this join path yet.
