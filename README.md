# Weblox

Run the **official Roblox web client** (`WebClient.wasm`) on a real UDP socket so
it joins over **RakNet** — the transport that works on every place, instead of
WebTransport, which the server only enables for playtest-enrolled universes.

No Luau reimplementation. No join ladder. No 0x9B solver. The engine does all of
that with its own code; this repository supplies the two things a browser does
not give it: a socket, and a way to be logged in.

It runs as a Chrome **Isolated Web App**: pure JS plus the patched module, no
helper process, no extension, no UDP relay. Installed, it is an app window called
**Roblox**, with Roblox's icon, and the canvas is the whole of it.

---

## Status (2026-11-02)

```
place 13569002876        Chrome 156 / Linux
tx 4073 datagrams        rx 3768 datagrams        1.08 MB replicated
canvas 1366x593          avatar, decals, textures, CoreGui, tool hotbar
```

| Piece | State | Where |
| --- | --- | --- |
| Transport selection + `RakPeer::Connect` | **works — the engine does it itself** | — |
| Datagram ring (tx/rx) | **works** | `tools/patch.py`, `iwa/ring.js` |
| RUPP TLV1/TLV2 stamping | **works** | `iwa/rupp.js` |
| UDP socket | **works** — Direct Sockets | `iwa/ring.js` |
| Authenticated join | **works** — controlled frame | `iwa/auth.js` |
| roblox.com API + assets | **works** | `iwa/rbx-net.js` |
| Fullscreen, pointer | **works** | manifest permissions policy |
| Audio | **works after the first click** — FMOD needs `mixerSuspend`+`mixerResume` in the gesture | `iwa/boot.js`, `docs/IWA.md` |
| Packaging as a signed bundle | **not done** | — |

## Shape

```
isolated-app://<bundle>
  index.html  boot.js  abi.js  stub.js
  auth.js     controlled frame: login, gamejoin, join script
  rbx-net.js  roblox.com -> frame; binaries -> direct
  rupp.js     TLV1/TLV2
  ring.js     rings <-> one UDPSocket
  app.js      boot order
  engine/     staged: patched wasm, glue, content tree
```

Three browser rules forced this shape:

| Rule | Answer |
| --- | --- |
| `SharedArrayBuffer` needs cross-origin isolation | an IWA is cross-origin isolated by definition |
| a page cannot open a UDP socket | `UDPSocket` (Direct Sockets), IWA-only |
| `gamejoin` is origin-locked and the cookie is `HttpOnly` | `<controlledframe>` is a real roblox.com context: the browser attaches the cookie, and CORS is Roblox's own policy |

The fourth problem — the session the server hands back depends on the *client
identity* that asked for it — is solved by rewriting `User-Agent` to
`Roblox/WinInet` on `gamejoin` only, through the frame's request interceptor.

## Quickstart

```bash
# one time: mirror the engine content the client loads (~48 MB)
./tools/mirror_engine.py

# one time: patch the module
./tools/build_patched.sh /path/to/WebClient.wasm build/WebClient.patched.wasm

# every run: stage, serve, install on first use, open
./tools/run_iwa.sh
PLACE=12345 ./tools/run_iwa.sh
```

`run_iwa.sh` installs the app into `~/.weblox-iwa` on first use, and reinstalls
when the manifest changes. Source changes need nothing: dev mode proxies every
request to `rkhost`.

## Layout

| Path | What |
| --- | --- |
| `iwa/` | the app: everything that runs in the browser |
| `tools/patch.py` | rewrites `WebClient.wasm`: socket syscalls into rings, transport selection |
| `tools/build_patched.sh` | the only entry point that writes a module |
| `tools/mirror_engine.py` | fetches the engine content tree into `iwa/engine/` |
| `tools/stage_engine.sh` | puts wasm + glue + prelude + ring ABI into `iwa/` |
| `app/rkhost/` | ~180-line static server with the IWA security policy |
| `docs/IWA.md` | the architecture, the API shapes, the traps |
| `docs/PATCHING.md` | how the module is patched, and how to re-find it after a rebuild |

## Known constraints

* **Engine builds move weekly.** Every rebuild needs `build_patched.sh`, which
  emits the ring ABI the page reads. `docs/PATCHING.md` covers re-resolving the
  patch sites.
* **Cloudflare WARP is required** for Roblox reachability from this network.
* **The content mirror is observed, not enumerated.** rbxcdn denies directory
  listing and the web client ships no archive, so `tools/engine.paths` is the
  set of files the client was seen asking for. A miss is logged loudly by
  rkhost as `MISSING engine/...`; re-run `mirror_engine.py --capture` against
  that log.
* **No signed bundle yet.** Dev-mode install is per-machine and flag-gated.

## Explicitly out of scope

* Reimplementing the engine's game logic in Luau.
* A join ladder, a ticket solver, or any per-place special casing.
* Supporting non-Chromium browsers — Direct Sockets and Controlled Frame are
  Chromium-only, and both are required.
