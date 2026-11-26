#!/usr/bin/env python3
"""Parse a HOST_TRACE=1 native-host log and report which import slots were hit.

Usage:
  host/import_trace.py JOIN_LOG [JOIN_LOG ...]        # ranked hit list
  host/import_trace.py --all-stubs JOIN_LOG           # hits + every stub slot
  host/import_trace.py --stub-catalog                 # just the stub catalog

The signatures come from the generated declarations in imports.h (never edit
those by hand).  ``stub`` vs ``real`` is derived from the two marker regions in
imports.cpp, so a promoted import is reported as real immediately after
regeneration.  Import names/slots in the log are cross-checked against the
header; a mismatch is reported instead of being silently trusted.
"""

from __future__ import annotations

import argparse
import re
import signal
import sys
from pathlib import Path

# Slots 86..97 are RNA imports: imports.cpp leaves the generated stub pointer
# in the table, but weblox::RnaBridge::attach() overwrites wasm_call at runtime
# (see rna_bridge.cpp).  They are not "zero-returning in the running host".
BRIDGED = set(range(86, 98))

DECL_RE = re.compile(
    r"^\s*(?P<ret>void|int32_t|uint32_t|int64_t|uint64_t|float|double)\s+"
    r"wasm_import_(?P<slot>\d{3})_(?P<name>\w+)\s*\((?P<args>[^)]*)\)\s*;",
    re.MULTILINE,
)
# Generated comment above each declaration, e.g.
#   // slot   4: env.emscripten_get_now  () -> f64  (type 92)
COMMENT_RE = re.compile(
    r"//\s*slot\s+(?P<slot>\d+):\s+\w+\.(?P<name>\w+)\s+"
    r"\((?P<args>[^)]*)\)\s*->\s*(?P<ret>\w+)"
)
HIT_RE = re.compile(
    r"\[import\] first call slot=(?P<slot>\d+)\s+(?P<module>[\w.]+)\.(?P<name>[\w]+)"
    r"(?:\s+args=(?P<args>[^\n]*))?"
)
HAND_BEGIN = "// === BEGIN HANDWRITTEN IMPLEMENTATIONS ==="
HAND_END = "// === END HANDWRITTEN IMPLEMENTATIONS ==="


def read_decls(root: Path) -> dict[int, tuple[str, str]]:
    """slot -> (name, wasm signature from the generated comment)."""
    text = (root / "imports.h").read_text()
    comments = {int(m.group("slot")): m for m in COMMENT_RE.finditer(text)}
    out: dict[int, tuple[str, str]] = {}
    for m in DECL_RE.finditer(text):
        slot = int(m.group("slot"))
        c = comments.get(slot)
        if c and c.group("name") == m.group("name"):
            sig = f"({c.group('args').strip()}) -> {c.group('ret')}"
        else:  # fall back to the C declaration, minus the two hidden vmctx args
            args = [a.strip() for a in m.group("args").split(",")
                    if a.strip() and not a.strip().startswith("void*")]
            sig = ", ".join(args) or "void"
            if m.group("ret") != "void":
                sig += f" -> {m.group('ret')}"
        out[slot] = (m.group("name"), sig)
    return out


def read_real_names(root: Path) -> set[str]:
    text = (root / "imports.cpp").read_text()
    hand = text.split(HAND_BEGIN, 1)[1].split(HAND_END, 1)[0]
    return {m.group(1) for m in re.finditer(r"wasm_import_\d{3}_(\w+)", hand)}


def parse_logs(paths: list[str], decls: dict[int, tuple[str, str]]) -> list[dict]:
    hits: dict[int, dict] = {}
    order = 0
    for path in paths:
        try:
            lines = Path(path).read_text(errors="replace").splitlines()
        except OSError as e:
            print(f"warning: {path}: {e}", file=sys.stderr)
            continue
        for line in lines:
            m = HIT_RE.search(line)
            if not m:
                continue
            slot = int(m.group("slot"))
            order += 1
            name, sig = decls.get(slot, (m.group("name"), "?"))
            mismatch = "" if name == m.group("name") else f" LOG-NAME={m.group('name')}"
            if slot in hits:
                hits[slot]["count"] += 1
                continue
            hits[slot] = dict(
                slot=slot, name=name + mismatch, sig=sig, module=m.group("module"),
                args=m.group("args") or "", count=1, order=order, log=Path(path).name,
            )
    return sorted(hits.values(), key=lambda h: h["order"])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logs", nargs="*", help="HOST_TRACE=1 log files")
    ap.add_argument("--all-stubs", action="store_true",
                    help="also list every still-zero-returning stub slot")
    ap.add_argument("--stub-catalog", action="store_true",
                    help="only print the stub catalog (no logs needed)")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent),
                    help="directory holding imports.h/imports.cpp")
    a = ap.parse_args()
    root = Path(a.root)
    decls = read_decls(root)
    real = read_real_names(root)
    stubs = {s: v for s, v in decls.items()
             if v[0] not in real and s not in BRIDGED}

    if a.stub_catalog:
        print(f"# {len(stubs)} zero-returning stubs (+{len(BRIDGED)} RNA slots "
              f"bridged at runtime) / {len(decls)} imports")
        for s, (name, sig) in sorted(stubs.items()):
            print(f"{s:3d}  env.{name:<42} {sig}")
        return 0

    hits = parse_logs(a.logs, decls)
    stub_names = {v[0] for v in stubs.values()}
    for h in hits:
        base = h["name"].split(" ")[0]
        h["kind"] = ("STUB" if base in stub_names else
                     "bridge" if h["slot"] in BRIDGED else "real")
    stub_hits = [h for h in hits if h["kind"] == "STUB"]
    print(f"# {len(hits)} distinct import slots hit across {len(a.logs)} log(s); "
          f"{len(stub_hits)} zero-returning stubs, "
          f"{len(hits)-len(stub_hits)-sum(h['kind']=='bridge' for h in hits)} real, "
          f"{sum(h['kind']=='bridge' for h in hits)} runtime-bridged")
    print(f"{'#':>3}  {'slot':>4}  {'kind':<6}  import / signature")
    for h in hits:
        print(f"{h['order']:3d}  {h['slot']:4d}  {h['kind']:<6}  "
              f"{h['module']}.{h['name']}({h['sig']})  x{h['count']}")
        if h["args"]:
            print(f"     args={h['args']}")
    if a.all_stubs:
        print(f"\n# stub catalog ({len(stubs)} slots not implemented in the "
              "handwritten region)")
        for s, (name, sig) in sorted(stubs.items()):
            mark = " <- HIT" if any(h["slot"] == s for h in stub_hits) else ""
            print(f"{s:3d}  env.{name:<42} {sig}{mark}")
    return 0


if __name__ == "__main__":
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass
    raise SystemExit(main())
