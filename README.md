# RakNet-Wasm-Client

Run the **official Roblox web client** (`WebClient.wasm`) on top of a real UDP
socket, so it joins over **RakNet** — the transport that works on every place —
instead of WebTransport, which the server only enables for playtest-enrolled
universes.

No Luau reimplementation. No join ladder. No 0x9B solver. The engine does all of
it with its own code; we only supply datagrams.

## Status

| Step | State |
| --- | --- |
| Engine boots, joins an enrolled place over WebTransport | works (upstream, unmodified) |
| RakNet transport selection inside the wasm | **works** — `selectedTransport=RakNet` |
| RakNet reaching the network | blocked at `getaddrinfo` → `EAI_FAIL` |
| Socket patch → ring buffers | not started |

## Why it works this way

The wasm build imports **no socket functions at all** (268 imports; no `socket`,
`sendto`, `recvfrom`, `poll`, `getaddrinfo`, no `__syscall_*` beyond uid/gid).
Every RakNet call therefore lands on Emscripten's libc, which has no backend.
The engine's RakNet client code is compiled in and runs — it just cannot open a
file descriptor.

So the fix is not "add networking to the browser". It is: **replace the libc
socket entry points with stubs that move bytes through ring buffers in linear
memory**, chosen because we cannot add imports to a compiled module.

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

The page cannot do UDP; a browser never can. The helper is dumb by design: it
forwards datagrams and proxies HTTP. Everything stateful — join crypto, tickets,
challenge, replication — stays inside the wasm.

## Layout

```
tools/
  probe/        CDP harness: launches Chrome, injects cookie, taps the engine
  wasm_scan.py  wasm section/function parser + i32.const xref scanner
  anchors.md    string anchors → function resolution
docs/
  FINDINGS.md   every experiment, verbatim logs, retractions
```

## Known constraints

- Engine needs `COOP: same-origin` + `COEP: require-corp` (shared memory).
- Engine assets come from `setup.rbxcdn.com`; the manifest is
  `WebClient.metadata.json` (222 KB of paths) and must be mirrored or proxied.
- `gamejoin` CORS is origin-locked to Roblox, so the join must go through the
  local proxy, not from the page directly.
- The wasm rotates weekly; the resolver (`wasm_scan.py`) must be able to find
  every patch target by string anchor, never by fixed offset.

## Explicitly out of scope

- Reimplementing Roblox's Luau, replication, physics, or ant-cheat.
- Using WebTransport for non-enrolled universes (server-side gate, unfixable
  client-side).
