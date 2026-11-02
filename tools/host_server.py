#!/usr/bin/env python3
"""host_server.py — self-hosted Roblox web client for the RakNet splice.

One origin, no CDN fetches from the browser, no CORS:

    /                     host/index.html
    /proxy-hook.js        host/proxy-hook.js
    /stub.js              host/stub.js
    /engine/*             local mirror, else proxied to setup.rbxcdn.com
    /proxy/<host>/<path>  forwarded to https://<host>/<path> with the cookie

Why: the page is served from 127.0.0.1, so every Roblox API host is
cross-origin. gamejoin is origin-locked to Roblox origins and the engine's
asset base is the engine directory, so everything is funnelled through here.

usage:
    tools/host_server.py --cookie ~/cookie.txt [--wasm path/to/WebClient.wasm]
                         [--engine-dir /tmp/opencode/rw] [--port 8765] [--tls]
"""

import argparse
import functools
import http.client
import http.server
import os
import socketserver
import ssl
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(ROOT, "host")

CFG = {
    "cookie": None,
    "engine_dir": None,
    "wasm": None,
    "cdn_base": None,       # https://setup.rbxcdn.com/webclient/version-<hash>
}
COOKIE = ""

WEB_UA = ("Roblox/Web Unknown (8192MB; 1366x768; 96x96; 1366x768; ; Linux) "
          "Browser/Chrome/155.0.0.0 RobloxApp/0.745.0.7451145 "
          "(GlobalDist; RobloxDirectDownload)")

HOP = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
       "te", "trailers", "transfer-encoding", "upgrade", "content-encoding",
       "content-length"}


def log(msg):
    sys.stderr.write("[host] %s\n" % msg)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # ---------------------------------------------------------------- utils --

    def _headers(self, code, ctype, length):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def _send_bytes(self, data, ctype="application/octet-stream", code=200):
        self._headers(code, ctype, len(data))
        self.wfile.write(data)

    def _file(self, path, ctype):
        try:
            data = open(path, "rb").read()
        except OSError:
            return False
        self._send_bytes(data, ctype)
        return True

    # --------------------------------------------------------------- routes --

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_OPTIONS(self):
        self._headers(204, "text/plain", 0)

    def _route(self, method):
        path = urllib.parse.urlparse(self.path).path
        try:
            if path == "/" or path == "/index.html":
                return self._file(os.path.join(HOST_DIR, "index.html"), "text/html; charset=utf-8")
            if path == "/proxy-hook.js":
                return self._file(os.path.join(HOST_DIR, "proxy-hook.js"), "text/javascript")
            if path == "/stub.js":
                return self._file(os.path.join(HOST_DIR, "stub.js"), "text/javascript")
            if path.startswith("/engine/"):
                return self._engine(path[len("/engine/"):])
            if path.startswith("/proxy/"):
                rest = path[len("/proxy/"):]
                host, _, sub = rest.partition("/")
                return self._proxy(host, "/" + sub + (("?" + urllib.parse.urlparse(self.path).query) if "?" in self.path else ""))
        except BrokenPipeError:
            return
        except Exception as e:  # noqa: BLE001 - surface instead of dying silently
            log("route error %s: %s" % (path, e))
            return self._send_bytes(b"error", "text/plain", 500)
        self._send_bytes(b"not found", "text/plain", 404)

    # --------------------------------------------------------------- engine --

    def _ctype(self, name):
        if name.endswith(".js"):
            return "text/javascript"
        if name.endswith(".wasm"):
            return "application/wasm"
        if name.endswith(".json"):
            return "application/json"
        if name.endswith(".html"):
            return "text/html; charset=utf-8"
        if name.endswith(".css"):
            return "text/css"
        if name.endswith(".ttf"):
            return "font/ttf"
        if name.endswith(".png"):
            return "image/png"
        if name.endswith(".svg"):
            return "image/svg+xml"
        return "application/octet-stream"

    def _engine(self, rel):
        rel = urllib.parse.unquote(rel.split("?")[0])
        if rel == "WebClient.wasm" and CFG["wasm"]:
            log("serving patched module")
            return self._file(CFG["wasm"], "application/wasm")

        local = os.path.join(CFG["engine_dir"], rel)
        if os.path.isfile(local):
            return self._file(local, self._ctype(rel))

        # not mirrored: proxy to the versioned CDN root
        if not CFG["cdn_base"]:
            return self._send_bytes(b"no cdn base", "text/plain", 500)
        url = CFG["cdn_base"].rstrip("/") + "/" + rel
        return self._proxy_abs("setup.rbxcdn.com", url)

    # ---------------------------------------------------------------- proxy --

    def _proxy(self, host, path):
        return self._proxy_abs(host, "https://" + host + path)

    def _proxy_abs(self, host, url):
        u = urllib.parse.urlparse(url)
        conn = http.client.HTTPSConnection(u.hostname, 443, timeout=30)
        body = None
        clen = self.headers.get("Content-Length")
        if clen:
            body = self.rfile.read(int(clen))

        headers = {
            "Host": u.hostname,
            "User-Agent": self.headers.get("User-Agent", "Mozilla/5.0"),
            "Accept": self.headers.get("Accept", "*/*"),
            "Accept-Encoding": "identity",
            "Referer": "https://play.roblox.com/",
            "rbx-user-agent": WEB_UA,
        }
        ct = self.headers.get("Content-Type")
        if ct:
            headers["Content-Type"] = ct
        for extra in ("Requester", "Cache-Control", "IsFtsThumbAccepted"):
            v = self.headers.get(extra)
            if v:
                headers[extra] = v
        if COOKIE:
            headers["Cookie"] = ".ROBLOSECURITY=" + COOKIE

        target = u.path + (("?" + u.query) if u.query else "")
        try:
            conn.request(self.command, target, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
        except Exception as e:  # noqa: BLE001
            log("upstream %s %s failed: %s" % (self.command, url, e))
            conn.close()
            return self._send_bytes(str(e).encode(), "text/plain", 502)
        finally:
            try:
                conn.close()
            except Exception:
                pass

        ctype = resp.getheader("Content-Type", "application/octet-stream")

        # Force the engine onto RakNet: the web client ships with RbxTransport on.
        if "/v2/settings/application/WebClientApp" in u.path and data[:1] == b"{":
            try:
                import json as _json
                doc = _json.loads(data)
                app = doc.setdefault("applicationSettings", {})
                app["FFlagUseRbxTransportClient3"] = "False"
                app["FFlagUseRbxTransportServer"] = "False"
                data = _json.dumps(doc).encode()
                log("settings: RakNet forced on")
            except Exception as e:  # noqa: BLE001
                log("settings rewrite failed: %s" % e)

        log("%s %s -> %d (%d B)" % (self.command, url[:110], resp.status, len(data)))
        self._headers(resp.status, ctype, len(data))
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cookie", required=True)
    ap.add_argument("--engine-dir", default="/tmp/opencode/rw")
    ap.add_argument("--wasm", default=None, help="patched module to serve as WebClient.wasm")
    ap.add_argument("--cdn-base", default=None,
                    help="https://setup.rbxcdn.com/webclient/version-<hash>")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--cert", default="/tmp/opencode/cert.pem")
    ap.add_argument("--key", default="/tmp/opencode/key.pem")
    a = ap.parse_args()

    global COOKIE
    COOKIE = open(a.cookie).read().strip()
    CFG["engine_dir"] = a.engine_dir
    CFG["wasm"] = a.wasm
    if a.cdn_base:
        CFG["cdn_base"] = a.cdn_base
    else:
        # infer from the mirrored metadata's sibling: engine dir holds the version
        CFG["cdn_base"] = "https://setup.rbxcdn.com/webclient/" + (
            os.path.basename(os.path.abspath(a.engine_dir)) or "")

    class _Srv(socketserver.ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = True

    with _Srv(("127.0.0.1", a.port), Handler) as httpd:
        if a.tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(a.cert, a.key)
            httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https" if a.tls else "http"
        log("serving %s://127.0.0.1:%d  engine=%s  wasm=%s"
            % (scheme, a.port, a.engine_dir, a.wasm or "(mirrored)"))
        httpd.serve_forever()


if __name__ == "__main__":
    main()
