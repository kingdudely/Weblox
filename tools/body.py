#!/usr/bin/env python3
"""body.py — print the wasm instruction listing for one function.

The 2.8 GB disassembly in .cache/disasm.txt is produced by tools/disasm.sh.
Scanning it with grep for a single function is awkward to type every time, so
this wraps it: it streams the file once and stops at the requested function.

usage:
    body.py 127931                 # whole function
    body.py 127931 --from 500 --to 620
    body.py 127931 --grep 'call '  # instruction lines matching a regex
    body.py 127931 --calls         # callee histogram with signatures
    body.py 127931 --calls-to 10567  # context around every call to func[N]

Function indices are per-build. Re-resolve with tools/wasm_sig.py after an
engine update; see docs/PATCHING.md.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISASM = os.path.join(ROOT, ".cache", "disasm.txt")
FUNC_RE = re.compile(r"^\s*[0-9a-f]+ func\[(\d+)\]")


def stream():
    with open(DISASM, errors="replace") as fh:
        cur = None
        for line in fh:
            m = FUNC_RE.match(line)
            if m:
                cur = int(m.group(1))
                continue
            if cur is None or "|" not in line:
                continue
            ins = line.split("|", 1)[1].strip()
            if ins:
                yield cur, ins


def collect(idx, limit=None):
    out = []
    for f, ins in stream():
        if f == idx:
            out.append(ins)
            if limit and len(out) >= limit:
                break
        elif out:
            break
    return out


def signatures():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import wasm_sig

    buf = open(os.path.join("/tmp/opencode/rw", "WebClient.wasm"), "rb").read()
    types = wasm_sig.types(buf)
    ft = wasm_sig.func_types(buf)
    return {i: types[t] for i, t in enumerate(ft)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("func", type=int)
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--to", dest="end", type=int, default=None)
    ap.add_argument("--grep", default=None)
    ap.add_argument("--calls", action="store_true")
    ap.add_argument("--calls-to", dest="calls_to", type=int, default=None)
    ap.add_argument("--sig", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(DISASM):
        raise SystemExit("missing %s — run tools/disasm.sh first" % DISASM)

    ins = collect(args.func)
    if not ins:
        raise SystemExit("func[%d] not found" % args.func)

    if args.calls or args.calls_to is not None:
        sigs = signatures()
        hist = {}
        for i, x in enumerate(ins):
            m = re.match(r"call (\d+)$", x)
            if m:
                hist.setdefault(int(m.group(1)), []).append(i)
        if args.calls:
            print("func[%d] %s  %d ins, %d callees"
                  % (args.func, sigs.get(args.func), len(ins), len(hist)))
            for f, sites in sorted(hist.items()):
                print("  func[%-6d] %-10s %dx  at %s"
                      % (f, sigs.get(f, "?"), len(sites),
                         ",".join(map(str, sites[:8]))))
            return
        sites = hist.get(args.calls_to, [])
        print("call %d sites in func[%d]: %s" % (args.calls_to, args.func, sites))
        for s in sites:
            print("\n--- site %d ---" % s)
            for k in range(max(0, s - 24), min(len(ins), s + 10)):
                print("%6d %s" % (k, ins[k]))
        return

    if args.sig:
        sigs = signatures()
        print("func[%d] %s" % (args.func, sigs.get(args.func)))

    for i, x in enumerate(ins):
        if i < args.start or (args.end is not None and i >= args.end):
            continue
        if args.grep and not re.search(args.grep, x):
            continue
        print("%6d %s" % (i, x))


if __name__ == "__main__":
    sys.exit(main())
