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

## Status (2026-11-03)

```
place 1818 (Crossroads)  Chrome 156 / Linux
tx 7046 datagrams        rx 8708 datagrams        5.2 MB replicated
canvas 1026x639          avatar, decals, textures, CoreGui, tool hotbar
audio                    free-running sink, contexts running
```

The window is the canvas and nothing else. Progress goes to the console; anything
that stops the session raises one `window.alert` rather than drawing over the
game.

| Piece | State | Where |
| --- | --- | --- |
| Transport selection + `RakPeer::Connect` | **works — the engine does it itself** | — |
| Datagram ring (tx/rx) | **works** | `tools/patch.py`, `iwa/ring.js` |
| RUPP TLV1/TLV2 stamping | **works** | `iwa/rupp.js` |
| UDP socket | **works** — Direct Sockets | `iwa/ring.js` |
| Authenticated join | **works** — controlled frame | `iwa/auth.js` |
| roblox.com API + assets | **works** | `iwa/rbx-net.js` |
| Fullscreen, pointer | **works** | manifest permissions policy |
| Audio | **works** — context unlocked on first input, engine's suspend refused, ScriptProcessor sink | `iwa/boot.js`, `docs/IWA.md` |
| Packaging as a signed bundle | **works** — install through `chrome://iwa-dev/` | `tools/build_bundle.sh` |

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

# build + sign the installable app (first run generates the key)
./tools/build_bundle.sh
# chrome://iwa-dev/ -> Install IWA from Signed Web Bundle -> build/Weblox.swbn

# optional dev loop for engine work
./tools/run_iwa.sh
./tools/run_iwa.sh --stop
```

The signed bundle is self-contained; no server runs with it. `run_iwa.sh` +
rkhost remain only as the dev-mode loop: they serve `iwa/`, install a dev app in
`~/.weblox-iwa`, and leave the server detached. They do not register an OS
handler.

Once installed, roblox.com's **Play** button opens the app directly. Chrome owns
the protocol registration; with the signed app in the same Chrome profile that
presses Play, no OS handoff or external-protocol dialog is involved. The link's
one-time `gameinfo` ticket signs the client in, and `app.js` restores
`?placeId=` before loading the engine (the loader's startup gate needs it). A
launch without a ticket cannot start a session.

## Layout

| Path | What |
| --- | --- |
| `iwa/` | the app: everything that runs in the browser |
| `tools/patch.py` | rewrites `WebClient.wasm`: socket syscalls into rings, transport selection |
| `tools/build_patched.sh` | the only entry point that writes a module |
| `tools/mirror_engine.py` | fetches the engine content tree into `iwa/engine/` |
| `tools/stage_engine.sh` | puts wasm + glue + prelude + ring ABI into `iwa/` |
| `tools/build_bundle.sh` | bundles `iwa/` and signs `build/Weblox.swbn` |
| `app/rkhost/` | dev-only static server with the IWA security policy |
| `docs/IWA.md` | the architecture, the API shapes, the traps — start here |
| `docs/PATCHING.md` | how the module is patched, and how to re-find it after a rebuild |
| `docs/FINDINGS.md` | protocol and behaviour notes gathered before the IWA existed |

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
* **One app window at a time.** The engine reserves 2 GiB of shared memory before
  any of its own logic runs, so a second window fails to allocate. The page
  alerts instead of dying silently.
* **Signed bundle.** `build/Weblox.swbn` is about 143 MB because it carries the
  engine tree; rebuild and re-sign after every engine build. The key at
  `~/.weblox-iwa-key.pem` fixes the app origin — losing it means a new app, not
  an update.

## Explicitly out of scope

* Reimplementing the engine's game logic in Luau.
* A join ladder, a ticket solver, or any per-place special casing.
* Supporting non-Chromium browsers — Direct Sockets and Controlled Frame are
  Chromium-only, and both are required.

---

## Native hosts (imported from Alcohol)

Weblox's original IWA path lives in `iwa/`, `tools/` and `app/`. The following
were merged in from the former `Alcohol` repo and are **experimental**:

| path | what it is |
| --- | --- |
| `client/` | Deno raw-backend host: WebGPU + draft-02 WebTransport over `Deno.connectQuic` |
| `v2-wasmtime/` | Rust/wasmtime host, including a native-UDP RakNet ring |
| `v3-wasm2c/`, `vendor/` | parked wasm2c / wasmtime source patches |
| `docs/ARCHIVE.md` | research record: join identity, RakNet gate, patch sites, ruled-out runtimes |
| `REGENERATING.md` | how to recreate gitignored engine mirrors and caches |

### Deno host

```sh
cd client
WC_COOKIE_FILE=/path/to/cookie.txt deno task dev
```

Built launcher: `deno desktop -A --unstable-net --include page --include mirror main.ts`
(stop at `Bundle client`). Place 189707 with a valid cookie is the known-good test.

### RakNet on wasmtime (experimental)

The current engine gates RakNet off; a small wasm patch plus a native UDP seam
is required. `v2-wasmtime/tools/patch_new.py` applies the socket patches and
`v2-wasmtime/src/raknet.rs` implements the host side. See `docs/ARCHIVE.md`.
The wasmtime host has not yet reached a join — treat it as work in progress.
