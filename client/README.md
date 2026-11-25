# Alcohol — Roblox web client on raw Deno

The engine is Roblox's own web build (`WebClient.wasm` + its glue scripts),
running on Deno with no DOM and no Electron. `main.ts` gives it a native window
and WebGPU surface; `page/` supplies the browser-shaped shims and real threads.
The engine generates the whole protocol — nothing here reimplements it.

## Run

```sh
WC_COOKIE_FILE=/tmp/opencode/jebus.txt ./client/client 189707
```

Both defaults (cookie `/home/john/mizg.txt`, place `189707`) live in `main.ts`;
no wrapper script and no environment flags are needed for a normal run. Only
one client should be joined at a time.

**Working join looks like:** `[draft02] CONNECT {":status":"200"}`,
`INCOMING STREAM ACCEPTED`, `Connection accepted`,
`Time taken to initialize schema`, `Assigned peer id`.

If Roblox returns an empty `NetStackTokenValue`, the same gamejoin POST is
retried in place (30 times, 250 ms apart). The window is never closed or
reopened; restarts are always manual.

## Build

```sh
deno check main.ts page/*.ts
deno desktop -A --unstable-net --include page --include mirror main.ts
```

`deno desktop` bundles **and runs** the app — stop it (Ctrl-C) once
`Bundle client` appears. The binary lands at `client/client`. The
`--unstable-net` flag bakes `Deno.connectQuic` into the binary; without it the
draft-02 transport cannot start.

`mirror/` is gitignored and holds the cached engine files. A new Roblox build
needs `WC_REFRESH=1` (or `rm -rf mirror`). Current engine:
`version-1ef59424f70d4932-RobloxWebClient`. See
[REGENERATING.md](../REGENERATING.md).

## Files

| file | job |
| --- | --- |
| `main.ts` | window, WebGPU surface, input, defaults, bootstrap |
| `page/env.ts` | browser surface (window/document/canvas/events/XHR/workers) |
| `page/net.ts` | engine/page resolution and mirrored `fetch` |
| `page/xhr.ts` | join POST, `gameFqdn` derivation, empty-token retry |
| `page/draftwt.ts` | WebTransport draft-02 over `Deno.connectQuic` |
| `page/worker.ts` | Emscripten pthread worker |
| `page/gl.ts`, `page/opfs.ts` | WebGL/2D and OPFS shims |

## The join identity (do not "simplify" this)

`rbx-user-agent: Roblox/Darwin` on the gamejoin POST; the regular `user-agent`
stays the Chrome UA. `patchJoin()` derives `gameFqdn` only when the server
returns none: `tokenHex(NetStackTokenValue)-ipv4Hex(MachineAddress)-portHex.browserVipHex.qdmux.roblox.com`,
where `browserVip = UdmuxEndpoints[0].Address` with the last octet **+1**.

## Optional overrides

`WC_COOKIE_FILE`, `WC_PLACE`, `WC_REFRESH`, `WC_ENGINE`, `WC_PAGE_ASSETS`,
`WC_MIRROR`, `WC_BUILD_VERSION` (engine/page pins), `WC_INPUT=1` (input trace),
`WC_DUMP=1`/`WC_DUMP_FILE`/`WC_DUMP_MS` (self-capture).

Historical evidence for the ruled-out hypotheses is in
[INVESTIGATION.md](INVESTIGATION.md).
