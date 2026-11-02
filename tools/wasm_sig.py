#!/usr/bin/env python3
"""wasm_sig.py — import names, function signatures, and callers of a function.

The module has no name section, so identity comes from three things:
  * the import section (``env.sock_sendto`` style names, if any survived)
  * the type section (a function's arity/shape)
  * the call graph (who calls whom)

usage:
    ./wasm_sig.py WebClient.wasm --imports
    ./wasm_sig.py WebClient.wasm --sig 6 --ret 1 [--max-body 4000]
    ./wasm_sig.py WebClient.wasm --callers 127498
"""

import argparse
import importlib.util
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("wasm_scan", os.path.join(_here, "wasm_scan.py"))
ws = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ws)


def skip_limits(buf, p):
    flags = buf[p]
    p += 1
    v, p = ws.read_leb(buf, p)
    if flags & 1:
        v, p = ws.read_leb(buf, p)
    return p


def types(buf):
    """List of (nparams, nresults) per type index."""
    out = []
    for sid, name, start, size in ws.sections(buf):
        if sid != 1:
            continue
        n, p = ws.read_leb(buf, start)
        for _ in range(n):
            if buf[p] != 0x60:
                raise SystemExit("unexpected functype %#x" % buf[p])
            p += 1
            np_, p = ws.read_leb(buf, p)
            params = list(buf[p:p + np_])
            p += np_
            nr, p = ws.read_leb(buf, p)
            results = list(buf[p:p + nr])
            p += nr
            out.append((len(params), len(results)))
    return out


def imports(buf):
    """[(kind, module, field, type_index_or_None)]."""
    out = []
    for sid, name, start, size in ws.sections(buf):
        if sid != 2:
            continue
        n, p = ws.read_leb(buf, start)
        for _ in range(n):
            ml, p = ws.read_leb(buf, p)
            mod = buf[p:p + ml].decode("utf-8", "replace")
            p += ml
            fl, p = ws.read_leb(buf, p)
            fld = buf[p:p + fl].decode("utf-8", "replace")
            p += fl
            kind = buf[p]
            p += 1
            if kind == 0:
                t, p = ws.read_leb(buf, p)
                out.append(("func", mod, fld, t))
            elif kind == 1:
                p += 1
                p = skip_limits(buf, p)
                out.append(("table", mod, fld, None))
            elif kind == 2:
                p = skip_limits(buf, p)
                out.append(("memory", mod, fld, None))
            elif kind == 3:
                p += 2
                out.append(("global", mod, fld, None))
            elif kind == 4:
                _, p = ws.read_leb(buf, p)
                out.append(("tag", mod, fld, None))
    return out


def func_types(buf):
    """type index for every function index (imports first)."""
    imp = [x for x in imports(buf) if x[0] == "func"]
    out = [x[3] for x in imp]
    for sid, name, start, size in ws.sections(buf):
        if sid != 3:
            continue
        n, p = ws.read_leb(buf, start)
        for _ in range(n):
            t, p = ws.read_leb(buf, p)
            out.append(t)
    return out


def body_calls(buf, idx, ftypes, bodies):
    """Distinct `call N` targets inside one function body."""
    for fi, bs, be in bodies:
        if fi != idx:
            continue
        blob = buf[bs:be]
        return sorted({int(m.group(1)) for m in re.finditer(rb"\x10([\x00-\x0f])", blob)}) if False else \
            sorted({_read_uleb(blob, m.start() + 1) for m in re.finditer(rb"\x10", blob)})
    return []


def _read_uleb(blob, p):
    val = shift = 0
    while True:
        b = blob[p]
        p += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val
        shift += 7


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wasm")
    ap.add_argument("--imports", action="store_true")
    ap.add_argument("--sig", type=int, default=None, help="match this many params")
    ap.add_argument("--ret", type=int, default=None, help="match this many results")
    ap.add_argument("--max-body", type=int, default=0, help="skip bodies larger than N bytes")
    ap.add_argument("--callers", type=int, default=None, help="list callers of this func index")
    ap.add_argument("--callees", type=int, default=None, help="list callees of this func index")
    a = ap.parse_args()

    buf = open(a.wasm, "rb").read()
    ty = types(buf)
    ft = func_types(buf)
    imp = [x for x in imports(buf) if x[0] == "func"]

    if a.imports:
        for i, (k, m, f, t) in enumerate(imp):
            print("%4d  %s.%s  %s" % (i, m, f, ty[t] if t is not None else "?"))
        print("imported funcs:", len(imp), " defined:", len(ft) - len(imp))
        return

    if a.sig is not None:
        hits = []
        for i, t in enumerate(ft):
            np_, nr = ty[t]
            if np_ == a.sig and (a.ret is None or nr == a.ret):
                hits.append(i)
        print("functions with params=%d ret=%s: %d" % (a.sig, a.ret, len(hits)))
        if a.max_body:
            bodies = {fi: (bs, be) for fi, bs, be in ws.parse(buf)["bodies"]}
            hits = [h for h in hits if h in bodies and bodies[h][1] - bodies[h][0] <= a.max_body]
            print("after body-size filter <=%d: %d" % (a.max_body, len(hits)))
        for h in hits[:60]:
            print("  func[%d]" % h)
        if len(hits) > 60:
            print("  ... %d more" % (len(hits) - 60))
        return

    if a.callers is not None:
        target = a.callers
        hits = []
        for sid, name, start, size in ws.sections(buf):
            if sid != 10:
                continue
            n, p = ws.read_leb(buf, start)
            for i in range(n):
                bs, p = ws.read_leb(buf, p)
                be = p + bs
                blob = buf[p:be]
                for m in re.finditer(rb"\x10", blob):
                    if _read_uleb(blob, m.start() + 1) == target:
                        hits.append(len(imp) + i)
                        break
                p = be
        print("callers of func[%d]: %d" % (target, len(hits)))
        for h in hits[:80]:
            print("  func[%d]  params=%s" % (h, ty[ft[h]]))
        return

    if a.callees is not None:
        for fi, bs, be in ws.parse(buf)["bodies"]:
            if fi == a.callees:
                blob = buf[bs:be]
                seen = sorted({_read_uleb(blob, m.start() + 1) for m in re.finditer(rb"\x10", blob)})
                print("callees of func[%d]: %d" % (a.callees, len(seen)))
                for c in seen:
                    print("  func[%-6d] %s" % (c, ty[ft[c]]))
                return


if __name__ == "__main__":
    main()
