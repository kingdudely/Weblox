#!/usr/bin/env python3
"""Mirror the engine content tree the client actually loads.

setup.rbxcdn.com denies directory listing and the web client ships no zip, so
there is no way to ask for "the whole tree". The list is therefore observed:
`--capture` turns an rkhost log into a path list, and this script fetches those
paths into the app root so the dev server can be a plain file server.

    ./tools/mirror_engine.py --capture /tmp/opencode/rkhost-iwa.log
    ./tools/mirror_engine.py                  # fetch whatever tools/engine.paths lists
    ./tools/mirror_engine.py --force          # re-fetch everything

A 404 under /engine/ in the server log means the engine asked for a file that
was not in the capture. Re-run with `--capture` against that log, then without.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import sys
import urllib.error
import urllib.request

DEFAULT_CDN = "https://setup.rbxcdn.com/webclient/version-75f1039cb4964863-RobloxWebClient"
DEFAULT_LIST = "tools/engine.paths"
DEFAULT_OUT = "iwa/engine"

# The two scripts and the module are produced locally; the module is patched,
# so it must never come from the CDN.
LOCAL_ONLY = ("WebClient.wasm", "WebClient.js", "WasmPlayer.js")


def capture(log_path: str, list_path: str) -> int:
    """Extract engine paths from an rkhost log, merge them into the list.

    Two shapes matter: the old proxy logged `GET .../RobloxWebClient/<path>`,
    and the static server logs `MISSING engine/<path>` when the mirror is short.
    """
    seen: set[str] = set()
    with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.search(r"GET \S*/RobloxWebClient/(\S+)", line) or re.search(
                r"MISSING engine/(\S+)", line
            )
            if not m:
                continue
            path = m.group(1).split("?")[0]
            if path in LOCAL_ONLY or path.startswith("http"):
                continue
            seen.add(path)

    existing: set[str] = set()
    if os.path.exists(list_path):
        with open(list_path, "r", encoding="utf-8") as fh:
            existing = {ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")}

    merged = sorted(existing | seen)
    with open(list_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(merged) + "\n")
    print(f"{len(seen)} in log, {len(merged) - len(existing)} new, {len(merged)} total -> {list_path}")
    return 0


def fetch_one(cdn: str, out_dir: str, rel: str, force: bool) -> tuple[str, str]:
    dest = os.path.join(out_dir, rel)
    if not force and os.path.isfile(dest) and os.path.getsize(dest) > 0:
        return rel, "cached"
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    url = f"{cdn.rstrip('/')}/{rel}"
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        return rel, f"HTTP {e.code}"
    except Exception as e:  # noqa: BLE001 - the summary is the error channel
        return rel, f"{type(e).__name__}: {e}"
    with open(tmp, "wb") as fh:
        fh.write(data)
    os.replace(tmp, dest)
    return rel, f"{len(data)} B"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", metavar="LOG", help="merge engine paths from an rkhost log")
    ap.add_argument("--paths", default=DEFAULT_LIST)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cdn-base", default=DEFAULT_CDN)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--jobs", type=int, default=12)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    if args.capture:
        capture(args.capture, args.paths)
        return 0

    with open(args.paths, "r", encoding="utf-8") as fh:
        paths = [ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")]
    if not paths:
        print(f"no paths in {args.paths}", file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    failed: list[tuple[str, str]] = []
    bytes_total = 0
    fetched = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(fetch_one, args.cdn_base, args.out, p, args.force) for p in paths]
        for fut in concurrent.futures.as_completed(futures):
            rel, status = fut.result()
            if status == "cached":
                continue
            if status.startswith("HTTP") or ":" in status and not status.endswith(" B"):
                failed.append((rel, status))
                continue
            fetched += 1
            bytes_total += int(status.split()[0])

    print(f"{fetched} fetched, {bytes_total / 1e6:.1f} MB, {len(paths) - fetched - len(failed)} cached -> {args.out}")
    for rel, status in failed:
        print(f"  FAILED {rel}: {status}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
