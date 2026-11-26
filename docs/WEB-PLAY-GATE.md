# Web Play gate: why the page must stay on `play.roblox.com`

Research note, 2026-11-26. This explains the redirect we hit in the Chrome/CEF
oracle and the fix applied to the local copy of the H644b extension.

## Symptom

The extension's globe button correctly navigates to
`https://play.roblox.com/games/start?placeId=…&returnUrl=…`, but the page then
redirects to:

```
https://www.roblox.com/games/start?placeId=189707&returnUrl=…&wasmRedirect=1
```

No Web Transport session ever starts; no `gamejoin` POST is observed.

## Cause

Web Play is a creators-only beta. The start page checks playtest permission
before loading the client. Found in the live Next.js chunk
`play.roblox.com/…/_next/static/chunks/774-3688b4b1497af314.js`:

```js
function u(e) {
  let n = new URLSearchParams(e);
  n.delete(s.uW);
  n.set("wasmRedirect", "1");
  return "/games/start?".concat(n.toString());
}

async function g(placeId) {
  const r = await fetch(
    "https://apis.roblox.com/guac-v2/v1/playtest-access/" + placeId,
    { credentials: "include" }
  );
  if (!r.ok) return false;
  const { hasAccess } = await r.json();
  return hasAccess === true;
}

// start-page bootstrap:
const auth = await m();                       // users/v1/users/authenticated
if (auth.status === "signedOut") { D(..., "/login?returnUrl=" + u(q)); return; }
if (auth.status === "unknown") { D(..., u(q)); return; }
const joinErr = await d(n, auth.userId);       // wasmJoinId validation
if (joinErr !== undefined && !(await g(placeId))) { D(joinErr, u(q)); return; }
// otherwise: load WasmPlayer and start the Web Client
```

For a user without playtest access and without a valid `wasmJoinId`, `g()` is
false, so the page redirects via `u()` — a relative `/games/start?...` URL with
`wasmRedirect=1`. Relative URL would keep the current host, but in practice the
page ends up on `www.roblox.com`, where the extension's `page-hook.js` does **not**
run (it matches only `https://play.roblox.com/games/start*`). The join patch
therefore never executes.

## Requirement

**The page must stay at `play.roblox.com/games/start`.** That is the only origin
where our page-level join patch runs. Letting it move to `www` defeats the
patch entirely. Do not "fix" this by broadening the extension match to `www`.

## Fix applied

In the local extension copy (`/tmp/opencode/ext-experimental/public-web-play-extension/page-hook.js`),
added a page-main-world `fetch` wrapper at `document_start` that answers only the
playtest-access gate locally:

```js
const nativeFetch = window.fetch.bind(window);
window.fetch = function (input, ...rest) {
  const url = typeof input === "string" ? input : input && input.url;
  if (url && /^https:\/\/apis\.roblox\.com\/guac-v2\/v1\/playtest-access\/\d+/.test(url)) {
    return Promise.resolve(new Response(JSON.stringify({ hasAccess: true }), {
      status: 200,
      headers: { "content-type": "application/json" },
    }));
  }
  return nativeFetch(input, ...rest);
};
```

All other requests pass through unchanged. The extension must be reloaded at
`chrome://extensions` after editing.

## The actual public-game route (from H644b/Roblox-Public-Web-Play-Join)

Their verified extension achieves playable public Web Play by two patches:
1. Join POST `rbx-user-agent` → `Roblox/Darwin` (or native identity) to change
   internal status 12 → 2.
2. Add `gameFqdn` to the join response, built from the join script:
   ```
   tokenHex(NetStackTokenValue)-ipv4Hex(MachineAddress)-portHex(NetStackPort)
     .ipv4Hex(browserVip).qdmux.roblox.com
   ```
   where `browserVip = UdmuxEndpoints[0].Address` with the **last octet + 1**
   (native `.33` → WebTransport `.34`). Using the native `.33` address makes the
   WebTransport handshake fail. This is the same derivation already implemented
   in `client/page/xhr.ts::patchJoin()`.

Their repo also confirms the browser build has WebSocket bindings but no UDP
socket imports, so RakNet is not reachable from a browser — WebTransport must
be made to work, or a native host must supply UDP itself.

## Vulkan / WebGPU requirement (Chrome on this box)

With plain Chrome the engine failed with
`[WasmPlayer] startup failed: Error: The browser did not provide a WebGPU adapter`.
`navigator.gpu` existed but `requestAdapter()` returned null. The fix is to
launch with Vulkan explicitly enabled:

```
--enable-unsafe-webgpu --ignore-gpu-blocklist --enable-features=Vulkan
--use-angle=vulkan --use-vulkan=native --disable-gpu-sandbox
```

After that `requestAdapter()` succeeds, the page stays at
`play.roblox.com/games/start`, and the engine reaches a real join
(`Assigned peer id`, `#players = 21`).

## Reliable gate patch injection

Reloading the extension did **not** reliably apply our edits. A deterministic
alternative is CDP `Page.addScriptToEvaluateOnNewDocument` with the `fetch`
wrapper above, installed before `Page.navigate`. Verified live: after 15 s the
page URL was still `https://play.roblox.com/games/start?placeId=189707`,
`window.__gateHit === 1`, and no `wasmRedirect` redirect occurred.

## State

The patched extension lives only in `/tmp` (not committed — it is third-party
code). Chrome runs with:

```sh
google-chrome \
  --user-data-dir=/tmp/opencode/chrome-oracle-profile \
  --load-extension=/tmp/opencode/ext-experimental/public-web-play-extension \
  --remote-debugging-port=9222 --remote-allow-origins='*'
```

A CDP collector watches for a session and will record whether the WebTransport
connection survives past ~120 s (our raw Deno shim dies at ~110 s with
`conn.closed REJECTED Error: timed out`).
