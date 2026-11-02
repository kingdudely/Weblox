# extension

Unpacked MV3 extension that runs the **patched** engine on play.roblox.com.

- `rules.json` redirects `WebClient.wasm` to the bundled
  `WebClient.patched.wasm` at the network layer, so it applies in every context
  (page *and* worker) with no JS hooking, no CORS, no mixed content.
- `content.js` (MAIN world, document_start) rewrites the client-settings
  response to force `FFlagUseRbxTransportClient3=False`.

Install for a normal browser: `chrome://extensions` → Developer mode → Load
unpacked → this folder. No Chrome flags needed.

For automation, `--load-extension` is blocked on current Chrome; use
`--enable-unsafe-extension-debugging` and CDP `Extensions.loadUnpacked`
(`tools/probe/tapprobe.js --ext host/extension`).

The bundled `.wasm` is gitignored; rebuild it with `tools/patch.py` and copy it
in before loading.
