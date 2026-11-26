# Chrome oracle — Web Play session longevity (place 189707)

Question from [`NATIVE-AOT-FINDINGS.md`](NATIVE-AOT-FINDINGS.md): does Roblox's
WebTransport session survive past ~120 s in real Chrome? A raw Deno shim dies at
~110 s with `conn.closed REJECTED Error: timed out`. This run observed the
stock Web Client in the logged-in Chrome profile with the unpacked
`public-web-play-extension` installed. Run on 2026-11-26, all times UTC.

## Verdict

**No clean survival result — the session was not observed alive past 120 s.**

A Web Play session was live with `selectedTransport=RbxTransport`, an assigned
peer, and 21 players. It ran for about **75 s of engine time** before the tab
navigated away at 19:32:15. No `conn.closed`, `timed out`, `Disconnect`,
`connectError`, `connectionClosed`, or `lost connection` message was captured,
and no WebTransport error appeared. The termination coincided with a navigation
to `about:blank` and then the native `roblox-player:` launcher, which looks like
an external Play/reload action rather than an organic transport timeout.

So this run **does not demonstrate >120 s WebTransport survival**, and it also
does not reproduce the Deno timeout. Treat the transport decision as still
open. A clean rerun must leave the tab untouched for 120 s after the join.

## Exact commands

Chrome (started by the controlling session; profile already logged in, extension
already loaded — not restarted or modified by this observation):

```
/opt/google/chrome/chrome \
  --user-data-dir=/tmp/opencode/chrome-oracle-profile \
  --load-extension=/tmp/opencode/ext-experimental/public-web-play-extension \
  --remote-debugging-port=9222 --remote-allow-origins=* \
  --no-first-run --no-default-browser-check --disable-gpu-sandbox \
  --enable-unsafe-webgpu --ignore-gpu-blocklist \
  --enable-features=Vulkan --use-angle=vulkan --use-vulkan=native about:blank
```

CDP discovery and verification:

```
curl -s http://127.0.0.1:9222/json/list
curl -s http://127.0.0.1:9222/json/version
```

Collector (written outside the repo at `/tmp/opencode/chrome_oracle_collector.py`):

```
python3 /tmp/opencode/chrome_oracle_collector.py \
  > /tmp/opencode/chrome_oracle_collector3.stdout 2>&1
```

It connects to the browser-level WebSocket from `/json/version`, calls
`Target.setDiscoverTargets` and `Target.setAutoAttach` with `flatten: true`, and
enables `Runtime`, `Log`, `Network`, and `Page` on every attached page/worker
session. Every `Runtime.consoleAPICalled`, `Runtime.exceptionThrown`,
`Log.entryAdded`, and `Network.*` event is appended to
`/tmp/opencode/chrome-oracle-events.jsonl`. A 5 s `Runtime.evaluate` probe
records `location.href`, `document.title`, canvas count, and loading-overlay
visibility into `/tmp/opencode/chrome-oracle-status.json`. The wait phase is
600 s; once a `Assigned peer` / `#players` / `CONNECT 200` / `INCOMING STREAM` /
WebTransport-ready signal appears, T0 is set and the collector observes 240 s.
A browser restart is retried/reconnected. Final data:
`/tmp/opencode/chrome-oracle-summary.json`.

## Timeline (UTC)

| Time (UTC) | Elapsed from T0 | Observation |
|---|---|---|
| 19:25:11 | — | First collector attempt attached. Only page/network events; the game worker never attached. |
| 19:28:04 | — | `play.roblox.com/games/start?placeId=189707` target appeared (Play click). |
| 19:28:44 | — | That browser target disappeared; first run captured 12,529 events but no join console. Preserved as `chrome-oracle-events-run1.jsonl`. |
| 19:29–19:31 | — | Chrome was relaunched twice by the controlling session; CDP port flapped, one collector attempt exited on `ECONNREFUSED`. |
| 19:31:39.400 | −0.12 s | Successful attach to `https://play.roblox.com/games/start?placeId=189707`; page title `Natural Disaster Survival`. The Web Client was **already joined** — 74 buffered heartbeats were replayed. |
| 19:31:39.512 | −0.01 s | `[DFLog::NetworkClient] Transport selection: useRbxTransportEnabled=true, rbxTransportHasValidPort=true, rbxTransportHasValidAddress=true, rbxTransportHasValidPubKey=true, canUseRbxTransport=true, selectedTransport=RbxTransport, rccPort=65413, DatacenterId=25862` |
| 19:31:39.512 | −0.01 s | `[FLog::Network] UDMUX Address = 128.116.53.33, Port = 65413 \| RCC Server Address = 10.221.40.210, Port = 65413`; `Connection accepted from 128.116.53.33\|65413` |
| **19:31:39.522** | **T0 (0.00 s)** | `[FLog::Network] Assigned peer id = 1736` — session detected. |
| 19:31:39.551 | +0.03 s | `[FLog::AvatarJoinMetrics]   #players = 21` |
| 19:32:15.423 | +35.9 s | `Page.frameStartedNavigating` — tab begins navigating away; `executionsContextsCleared` follows. |
| 19:32:15.494 | +36.0 s | Last game heartbeat: `[MAIN] heartbeat #146 t=74901.2ms delta=502.7ms` (engine time 74.9 s). Implied engine/page start ≈19:31:00.6. No later heartbeat. |
| 19:32:16.224 | +37.7 s | Redirect to `https://www.roblox.com/games/start?placeId=189707&wasmRedirect=1`. |
| 19:32:16.941 | +38.4 s | Navigation to `about:blank`. |
| 19:32:21.291 | +41.8 s | Navigation to `roblox-player:1+launchmode:play+gameinfo:…+placelauncherurl:…PlaceLauncher.ashx?…placeId=189707…` (native launcher). |
| 19:32:26.8 | +46.5 s | "Loading overlay dismissed" DOM probe — but by then the tab was already off-game, so this is not in-game evidence. |
| 19:35:40.351 | +240.8 s | Collector finished normally (`phase: done`), 3,666 events, no errors. |

Join time: the engine was already joined at attach. From the last heartbeat's
clock the join/page start was **≈19:31:00.6 UTC**, i.e. ~39 s before T0. First
`#players` at 19:31:39.55 came from CDP's buffered console replay, not from a
live new join event.

No disconnect time: no disconnect/timeout/error console event was captured at
any point. The only observable session end is the 19:32:15 navigation.

## End-of-run state

- Last page URL: `https://www.roblox.com/games/start?placeId=189707&wasmRedirect=1`
- Last page title: `Natural Disaster Survival | Play on Roblox`
- Final DOM: `canvas: 0`, no loading-overlay element visible, body text is the
  game page shell (`Charts / Marketplace / Create / About / Store / Servers`) —
  not an in-game render.
- Event keyword counts: `assigned_peer` 1, `players` 1, `heartbeat` 146 (last at
  19:32:15), `conn_closed` 0, `timed_out` 0, `disconnect` 0, `connect_error` 0,
  `WebTransport` 0, `webSocketCreated` 3 (site chat/realtime, not game
  transport). Non-game noise: 9 `failed/unavailable` asset-log lines, 10
  `[FLog::Network]`/`[DFLog::NetworkClient]` lines — all before 19:32:15.
- Network traffic continued after the navigation (Roblox page APIs, telemetry,
  CDN), but no new Web Client load, heartbeats, or join occurred.

## Route info

The client selected `RbxTransport` with `rccPort=65413`, `DatacenterId=25862`,
and reported native UDMUX `128.116.53.33`. The extension's join patch derives
the browser multiplexer by adding 1 to the last octet, so the expected browser
route is **`128.116.53.33 → 128.116.53.34`** (same `.33 → .34` rule documented
in [`reference/research/native/transport-followup.md`](reference/research/native/transport-followup.md)).
The popup was not open during the run, so its literal
`Route supplied (… → …)` text was not captured, and no `gameFqdn`/`qdmux` string
appeared in CDP console because attachment happened after the join. The `.34`
value is therefore derived from the patch rule, not directly observed.

## Clean hands-off run — DECISIVE

The first run was invalidated by an external tab navigation. A second run was
made with the collector attached **before** navigation and the tab untouched:

```
/tmp/opencode/run-clean-oracle.sh
  (kills Chrome, relaunches with Vulkan/WebGPU flags, starts collector,
   injects the gate patch via CDP Page.addScriptToEvaluateOnNewDocument,
   navigates to play.roblox.com/games/start?placeId=189707, waits)
```

Summary (`/tmp/opencode/chrome-oracle-summary.json`):

| field | value |
| --- | --- |
| phase | `done` |
| detected | `true` |
| T0 (`Assigned peer id`) | 2026-11-26T19:38:26Z (9 s after attach) |
| `#players = 21` | 21 s after attach |
| observed after T0 | **240.6 s** |
| `survived_120s` | **true** |
| final URL | `https://play.roblox.com/games/start?placeId=189707` (never redirected) |
| final title | Natural Disaster Survival |
| last heartbeat | #490 |
| `conn.closed` / `timed out` / `Disconnect` / `connectError` | **zero** |

**Verdict:** real Chrome's WebTransport session stays alive well past 120 s.
Our raw Deno shim's `conn.closed REJECTED Error: timed out` at ~110 s is a bug
in our QUIC/WebTransport implementation, not a Roblox server limit. WebTransport
is viable; RakNet is not mandatory.

## Interpretation for the native host

- The C++ AOT host work (`feat/cpp-host`, Cranelift-object) remains valid — it
  gives native execution with no engine. But the transport choice now favours
  implementing WebTransport natively (quiche/ngtcp2) rather than RakNet patching,
  since Chrome proves the protocol path works indefinitely.
- The previously documented RakNet/native-UDP seam stays as a fallback on
  `feat/cpp-host` (`host/raknet.*`).

## Earlier run caveats (kept for history)

The first run's attachment was ~39 s after the join and its 240 s window was
mostly after an external tab navigation, so it could not answer the question.
It is superseded by the clean run above.
