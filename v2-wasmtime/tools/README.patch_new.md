# `patch_new.py` — socket-patched WebClient.wasm (new build)

Byte-level adaptation of Weblox's `tools/patch.py`. Target is
`version-1ef59424f70d4932` (input `/home/john/rr/WebClient.wasm`,
102,268,042 B, SHA-256 `be370a4446dcfce62635f7e6d271526b15e2099507fb7250b2d3360eacd4a61f`).

## Run

```sh
python3 v2-wasmtime/tools/patch_new.py \
    /home/john/rr/WebClient.wasm /tmp/opencode/WebClient.new.patched.wasm \
    --all --abi /tmp/opencode/WebClient.new.abi.json
```

`--all` = getaddrinfo + nosocket + nofd + udp_send + portsub + transport
splices/append + stash-peer. Output (102,267,785 B, SHA-256
`eb469e50b1b053a0ef001e1cdb5307b8101c9d966135a58a3497b9fa1ab6ab90`) and
the ABI JSON are generated artifacts and are **not** committed. Ring layout
is unchanged from Weblox (`base 0x7FF00000`, `slot 1536`, 64 slots,
`maxDatagram 1500`); the page bridge must use the emitted ABI.

## Verified absolute function indices (imports included)

| What | Index | Signature / note |
|---|---|---|
| EBADF stub (old 298) | **395** | `()->i32`, errno 8 |
| ENOSYS stub (old 299) | **397** | `()->i32`, errno 138 |
| getaddrinfo (old 301) | **398** | `(i32)->i32`, EAI_FAIL 124 |
| malloc export | **150325** | `(i32)->i32` |
| udp_send (old 127931) | **117578** | `(i32,i32,i32,i32)->i32` |
| recv loop (old 127685) | **117378** | `(i32)->i32`, body 6,809 B |
| port check (old 127810) | **117374** | body 9,093 B |
| UpdateNetworkLoop (old 127660) | **117377** | `(i32)->i32`, local 0 = `this` |
| RakPeer::Connect (old 127655) | **117465** | 10 i32 params |
| appended select / recv | **154891 / 154892** | original total = 154891 |
| imported func count | **341** | defined = 154550 |

Exact bodies asserted:

- 395 `00 10 b6 93 09 41 08 36 02 00 41 7f 0b`
- 397 `00 10 b6 93 09 41 8a 01 36 02 00 41 7f 0b`
- 398 `00 20 00 04 40 20 00 41 00 36 02 00 0b 41 7c 0b`

Byte sites (each must be unique at the given offset inside the function body):

- portsub in 117374, body+8641 (`0x21c1`): `41 c0 e1 93 04 28 02 00` → `41 00`
- recv splice in 117378, body+`0x4EA`: `20 02 10 8b 03` →
  `20 02 20 01 20 00 10 <recv_idx>`; the leading `20 02` (outLen*) stays on
  the stack for the following `i32.store`, and the two call arguments are
  dest=local1, this=local0
- select splice in 117378, body+`0x4C9`: `37 03 c0 01 10 8b 03` →
  `37 03 c0 01 10 <sel_idx>` (`37 03 c0 01` is an `i64.store`, so select has
  **no** arguments, as in the old build)

## Assertions the tool prints and enforces

Imported count 341 and total 154891; the three exact stub bodies above;
signatures for 395/397/398/150325/117578/117378/117377/117465; `malloc`
export resolving to 150325; import func 313 = `env.emscripten_console_log`
and 314 = `env.emscripten_console_warn`; body lengths 9,093 B (117374) and
6,809 B (117378); uniqueness **and** exact offset of the portsub, recv and
select patterns; call target decoded from both splice patterns = 395;
appended indices = 154891/154892. Any mismatch aborts before writing.

## Validation / caveats

- `wasm-tools validate --features all`: **passes** on the patched file.
- `wasm-validate --enable-all` (wabt 1.0.36): fails at file offset `0x60`
  with `expected valid result type (got -0x17)` on **both** the original and
  the patched file — the installed wabt is too old for a valtype used by this
  build, so this is not a patch regression. Use `wasm-tools` for validation.
- Research note said `emscripten_console_log` import index **314**; in this
  binary `wasm-tools print` shows **313** for `console_log` and **314** for
  `console_warn`. The tool asserts both and uses 313 if tracing is enabled.
- `--trace` is disabled: the old `MARKER_STR = 953724` was not re-resolved in
  the new build, so there is no safe marker address to splice.
- Stash-peer relies on local 0 of 117377 being `RakPeer*` (signature
  `(i32)->i32` and unchanged prefix shape were checked statically; no
  runtime execution test was done here). Likewise the ring stubs were
  validated structurally, not by running the engine.
