#!/usr/bin/env python3
"""wasm_scan.py — locate functions in the Roblox web engine by data anchor.

The module has no name section and 167,913 functions, so nothing can be found by
name. What always survives is the string table: every log format, flag name and
assert path is in the data segments. A function that references a string loads
its linear address with `i32.const`, so:

    anchor string -> linear address (runtime heap scan)
                  -> `41 <sleb>` occurrences in the code section
                  -> containing function index
                  -> table index in the exported __indirect_function_table

Everything here works on raw bytes, so it does not care that distro wabt cannot
decode the module's post-MVP encodings.

usage:
    ./wasm_scan.py WebClient.wasm --addr 0x12ab3400 [--addr ...]
    ./wasm_scan.py WebClient.wasm --addr 0x12ab3400 --context 24
"""

import argparse
import sys


def read_leb(buf, pos):
    """Unsigned LEB128. Returns (value, new_pos)."""
    val = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated leb")
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7
        if shift > 63:
            raise ValueError("leb overflow")


def read_sleb(buf, pos, bits=32):
    """Signed LEB128 (for i32.const immediates)."""
    val = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated sleb")
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            if shift < bits and (b & 0x40):
                val |= -(1 << shift)
            return val & ((1 << bits) - 1), pos
        if shift > bits + 7:
            raise ValueError("sleb overflow")


def _skip_limits(buf, pos):
    """limits: flags byte, then min (and max when flags has bit 0)."""
    flags = buf[pos]
    pos += 1
    _, pos = read_leb(buf, pos)
    if flags & 0x01:
        _, pos = read_leb(buf, pos)
    return pos


def sections(buf):
    """Yield (id, name, payload_start, payload_len)."""
    if buf[:4] != b"\x00asm":
        raise SystemExit("not a wasm module")
    pos = 8
    while pos < len(buf):
        sid = buf[pos]
        pos += 1
        size, pos = read_leb(buf, pos)
        payload = pos
        name = None
        if sid == 0:
            nlen, p = read_leb(buf, payload)
            name = buf[p:p + nlen].decode("utf-8", "replace")
        yield sid, name, payload, size
        pos += size


def parse(buf):
    """Return dict with code bodies, import func count, elem mapping."""
    bodies = []          # (func_index, body_start, body_end)
    imported_funcs = 0
    elem_map = {}        # func_index -> table_index
    for sid, name, start, size in sections(buf):
        end = start + size
        if sid == 2:                                  # import
            count, p = read_leb(buf, start)
            for _ in range(count):
                mlen, p = read_leb(buf, p)
                p += mlen                              # module
                flen, p = read_leb(buf, p)
                p += flen                              # field
                kind = buf[p]
                p += 1
                if kind == 0x00:                       # func
                    _, p = read_leb(buf, p)
                    imported_funcs += 1
                elif kind == 0x01:                     # table
                    p += 1                             # reftype
                    p = _skip_limits(buf, p)
                elif kind == 0x02:                     # memory
                    p = _skip_limits(buf, p)
                elif kind == 0x03:                     # global
                    p += 2
                elif kind == 0x04:                     # tag
                    _, p = read_leb(buf, p)
                else:
                    raise SystemExit("unknown import kind %#x" % kind)
        elif sid == 9:                                 # element
            count, p = read_leb(buf, start)
            for _ in range(count):
                flags, p = read_leb(buf, p)
                if flags == 0:
                    op = buf[p]
                    p += 1
                    if op == 0x41:
                        base, p = read_sleb(buf, p)
                    else:
                        raise SystemExit("elem offset opcode %#x" % op)
                    if buf[p] != 0x0B:
                        raise SystemExit("elem offset not terminated")
                    p += 1
                    n, p = read_leb(buf, p)
                    for k in range(n):
                        f, p = read_leb(buf, p)
                        elem_map.setdefault(f, base + k)
                else:
                    raise SystemExit("unsupported elem flags %#x" % flags)
        elif sid == 10:                                # code
            count, p = read_leb(buf, start)
            for i in range(count):
                body_size, p = read_leb(buf, p)
                bodies.append((imported_funcs + i, p, p + body_size))
                p += body_size
            if p > end:
                raise SystemExit("code section overrun")
    return {"bodies": bodies, "elem": elem_map, "imported_funcs": imported_funcs}


def find_const(buf, body_start, body_end, target):
    """Yield offsets of `i32.const target` inside one function body."""
    p = body_start
    while p < body_end - 1:
        if buf[p] == 0x41:
            try:
                val, np = read_sleb(buf, p + 1)
            except ValueError:
                p += 1
                continue
            if val == target:
                yield p
            p = np
        else:
            p += 1


def fmt_addr(a):
    return "0x%x" % a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wasm")
    ap.add_argument("--addr", action="append", default=[],
                    help="linear address of an anchor string (repeatable)")
    ap.add_argument("--context", type=int, default=0,
                    help="also print N bytes of the containing body as hex")
    args = ap.parse_args()

    buf = open(args.wasm, "rb").read()
    info = parse(buf)
    print("module: %s" % args.wasm)
    print("functions: %d defined (+%d imported)  elem entries: %d"
          % (len(info["bodies"]), info["imported_funcs"], len(info["elem"])))

    for raw in args.addr:
        target = int(raw, 0)
        hits = []
        for fidx, s, e in info["bodies"]:
            for off in find_const(buf, s, e, target):
                hits.append((fidx, s, e, off))
        print("\nanchor %s -> %d function(s)" % (fmt_addr(target), len({h[0] for h in hits})))
        for fidx, s, e, off in hits[:20]:
            as_import = fidx - info["imported_funcs"]
            body_len = e - s
            table_idx = info["elem"].get(fidx)
            print("  func[%d] (defined #%d, %d B) i32.const at body+%d  table=%s"
                  % (fidx, as_import, body_len, off - s,
                     table_idx if table_idx is not None else "not-in-table"))
            if args.context:
                lo = max(s, off - args.context)
                hi = min(e, off + args.context)
                print("    %s" % buf[lo:hi].hex(" "))
        if not hits:
            print("  (no reference — anchor may be reached indirectly or dead)")


if __name__ == "__main__":
    sys.exit(main())
