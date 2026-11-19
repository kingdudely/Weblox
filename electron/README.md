# alcohol (electron)

Reference host: loads the official `play.roblox.com` start page in a Chromium
window and patches only the two things that page gets wrong for a public join
(native identity on the join, and the missing transport host name). Everything
else — WebGPU, WebTransport, input, pointer lock, windowing — is Chromium's.

It is an oracle for the native hosts in `../v2-wasmtime` and `../v3-wasm2c`,
not the density path.

```sh
npm install
WC_COOKIE='_|WARNING...' ./run.sh 1818
WC_COOKIE_FILE=/path/to/cookie ./run.sh 1818
```

`WC_DEBUG=1` also opens `--remote-debugging-port=9222`. Escape → Leave closes
the run and the app.
