# RakNet-Wasm-Client

Run the **official Roblox web client** (`WebClient.wasm`) on top of a real UDP
socket, so it joins over **RakNet** — the transport that works on every place —
instead of WebTransport, which the server only enables for playtest-enrolled
universes.

No Luau reimplementation. No join ladder. No 0x9B solver. The engine does all of
it with its own code; we only supply datagrams.

---

## Status (2026-11-02)

The engine joins over **real RakNet UDP** from a self-hosted desktop shell. No
extension, no Chrome flag, no node/python bridge: `app/rkhost` serves the page,
proxies the Roblox APIs, owns the UDP socket, and the Tauri window at
`app/src-tauri` is just the shell around it.

Measured live against place `13569002876`:

```
udp rx 32 B  <- 128.116.32.33:57651    Reply1 (7e + magic + guid8 + mtu 1200)
udp rx 158 B <- 128.116.32.33:57651    Reply2 (7d, RUPP 23, TLV1 subtype 2)
… 3410 datagrams / 1.07 MB down, 3166 up, 0 engine failures over 90 s
```

| Step | State | Where |
| --- | --- | --- |
| Engine boots in WebKitGTK **and** Chrome | **works** | `app/src-tauri`, `app/rkhost` |
| `SharedArrayBuffer` under WebKit | **works** — needs `JSC_useSharedArrayBuffer=1` + COEP `require-corp` | `app/src-tauri/src/main.rs` |
| Web Audio init | **works** — `maxChannelCount` clamp | `host/ring.js` |
| RakNet transport selection + `RakPeer::Connect` | **works — the engine does it itself** | — |
| Datagram ring (tx/rx) | **works** | `tools/patch.py`, `host/ring.js` |
| RUPP token / TLV2 | **works** — rewritten in the page on the way out | `host/ring.js` |
| Join over RakNet | **works** — 1 MB+ replicated in the first 90 s | — |
| Tauri shell | **builds and runs** | `app/src-tauri` |

Full evidence, verbatim logs and retractions: **[docs/FINDINGS.md](docs/FINDINGS.md)**.
Operational manual for patches: **[docs/PATCHING.md](docs/PATCHING.md)**.

## Why this works at all

The wasm imports **no socket functions** (268 imports; no `socket`, `sendto`,
`recvfrom`, `poll`, `getaddrinfo`, no `__syscall_*` beyond uid/gid). RakNet's
client code is compiled in and runs — it just cannot open a file descriptor.
Because imports are fixed at compile time, the fix is to replace the libc bodies
that RakNet calls with stubs that move bytes through ring buffers in linear
memory.

```
  page (Chrome, cross-origin isolated)
  ┌──────────────────────────────────────────────────────────┐
  │ WebClient.wasm  ── patched libc ──►  tx ring (heap)      │
  │      ▲                                   │               │
  │      └────── rx ring (heap) ◄────────────┘               │
  │ bridge.js: polls rings, WebSocket to localhost           │
  └──────────────────────────────────────────────────────────┘
             │
             ▼
  local helper (Node dgram, or C++)
  ├── real UDP socket                (datagrams verbatim)
  └── gamejoin/asset HTTP proxy      (same-origin, cookie injected)
```

The page cannot do UDP — no browser can. The helper is deliberately dumb: it
forwards datagrams and proxies HTTP. Everything stateful (join crypto, tickets,
challenge, replication) stays inside the wasm.

## Quickstart

```sh
# 1. fetch the engine once (version string is in the page's network log)
curl -A 'Mozilla/5.0 … Chrome/155.0.0.0 Safari/537.36' \
  -o WebClient.wasm \
  'https://setup.rbxcdn.com/webclient/version-<hash>-RobloxWebClient/WebClient.wasm'

# 2. build the toolchain (wabt from source — distro 1.0.36 cannot decode exnref)
tools/disasm.sh WebClient.wasm .cache     # -> .cache/disasm.txt (~2.8 GB)

# 3. patch and validate
python3 tools/patch.py WebClient.wasm WebClient.patched.wasm \
        --patch getaddrinfo --patch nosocket --patch nofd
.cache/wabt-src/build/wasm-validate --enable-all WebClient.patched.wasm

# 4. serve the patched module
python3 tools/serve_wasm.py --file WebClient.patched.wasm --port 8765 &

# 5. drive a live session against it
node --experimental-websocket tools/probe/runprobe.js \
  --place 13569002876 --cookie /path/to/cookie.txt \
  --wasm-url http://127.0.0.1:8765/WebClient.wasm \
  --flags FFlagUseRbxTransportClient3=False \
  --seconds 70 --out /tmp/run.jsonl
```

`runprobe.js` redirects the page's `WebClient.wasm` request to your local copy
via CDP `Fetch.continueRequest`, so no hosting or DNS games are needed.

## Layout

```
tools/
  patch.py        wasm body rewriter + patch registry (the only thing that edits bytes)
  wasm_scan.py    section/function parser + i32.const address scanner
  disasm.sh       build current wabt, cache a full disassembly
  serve_wasm.py   COOP/COEP/CORS server for a patched module
  probe/          CDP harness: cookie, flags, hooks, wasm redirect
docs/
  FINDINGS.md     every experiment, verbatim logs, retractions
  PATCHING.md     how to add/re-resolve patches, current indices, traps
```

## Known constraints

- Engine needs `COOP: same-origin` + `COEP: require-corp` (shared memory,
  pthreads).
- Engine assets come from `setup.rbxcdn.com`; `WebClient.metadata.json`
  (222 KB of paths) is the manifest.
- `gamejoin` CORS is origin-locked to Roblox origins, so the join must go
  through the local proxy, not the page.
- The engine rotates weekly. **Function indices are per-build** — see
  `docs/PATCHING.md` for the re-resolve procedure. Never hardcode a file offset.

## Next step (pick up here)

The fd fake is a dead end — musl's fd table, `errno`, `fd_set` and `sockaddr`
semantics all have to agree, and they are now three patches deep with a fourth
(`getsockname`) already failing. **Prefer replacing RakNet's own socket
abstraction** (or the engine's socket setup function that logs
`binding socket on inaddr_any:0`), which needs no errno and no fd table:

1. Locate the engine's socket wrapper. It is the function that emits
   `binding socket on inaddr_any:0`, `setsockopt SO_* failed`, and
   `RakNet Socket bound to invalid (privileged) port`. String xrefs do not work
   (see PATCHING) — use call-graph narrowing from `getsockname`/`bind` leaves,
   or patch-a-candidate-and-diff-the-log.
2. Replace its send/recv/bind bodies with ring-buffer stubs (tx push, rx pop,
   `EAGAIN`-equivalent when empty).
3. Only then build the host page + bridge. Until data actually moves, the bridge
   has nothing to carry.

Keep the three existing patches — they are harmless and they get the engine to
the socket setup, which is where the real work starts.

## Explicitly out of scope

- Reimplementing Roblox's Luau, replication, physics, or anti-cheat.
- WebTransport for non-enrolled universes (server-side gate; unfixable
  client-side — measured, see FINDINGS §1).
