#!/usr/bin/env python3
"""patch.py — rewrite function bodies in the Roblox web engine module.

The module is compiled with socket support absent: the libc entry points are
stubs that set errno and return -1 (see docs/FINDINGS.md). We cannot add
imports to a compiled module, so the only way to give it a network is to
replace those stub bodies with code that moves bytes through ring buffers in
linear memory.

Everything here is byte-level on purpose: it must survive a weekly rebuild,
so patches are keyed by function index resolved from a shape/behaviour scan,
never by file offset.

usage:
    patch.py in.wasm out.wasm --patch getaddrinfo
    patch.py in.wasm out.wasm --patch getaddrinfo --patch nosocket
"""

import sys

# ---------------------------------------------------------------- leb128 ----

def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(v, bits=32):
    out = bytearray()
    more = True
    while more:
        b = v & 0x7F
        v >>= 7
        sign = b & 0x40
        if (v == 0 and not sign) or (v == -1 and sign):
            more = False
        else:
            b |= 0x80
        out.append(b)
    return bytes(out)


def read_uleb(buf, pos):
    val = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


# ------------------------------------------------------------- assembler ----

class Asm:
    """Tiny wasm instruction assembler. Enough for stub bodies, nothing more."""

    def __init__(self):
        self.b = bytearray()

    def local_get(self, i):        self.b += bytes([0x20]) + uleb(i); return self
    def local_set(self, i):        self.b += bytes([0x21]) + uleb(i); return self
    def local_tee(self, i):        self.b += bytes([0x22]) + uleb(i); return self
    def i32_const(self, v):        self.b += bytes([0x41]) + sleb(v); return self
    def i32_load(self, align=2, off=0):   self.b += bytes([0x28]) + uleb(align) + uleb(off); return self
    def i32_store(self, align=2, off=0):  self.b += bytes([0x36]) + uleb(align) + uleb(off); return self
    def i32_store16(self, align=1, off=0): self.b += bytes([0x3b]) + uleb(align) + uleb(off); return self
    def i32_store8(self, align=0, off=0):  self.b += bytes([0x3a]) + uleb(align) + uleb(off); return self
    def i32_add(self):             self.b += b"\x6a"; return self
    def i32_eqz(self):             self.b += b"\x45"; return self
    def i32_eq(self):              self.b += b"\x46"; return self
    def call(self, fi):            self.b += bytes([0x10]) + uleb(fi); return self
    def drop(self):                self.b += b"\x1a"; return self
    def if_(self):                 self.b += b"\x04\x40"; return self
    def else_(self):               self.b += b"\x05"; return self
    def end(self):                 self.b += b"\x0b"; return self
    def ret(self):                 self.b += b"\x0f"; return self
    def nop(self):                 self.b += b"\x01"; return self
    def raw(self, h):              self.b += bytes.fromhex(h.replace(" ", "")); return self

    def body(self, locals_=()):
        """locals_: [(count, valtype_byte), ...]"""
        out = bytearray(uleb(len(locals_)))
        for cnt, ty in locals_:
            out += uleb(cnt) + bytes([ty])
        out += self.b
        if not out.endswith(b"\x0b"):
            out += b"\x0b"
        return bytes(out)


I32 = 0x7F

# ---------------------------------------------------------------- module ----

def sections(buf):
    if buf[:4] != b"\x00asm":
        raise SystemExit("not a wasm module")
    pos = 8
    out = []
    while pos < len(buf):
        sid = buf[pos]
        pos += 1
        size, pos = read_uleb(buf, pos)
        out.append((sid, pos, size))
        pos += size
    return out


def code_bodies(buf, code_start, code_size):
    """Return (count, [(body_off, body_len), ...])."""
    pos = code_start
    count, pos = read_uleb(buf, pos)
    bodies = []
    for _ in range(count):
        ln, pos = read_uleb(buf, pos)
        bodies.append((pos, ln))
        pos += ln
    if pos != code_start + code_size:
        raise SystemExit("code section length mismatch")
    return count, bodies


def find_code(buf):
    """Return (section_id_offset, payload_start, payload_size)."""
    pos = 8
    while pos < len(buf):
        id_off = pos
        sid = buf[pos]
        pos += 1
        size, pos = read_uleb(buf, pos)
        if sid == 10:
            return id_off, pos, size
        pos += size
    raise SystemExit("no code section")


def count_imported_funcs(buf):
    for sid, start, size in sections(buf):
        if sid != 2:
            continue
        pos = start
        n, pos = read_uleb(buf, pos)
        funcs = 0
        for _ in range(n):
            ln, pos = read_uleb(buf, pos)
            pos += ln
            ln, pos = read_uleb(buf, pos)
            pos += ln
            kind = buf[pos]
            pos += 1
            if kind == 0:
                _, pos = read_uleb(buf, pos)
                funcs += 1
            elif kind in (1, 2):
                flags = buf[pos]
                pos += 1
                _, pos = read_uleb(buf, pos)
                if flags & 1:
                    _, pos = read_uleb(buf, pos)
            elif kind == 3:
                pos += 2
            elif kind == 4:
                _, pos = read_uleb(buf, pos)
            else:
                raise SystemExit("unknown import kind %#x" % kind)
        return funcs
    return 0


def replace_functions(buf, new_bodies):
    """new_bodies: {func_index: body_bytes}. Returns patched module bytes."""
    id_off, code_start, code_size = find_code(buf)
    count, bodies = code_bodies(buf, code_start, code_size)

    payload = bytearray()
    payload += uleb(count)
    for i, (off, ln) in enumerate(bodies):
        body = new_bodies.get(i, None)
        if body is None:
            body = buf[off:off + ln]
        payload += uleb(len(body))
        payload += body

    out = bytearray(buf[:id_off])
    out.append(10)
    out += uleb(len(payload))
    out += payload
    out += buf[code_start + code_size:]
    return bytes(out)


# ---------------------------------------------------------------- patches ----
# Function indices are resolved by tools/wasm_scan.py + shape analysis.
# Keep them in one place so a rebuild only needs a re-resolve pass.

MALLOC = 163490          # exports: func[163490] <malloc>
GETADDRINFO = 301        # (i32)->i32, writes NULL to *arg and returns EAI_FAIL

# Control block: the bridge writes the peer address here before the join.
# 0x?? chosen inside the module's existing data; see tools/anchors.md.
CTL_ADDR = 0


def patch_getaddrinfo():
    """Replace the EAI_FAIL stub with a synthetic IPv4 addrinfo.

    struct addrinfo (wasm32): flags, family, socktype, protocol, addrlen,
                              addr, canonname, next  = 32 bytes
    struct sockaddr_in:       family(2) port(2) addr(4) pad(8) = 16 bytes
    """
    a = Asm()
    # ai = malloc(48); sa = ai + 32
    a.i32_const(48).call(MALLOC).local_set(1)
    a.local_get(1).i32_const(32).i32_add().local_set(2)

    # if (arg) *arg = ai
    a.local_get(0).if_()
    a.local_get(0).local_get(1).i32_store(2, 0)
    a.end()

    # addrinfo fields
    for off, val in ((0, 0), (4, 2), (8, 2), (12, 17), (16, 16), (24, 0), (28, 0)):
        a.local_get(1).i32_const(val).i32_store(2, off)
    a.local_get(1).local_get(2).i32_store(2, 20)      # ai_addr = sa

    # sockaddr_in: AF_INET, port 9, 127.0.0.1
    a.local_get(2).i32_const(2).i32_store16(1, 0)
    a.local_get(2).i32_const(0x0900).i32_store16(1, 2)   # port 9, big endian
    a.local_get(2).i32_const(0x0100007F).i32_store(2, 4)  # 127.0.0.1
    a.local_get(2).i32_const(0).i32_store(2, 8)
    a.local_get(2).i32_const(0).i32_store(2, 12)

    a.i32_const(0)
    return a.body([(2, I32)])


ENOSYS_STUB = 299        # ()->i32, sets errno=ENOSYS and returns -1


def patch_nosocket():
    """Make the ENOSYS catch-all report success, to find the next wall.

    Diagnostic only: socket()/bind()/setsockopt() then return 0, sendto() claims
    to have sent, recvfrom() claims an empty datagram.
    """
    a = Asm()
    a.i32_const(0)
    return a.body()


EBADF_STUB = 298         # ()->i32, sets errno=EBADF and returns -1


def patch_nofd():
    """Make the EBADF fd-validator report success (diagnostic).

    musl's setsockopt()/bind() reject our unregistered fd with EBADF before the
    syscall ever runs. Reporting success shows what RakNet does next.
    """
    a = Asm()
    a.i32_const(0)
    return a.body()


# Byte-level edits inside an existing body: (func_index, pattern_hex, replacement_hex).
# Used when the change is "neutralise one instruction", not "replace a function".
BYTE_PATCHES = {
    # func[127810] checks the socket's bound port against a global threshold and
    # aborts the join when it is too low. getsockname() writes nothing in this
    # build, so the port reads 0 and the check always trips. Replacing the
    # loaded threshold with 0 turns `if (port < threshold)` into `if (port < 0)`,
    # which can never fire.
    "portsub": (127810, "41ace5f203280200", "4100"),
}

PATCHES = {
    "getaddrinfo": (GETADDRINFO, patch_getaddrinfo),
    "nosocket": (ENOSYS_STUB, patch_nosocket),
    "nofd": (EBADF_STUB, patch_nofd),
}


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    names = [argv[i + 1] for i in range(len(argv) - 1) if argv[i] == "--patch"]
    buf = open(src, "rb").read()
    imported = count_imported_funcs(buf)
    changed = {}
    byte_names = [argv[i + 1] for i in range(len(argv) - 1) if argv[i] == "--byte-patch"]
    for name in byte_names:
        if name not in BYTE_PATCHES:
            raise SystemExit("unknown byte patch %r" % name)
        idx, pat_hex, rep_hex = BYTE_PATCHES[name]
        pat = bytes.fromhex(pat_hex)
        rep = bytes.fromhex(rep_hex)
        code_start, code_size = find_code(buf)[1], find_code(buf)[2]
        count, bodies = code_bodies(buf, code_start, code_size)
        off, ln = bodies[idx - imported]
        body = buf[off:off + ln]
        n = body.count(pat)
        if n == 0:
            raise SystemExit("byte patch %r: pattern not found in func[%d]" % (name, idx))
        changed[name] = body.replace(pat, rep)
        print("byte-patch %-10s func[%d]: %d site(s), %d -> %d B"
              % (name, idx, n, len(body), len(changed[name])))
    print("imported funcs: %d" % imported)
    new = {}
    for n in names:
        if n not in PATCHES:
            raise SystemExit("unknown patch %r (have: %s)" % (n, ", ".join(PATCHES)))
        idx, fn = PATCHES[n]
        if idx < imported:
            raise SystemExit("patch %r targets an imported function" % n)
        defined = idx - imported
        new[defined] = fn()
        print("patch %-12s -> func[%d] (defined #%d) body %d B"
              % (n, idx, defined, len(new[defined])))
    for name in byte_names:
        idx, _, _ = BYTE_PATCHES[name]
        new[idx - imported] = changed[name]
    out = replace_functions(buf, new)
    open(dst, "wb").write(out)
    print("wrote %s (%d -> %d bytes)" % (dst, len(buf), len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
