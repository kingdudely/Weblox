// @ts-nocheck
// XMLHttpRequest over fetch, plus the join-response patch the extension does
// in the browser (native VIP -> browser VIP in gameFqdn).
import { patchHeaders } from "./env.ts";

const JOIN_ORIGIN = "https://gamejoin.roblox.com";
const JOIN_PATHS = new Set(["/v1/join-game", "/v1/join-game-instance", "/v1/join-reserved-game", "/v1/join-private-game"]);
const DOMAIN = "qdmux.roblox.com";
// Roblox intermittently answers status 2 with an empty NetStackTokenValue.
// Re-post the same join in place until a usable token arrives (no restarts).
const MAX_JOIN_RETRIES = 30;
const RETRY_DELAY_MS = 250;

function ipv4Parts(v) {
  if (typeof v !== "string") throw new Error("Missing IPv4 address");
  const p = v.split(".").map(Number);
  if (p.length !== 4 || p.some((x) => !Number.isInteger(x) || x < 0 || x > 255)) throw new Error("Invalid IPv4 address");
  return p;
}
function ipv4Hex(p) { return p.map((x) => x.toString(16).padStart(2, "0")).join(""); }
function tokenHex(v) {
  if (typeof v !== "string") throw new Error("Missing network token");
  if (/^[0-9a-f]{32}$/i.test(v)) return v.toLowerCase();
  const b = atob(v);
  if (b.length !== 16) throw new Error("Invalid network token length");
  return Array.from(b, (c) => c.charCodeAt(0).toString(16).padStart(2, "0")).join("");
}
export function patchJoin(raw) {
  let res;
  try { res = JSON.parse(raw); } catch { return null; }
  if (res?.status !== 2) return null;
  const isStr = typeof res.joinScript === "string";
  let js;
  try { js = isStr ? JSON.parse(res.joinScript) : res.joinScript; } catch { return null; }
  // Match the Chrome extension: only derive a browser route when the server
  // did not provide gameFqdn. Never overwrite a value the server supplied.
  if (!js || typeof js !== "object" || js.gameFqdn) return null;
  // Roblox intermittently returns status 2 with an empty network token. Do not
  // fail the handshake here — the XHR layer retries the join request itself.
  if (!js.NetStackTokenValue || !js.UdmuxEndpoints?.length) return null;
  try {
    const vip = ipv4Parts(js.UdmuxEndpoints?.[0]?.Address);
    if (vip[3] % 2 !== 1 || vip[3] === 255) return null;
    const browserVip = [...vip];
    browserVip[3] += 1;
    const machine = ipv4Parts(js.MachineAddress);
    const port = Number(js.NetStackPort);
    if (!Number.isInteger(port) || port < 1 || port > 65535) return null;
    js.gameFqdn = `${tokenHex(js.NetStackTokenValue)}-${ipv4Hex(machine)}-` +
      `${port.toString(16).padStart(4, "0")}.${ipv4Hex(browserVip)}.${DOMAIN}`;
    res.joinScript = isStr ? JSON.stringify(js) : js;
    return JSON.stringify(res);
  } catch { return null; }
}

/** Roblox occasionally returns a join response with an empty network token;
 *  retrying the same request gets a usable one. Detect that before delivering
 *  the response to the engine, which would otherwise fail with no transport. */
function joinTokenMissing(buf: Uint8Array) {
  try {
    const res = JSON.parse(new TextDecoder().decode(buf));
    const js = typeof res.joinScript === "string" ? JSON.parse(res.joinScript) : res.joinScript;
    if (!js || typeof js !== "object") return false;
    const t = js.NetStackTokenValue;
    return typeof t === "string" ? t.length < 16 : true;
  } catch { return false; }
}

export class XHR {
  constructor() {
    this.readyState = 0;
    this.status = 0;
    this.statusText = "";
    this.response = null;
    this.responseText = "";
    this.responseURL = "";
    this.responseType = "";
    this.withCredentials = true;
    this.timeout = 0;
    this.onload = null;
    this.onerror = null;
    this.ontimeout = null;
    this.onprogress = null;
    this.onreadystatechange = null;
    this.onabort = null;
    this._h = new Headers();
    this._chunks = [];
    this._join = false;
    this._ctrl = null;
  }
  open(method, url) {
    this._method = method;
    this._url = String(url);
    this.readyState = 1;
    try {
      const u = new URL(this._url, location.href);
      this._join = String(method).toUpperCase() === "POST" && u.origin === JOIN_ORIGIN && JOIN_PATHS.has(u.pathname);
    } catch { this._join = false; }
  }
  setRequestHeader(k, v) { this._h.set(k, v); }
  getResponseHeader(k) { return this._k?.get(k) ?? null; }
  getAllResponseHeaders() {
    let out = "";
    for (const [k, v] of this._k || []) out += `${k}: ${v}\r\n`;
    return out;
  }
  abort() { this._ctrl?.abort(); }
  _setResponse(final) {
    const total = this._chunks.reduce((n, c) => n + c.length, 0);
    const buf = new Uint8Array(total);
    let at = 0;
    for (const c of this._chunks) { buf.set(c, at); at += c.length; }
    const body = final && this._join ? patchJoin(new TextDecoder().decode(buf)) : null;
    const bytes = body === null ? buf : new TextEncoder().encode(body);
    if (this.responseType === "arraybuffer") this.response = bytes.buffer;
    else if (this.responseType === "blob") this.response = new Blob([bytes]);
    else if (this.responseType === "json") { try { this.response = JSON.parse(new TextDecoder().decode(bytes)); } catch { this.response = null; } }
    else { this.responseText = new TextDecoder().decode(bytes); this.response = this.responseText; }
  }
  send(body) {
    const ctrl = (this._ctrl = new AbortController());
    // Public places require rbx-user-agent: Roblox/Darwin on the join POST;
    // the regular User-Agent stays the Chrome UA (set by patchHeaders).
    if (this._join) this._h.set("rbx-user-agent", "Roblox/Darwin");
    patchHeaders(this._url, this._h);
    const timer = this.timeout ? setTimeout(() => { ctrl.abort(); this.ontimeout?.({}); }, this.timeout) : null;
    this.readyState = 2;
    this.onreadystatechange?.({});
    const attempt = (n: number) =>
      fetch(this._url, { method: this._method, headers: this._h, body, signal: ctrl.signal, redirect: "follow" })
        .then(async (r) => {
          this._k = r.headers;
          this.status = r.status;
          if (!r.ok) console.error("[xhr ]", this._method, this._url, "->", r.status);
          this.statusText = r.statusText;
          this.responseURL = r.url;
          const total = Number(r.headers.get("content-length")) || 0;
          this.readyState = 3;
          if (r.body) {
            let loaded = 0;
            for await (const c of r.body) {
              this._chunks.push(c);
              loaded += c.length;
              this._setResponse(false);
              this.onprogress?.({ loaded, total, lengthComputable: !!total });
            }
          }
          const bytes = this._chunks.reduce((a, c) => a + c.length, 0);
          const buf = new Uint8Array(bytes);
          let at = 0;
          for (const c of this._chunks) { buf.set(c, at); at += c.length; }
          if (this._join && r.ok && joinTokenMissing(buf) && n <= MAX_JOIN_RETRIES) {
            console.error("[xhr ] empty network token, retrying gamejoin (attempt", n, "/", MAX_JOIN_RETRIES, ")");
            this._chunks = [];
            await new Promise((r) => setTimeout(r, RETRY_DELAY_MS));
            return attempt(n + 1);
          }
          this._setResponse(true);
          this.readyState = 4;
          this.onreadystatechange?.({});
          this.onload?.({});
          clearTimeout(timer);
        });
    attempt(1)
      .catch((e) => {
        clearTimeout(timer);
        if (e.name === "AbortError") { this.onabort?.({}); return; }
        console.error("[xhr ]", this._url, e.message);
        this.onerror?.({});
      });
  }
}
