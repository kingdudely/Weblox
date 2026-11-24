# Alcohol — Roblox web client in a native window

The engine is Roblox's own web build (`WebClient.wasm` + `WebClient.js`), running
on Deno with no DOM. Deno supplies the browser-shaped page (`page/env.ts`), the
window/WebGPU surface (`main.ts`), and real threads for Emscripten pthreads
(`page/worker.ts`). There is no Rust host and no Electron in this product.

**The Rust/wasmtime harness under `../v2-wasmtime` is a secondary research
harness only. Keep it. Do not delete or rewrite it.**

## Current status (IMPORTANT — read before changing anything)

**The client joins the HTTP/gamejoin layer successfully but the game server never
opens Roblox's data channels, so the UI stays on "Joining server" forever.**
Chrome with the same engine, account, and place works. Everything observable has
been compared byte-for-byte and is identical; see `INVESTIGATION.md` in this
directory for the full evidence table and the experiments already ruled out.

Do **not** re-run the old "Origin header" / "npm WebTransport library" / "fake
`getStats()`" / "account" hypotheses — they are all ruled out. Read
`INVESTIGATION.md` first.

## Run

```sh
./run.sh 189707
```

Defaults: cookie from `/home/john/mizg.txt`, place `189707`. Override with
`WC_COOKIE_FILE`, `WC_PLACE`, or the first argument. A known-good alternative
cookie for the `jebuscristgaming` account is `/tmp/opencode/jebus.txt`.

The client restarts itself on a separate thread (`page/watchdog.ts`) when it
detects a GPU device loss, a pre-join stall (18 s), a post-join stall (45 s), or
a WebTransport handshake failure (see below). `WC_WATCHDOG_MS=0` disables the
frame/stall restarts; `WC_REJOIN=0` disables handshake-failure rejoins;
`WC_WATCHDOG_LOG=1` prints them. Only one client should be joined at a time —
overlapping sessions on the same account get disconnected.

## Build

```sh
deno check main.ts page/*.ts
deno desktop -A --include page --include mirror --include page/watchdog.ts main.ts
```

`deno desktop` bundles **and runs** the app, so stop it (Ctrl-C / kill) once
`Bundle client` appears. The binary lands at `client/client/`. Builds take
~45 s. Use `/tmp/opencode/fasttest.sh` for short A/B runs.

Large generated artifacts (`mirror/`, the v2 wasmtime cache, the v3 wasm2c
translation, captures) are not in git; see [REGENERATING.md](../REGENERATING.md)
for how to recreate each.

`mirror/` is gitignored and must stay that way — it holds cached Roblox files.
The build version and page asset hash are resolved from live endpoints, so a new
Roblox build needs `rm -rf mirror` (or `WC_REFRESH=1`). Current engine:
`version-1ef59424f70d4932-RobloxWebClient` (0.748.0.7481526) — verified
identical to the build Chrome loads.

## Architecture

- `main.ts` — bootstrap, WebGPU adapter, native window, `Host` object, watchdog.
- `page/env.ts` — browser surface: `window`, `self`, canvas, events, XHR, fetch
  shims, WebTransport wrapper, RNA event bridge, worker patching.
- `page/worker.ts` — real Emscripten worker; `eval`s `stub.js` from the mirror.
- `page/net.ts` — engine URL resolution, mirror cache, `fetch` patching.
- `page/xhr.ts` — XHR shim; `patchJoin()` adds the browser `gameFqdn`.
- `page/watchdog.ts` — separate-thread restarts.
- `page/opfs.ts`, `page/audio.ts` — OPFS shim, stub audio.
- `mirror/files/` — cached WebClient.js/wasm/asset files (gitignored).

### The join identity (do not "simplify" this)

Public places require `rbx-user-agent: Roblox/Darwin` on the gamejoin POST;
`Roblox/Web` returns HTTP 200 / `status: 12` ("Playtesters Only") for both test
accounts. The regular `user-agent` stays the browser UA, matching the Chrome
extension. `patchJoin()` derives `gameFqdn` only when the response has none
(the server always returns `null` for public places) using the extension's
formula: `tokenHex(NetStackTokenValue)-ipv4Hex(MachineAddress)-portHex.browserVipHex.qdmux.roblox.com`,
where `browserVip = UdmuxEndpoints[0].Address` with the last octet **+1**.

## Environment flags

Join / identity:
- `WC_COOKIE_FILE`, `WC_PLACE`
- `WC_JOIN_UA` — `rbx-user-agent` for gamejoin (default `Roblox/Darwin`)
- `WC_RBX_UA` — `rbx-user-agent` for non-join Roblox requests
- `WC_JOIN_HTTP_UA` — override the regular `user-agent` on the join only
- `WC_JOIN_NO_ORIGIN=1` — drop `origin`/`referer` from the join (test)
- `WC_JOB=<jobUuid>` — pin to one server instance by rewriting the join to
  `/v1/join-game-instance` with that `gameId` (used for the same-job control
  test in `INVESTIGATION.md`)
- `WC_JOIN_DEBUG=1` — log join keys/values and dump joinScript to
  `/tmp/opencode/our_joinscript.json`
- `WC_NO_FQDN=1` — do not patch `gameFqdn`
- `WC_FORCE_DISMISS=0` — keep the synthetic loading screen dismissal off

Networking:
- `WC_ENGINE`, `WC_PAGE_ASSETS`, `WC_MIRROR`, `WC_REFRESH=1`, `WC_GLES=1`
- `WC_WT_LIB=1` — npm `@webtransport-bun` (ruled out)
- `WC_WT_FC=1` — npm `@fails-components` (ruled out)
- `WC_WT_RW=1` — npm `rwebtransport` (Origin-capable; ruled out)
- `WC_WT_PASSTHROUGH=1` — zero wrapper overrides (ruled out)
- `WC_WT_ORIGIN_HDR`, `WC_WT_UA_HDR`, `WC_WT_CHROME_HDRS=1` — CONNECT headers
- `WC_REAL_STATS=1` — use Deno's real `getStats()` (ruled out)

Diagnostics (gated, safe to leave in):
- `WC_RNA_LOG=1` — log every wasm RNA event (`[rna]`)
- `WC_RNA_DUMP=1` — dump incoming datagram bytes (`[rxbytes]`)
- `WC_WT_DUMP=1` — dump all WebTransport stream writes (`[wtdump]`) and
  datagram writes (`[wtdg]`)
- `WC_DUMP=1`, `WC_DUMP_FILE`, `WC_DUMP_MS` — self-capture of the rendered canvas
- `WC_NET_DEBUG=1`, `WC_SIZES=1`, `WC_INPUT=1`
- `WC_WT_HANDSHAKE_MS` — rejoin timeout when no server stream appears
  (0 disables); `WC_REJOIN_MAX` (default 12)

## Known-good comparison data on disk

- `/home/john/Alcohol/play.roblox.com.har` — Chrome HAR with the successful
  gamejoin request/response (912 entries).
- `/tmp/opencode/chrome_netlog2.json` — Chrome NetLog (valid JSON, 290676
  events) with full QUIC/HTTP3 frames of a successful session.
- `/tmp/opencode/chrome_udp.pcap`, `chrome_warp.pcap`, `deno_warp.pcap` — UDP
  captures (pcapng; parse with `/tmp/opencode/pcapng.py`). Traffic runs over the
  `CloudflareWARP` interface (172.16.0.2), so capture there, not `wlp2s0`.
- `/tmp/opencode/cdp_*.ts` — CDP scripts used to attach to Chrome on port 9337.
- `/tmp/opencode/purewt.ts` — standalone WebTransport probe (join + connect).

## Do not do these again

- Rebuilding Deno's Rust source (user rejected: "editing deno is not good
  practice", "its building forever"). The local source copy at
  `/tmp/opencode/deno-src` has a `WC_WT_ORIGIN` patch but it was never compiled.
- Manually implementing the Roblox protocol in JS like Boblox does. The whole
  point of Alcohol is that the wasm does the protocol.
- Replacing the wasm's engine files with older ones.

See `INVESTIGATION.md` for the full evidence and remaining hypotheses.
