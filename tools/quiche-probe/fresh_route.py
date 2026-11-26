#!/usr/bin/env python3
"""Produce a fresh Roblox/qdmux WebTransport route for quiche-probe.

Uses a native gamejoin identity with the browser WebTransport UDMUX address.
The qdmux FQDN is derived exactly as in the Web Client patch / join-route.json:

    hex(base64decode(NetStackTokenValue)) - hex(MachineAddress) - hex(NetStackPort)
        . hex(UdmuxEndpoints[0].Address with last octet + 1) . qdmux.roblox.com

The last-octet + 1 maps the native UDMUX `.33` address to the browser/qdmux
`.34` multiplexer, which is the address that accepts the WebTransport handshake.

Usage:
    PLACE=189707 python3 tools/quiche-probe/fresh_route.py > route.json
"""

import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

PLACE = int(os.environ.get("PLACE", "189707"))
COOKIE_FILE = os.environ.get("COOKIE_FILE", "/home/john/mizg.txt")
JOIN_URL = "https://gamejoin.roblox.com/v1/join-game"


def ipv4_hex(addr):
    parts = [int(x) for x in addr.split(".")]
    assert len(parts) == 4 and all(0 <= p <= 255 for p in parts), addr
    return "".join("%02x" % p for p in parts), parts


def main():
    cookie = open(COOKIE_FILE).read().strip()
    body = {
        "placeId": PLACE,
        "isTeleport": False,
        "gameJoinAttemptId": str(uuid.uuid4()),
        "browserTrackerId": 0,
        "playSessionId": str(uuid.uuid4()),
        "eventId": str(uuid.uuid4()),
        "launchData": "",
        "joinAttemptOrigin": "PlayButton",
    }
    req = urllib.request.Request(
        JOIN_URL, data=json.dumps(body).encode(), method="POST"
    )
    req.add_header("Content-Type", "application/json")
    req.add_header("Cookie", ".ROBLOSECURITY=" + cookie)
    req.add_header("User-Agent", "Roblox/WinInet")
    req.add_header("rbx-user-agent", "Roblox/Darwin")
    req.add_header("Requester", "Client")
    req.add_header("Referer", "https://www.roblox.com/")
    req.add_header("Origin", "https://www.roblox.com")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            reply = json.load(resp)
    except urllib.error.HTTPError as e:
        reply = json.load(e)

    js = reply.get("joinScript")
    if isinstance(js, str):
        js = json.loads(js)
    if not js:
        print("no joinScript; status=%r message=%r" % (reply.get("status"), reply.get("message")), file=sys.stderr)
        sys.exit(1)

    cfg = js.get("NetStackConfig") or "{}"
    if isinstance(cfg, str):
        cfg = json.loads(urllib.parse.unquote(cfg))

    tok_bytes = base64.b64decode(js["NetStackTokenValue"])
    assert len(tok_bytes) == 16, len(tok_bytes)
    tok_hex = tok_bytes.hex()
    mach_hex, _ = ipv4_hex(js["MachineAddress"])
    port = int(js["NetStackPort"])
    native_addr = js["UdmuxEndpoints"][0]["Address"]
    _, udmux_parts = ipv4_hex(native_addr)
    assert udmux_parts[3] % 2 == 1 and udmux_parts[3] != 255, native_addr
    browser_parts = udmux_parts[:3] + [udmux_parts[3] + 1]
    browser_hex, _ = ipv4_hex(".".join(map(str, browser_parts)))
    fqdn = "%s-%s-%04x.%s.qdmux.roblox.com" % (tok_hex, mach_hex, port, browser_hex)

    route = {
        "fqdn": fqdn,
        "nativeHost": native_addr,
        "browserHost": ".".join(map(str, browser_parts)),
        "netstackPort": port,
        "caps": cfg["caps"],
        "certHashes": cfg["certHashes"],
        "pubKey": cfg.get("pubKey"),
        "configKeys": sorted(cfg.keys()),
        "placeId": PLACE,
        "gameId": reply.get("jobId"),
        "status": reply.get("status"),
    }
    print(json.dumps(route, indent=2))


if __name__ == "__main__":
    main()
