#!/usr/bin/env python3
"""serve_wasm.py — serve a patched engine module to a live page.

The page is cross-origin isolated (COOP/COEP), so a cross-origin fetch of the
module needs CORS *and* a permissive CORP header. The CDP probe redirects
`WebClient.wasm` here with `Fetch.continueRequest({url})`.

usage: serve_wasm.py --file /path/to/WebClient.patched.wasm [--port 8765]
"""

import argparse
import functools
import http.server
import os
import socketserver
import sys

FIXED = None


def _send(handler, path, ctype):
    try:
        data = open(path, "rb").read()
    except OSError:
        handler.send_error(404)
        return
    handler.send_response(200)
    handler.send_header("Content-Type", ctype)
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    handler.wfile.write(data)


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if FIXED and self.path.split("?")[0].endswith("WebClient.wasm"):
            sys.stderr.write("[srv] serving patched module (%s)\n" % self.path)
            return _send(self, FIXED, "application/wasm")
        return super().do_GET()

    def log_message(self, fmt, *args):
        sys.stderr.write("[srv] " + (fmt % args) + "\n")


def main():
    global FIXED
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--dir", default=".")
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()

    FIXED = os.path.abspath(a.file)
    handler = functools.partial(Handler, directory=os.path.abspath(a.dir))
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", a.port), handler) as httpd:
        sys.stderr.write("[srv] http://127.0.0.1:%d/WebClient.wasm -> %s\n" % (a.port, FIXED))
        httpd.serve_forever()


if __name__ == "__main__":
    main()
