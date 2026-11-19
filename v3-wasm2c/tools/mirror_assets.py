#!/usr/bin/env python3
"""Mirror the engine's rbxasset tree from the CDN into data/content.

The metadata list (data/meta.json) is the authority; every path is fetched to
data/content/<path> if it is not already there with the right size.
"""
import json, os, sys, urllib.request, urllib.parse
from concurrent.futures import ThreadPoolExecutor

BASE = os.environ.get(
    "WC_CDN",
    "https://setup.rbxcdn.com/webclient/version-f807b21caba84809-RobloxWebClient/")
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "content")

entries = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                      "..", "data", "meta.json")))

def one(e):
    path = os.path.join(ROOT, e["path"].lstrip("/"))
    if os.path.isfile(path) and os.path.getsize(path) == e["size"]:
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    url = BASE + urllib.parse.quote(e["path"].lstrip("/"))
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                body = r.read()
            if len(body) != e["size"]:
                raise OSError(f"size {len(body)} != {e['size']}")
            with open(path, "wb") as f:
                f.write(body)
            return 1
        except Exception as ex:
            if attempt == 2:
                return print("FAIL", e["path"], ex)
    return 0

with ThreadPoolExecutor(max_workers=24) as ex:
    got = sum(x or 0 for x in ex.map(one, entries))
print(f"fetched {got} of {len(entries)}")
