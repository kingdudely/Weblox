# PATCHING

How to change the engine's behaviour, and how to keep the patches working after
the weekly rebuild. Read this before touching `tools/patch.py`.

## The problem shape

The module has **no name section** and **167,913 functions**. There is no
symbol to grep for. Two identification techniques are known to work, and one is
known to fail — trust that ordering, it costs an afternoon to rediscover.

### ❌ String cross-references — do not use

Literals are interned into one blob. `i32.const 0x2b14c4` lands *inside* the
string `"...torAngles (ignored): %s\0[FLog::Network]"`. Attempting to resolve a
log format string by scanning for its address yields zero hits even for strings
that are definitely used (verified for `getaddrinfo(...) failed`, `Assigned peer
id = %u`, `FLog::Network`).

### ✅ Behavioural shape — use this

Find a function whose *observable behaviour* is the one you saw on the wire.
Example: the log said `Error code - -4` (`EAI_FAIL`), so scan short bodies for
`i32.const -4` returned unconditionally with an out-param nulled:

```
func[301] (i32)->i32
  local.get 0 ; if ; local.get 0 ; i32.const 0 ; i32.store 2 0 ; end
  i32.const 4294967292 ; end
```

That is `if (res) *res = NULL; return EAI_FAIL;`. Correct on the first try.

### ✅ Arity + call-graph narrowing

The type section gives every function's signature. Filter by expected arity
(`socket`/`bind` are `(i32,i32,i32)->i32`), then intersect with callers of a
known leaf (the errno stubs). Cheap and precise when the candidate set is small.

## Current patch set

Indices are for **`version-75f1039cb4964863-RobloxWebClient`** only.

| Name | func | Signature | What it does |
| --- | --- | --- | --- |
| `getaddrinfo` | 301 | `(i32)->i32` | writes a synthetic `addrinfo` (AF_INET, 127.0.0.1:9) and returns 0 |
| `nosocket` | 299 | `()->i32` | ENOSYS catch-all; patched to return 0 (diagnostic) |
| `nofd` | 298 | `()->i32` | EBADF fd-validator; patched to return 0 (diagnostic) |

`nosocket` and `nofd` are **diagnostics**, not the design. They exist to walk the
socket setup ladder one rung at a time:

```
[1] getaddrinfo(...) failed:Error code - -4     -> patched
[2] Unknown bind__() error 0                     -> patched (298)
[3] setsockopt SO_RCVBUF failed : errno=8        -> patched (298)
[4] RakNet Socket bound to invalid port: 0       -> open (getsockname)
```

## Re-resolving after a rebuild

```sh
# 1. fresh disassembly (authoritative; wasm_scan.py is only a helper)
tools/disasm.sh WebClient.new.wasm .cache

# 2. find the EAI_FAIL stub: short body, returns 0xFFFFFFFC, nulls out-param
python3 - <<'PY'
import re
pat = re.compile(r'^\s*[0-9a-f]+ func\[(\d+)\]')
cur, ins = None, []
for line in open('.cache/disasm.txt', errors='replace'):
    m = pat.match(line)
    if m:
        if cur and 3 <= len(ins) <= 12 and any('4294967292' in i for i in ins) and not any('call' in i for i in ins):
            print('candidate func[%d]: %s' % (cur, ' ; '.join(ins)))
        cur, ins = int(m.group(1)), []
        continue
    if '|' in line and cur is not None:
        b = line.split('|', 1)[1].strip()
        if b: ins.append(b)
PY

# 3. confirm by running with only that patch; the getaddrinfo error line must vanish
# 4. update the table above and tools/patch.py PATCHES
```

The errno stubs are recognisable by shape too: `call <__errno_location>; i32.const
<errno>; i32.store; i32.const -1`.

## Adding a patch

1. Add a `patch_<name>()` function in `tools/patch.py` that builds a body with
   `Asm` and returns `a.body([(count, I32), ...])`.
2. Register it in `PATCHES = {name: (abs_func_index, builder)}`.
3. `python3 tools/patch.py in.wasm out.wasm --patch <name>`
4. **Always** `wasm-validate --enable-all out.wasm` before serving.
5. Run `tools/probe/runprobe.js` and diff the log against the previous wall.

## Writing bodies

- Locals: `a.body([(2, I32)])` declares two i32 locals; **params occupy locals
  0..n-1**, so the first declared local is `n`.
- Memory access uses explicit `align, offset`: `i32.store(2, 20)`.
- No new imports are possible. If you need to talk to JS, write to a fixed
  linear-memory address and poll it from the host.
- Call existing exports by index (`MALLOC = 163490`); the export list is in
  `.cache/sections.txt` under `Export[49]`.

## Traps

- **Distro wabt (1.0.36) cannot decode this module** — it dies on `exnref`
  at offset 0x63. `tools/disasm.sh` builds 1.0.42, which works.
- The engine instantiates **in page scope**; workers are the pthread pool.
  `Page.addScriptToEvaluateOnNewDocument` is the only reliable hook point.
- Exported callbacks (`rbx_wasm_rna_on_*`) get re-wrapped by the glue via
  `Object.assign`, so wrapping `instance.exports` is unreliable — tap imports.
- The redirect must keep the module's `application/wasm` MIME or
  `instantiateStreaming` silently falls back (slower, but works).
- `--flags` values go into the `WebClientApp` settings response as strings:
  `FFlagUseRbxTransportClient3=False`.

## Rebuilding the module

`tools/build_patched.sh <stock.wasm> <out.wasm>` runs the full flag set and
validates the result:

```
--patch getaddrinfo --patch nosocket --patch nofd --patch udp_send
--byte-patch portsub --transport --stash-peer
```

Takes ~4 s on this machine and does not renumber function indices.

`CONNECT_ON_SELECT` in `tools/patch.py` is off by default. Leave it off: the
engine's `NetworkClient` calls `RakPeer::Connect` itself once the transport
selection is RakNet. The stub variant exists only to bisect a broken connect —
and note its ten parameters, of which the last three must not be zero
(`connectionAttempts`, `connectionAttemptDelay`, `socketIndex`), or the call
traps with divide-by-zero.

## Host + shell (legacy path)

Kept until the IWA is the only target. The module, the ring and the RUPP fields
are identical in both; only the datagram seam differs.

```
cargo build --manifest-path app/rkhost/Cargo.toml
cargo build --manifest-path app/src-tauri/Cargo.toml

app/src-tauri/target/debug/raknet-client \
  --place 13569002876 \
  --wasm host/extension/WebClient.patched.wasm \
  --engine-dir /tmp/opencode/rw \
  --cookie ~/cookie.txt
```

The shell sets the two WebKitGTK requirements itself
(`JSC_useSharedArrayBuffer=1`, `RKHOST_COEP=require-corp`); `rkhost` serves the
page with COOP/COEP so `SharedArrayBuffer` exists.

Debugging without a console: the page streams its console to `/__log` and posts
`{coi, sab, jspi, wasm, stats}` to `/__client`, both served by `rkhost`. The
bridge prints the first datagrams in each direction with full hex, plus a RUPP
payload-id histogram every 500 sends, which is how the retransmit storm was
diagnosed.

## Isolated Web App

The current path. Same patched module, same ring, same RUPP — the seam is a
`UDPSocket` instead of a WebSocket, and the join is done by a
`<controlledframe>` instead of the Rust proxy.

```
./tools/run_iwa.sh                      # install once, then open
PLACE=12345 ./tools/run_iwa.sh
```

What still matters when the module is rebuilt:

* `--abi` output (`build/WebClient.patched.ring.json`) is served as `/abi.js`.
  If the ring layout moves in `tools/patch.py`, the page picks it up with no
  edit — do not reintroduce a second copy of the offsets in JS.
* `--wasm` is served at `/engine/WebClient.wasm`; `--cdn-base` must point at the
  matching `version-<hash>-RobloxWebClient` directory, or every engine asset
  403s and the client silently runs on default flags.
* `rkhost --iwa` prepends the worker prelude to `WebClient.js` so workers load
  `rbx-net.js`. Without it the engine's worker fetches bypass the frame.
* The engine only picks RakNet if `FFlagUseRbxTransportClient3` is `"False"`.
  `iwa/rbx-net.js` rewrites that flag in the `WebClientApp` settings response.

Console access: `--remote-debugging-port=9222`, then attach to the `app` target
(`curl 127.0.0.1:9222/json`). The old `/__log` and `/__client` endpoints are
unused — the IWA CSP has no localhost in `connect-src`, so the page cannot post
to them.

Details, API shapes and traps: **[IWA.md](IWA.md)**.
