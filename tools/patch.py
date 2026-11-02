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


# ------------------------------------------------------------- sections ----

def find_section(buf, want):
    """Return (id_offset, payload_start, payload_len) for a section id."""
    pos = 8
    while pos < len(buf):
        id_off = pos
        sid = buf[pos]
        pos += 1
        size, pos = read_uleb(buf, pos)
        if sid == want:
            return id_off, pos, size
        pos += size
    raise SystemExit("no section %d" % want)


def replace_section(buf, sid, payload):
    """Rebuild one section with a new payload; everything else is copied."""
    id_off, start, size = find_section(buf, sid)
    out = bytearray(buf[:id_off])
    out.append(sid)
    out += uleb(len(payload))
    out += payload
    out += buf[start + size:]
    return bytes(out)


def rename_imports(buf, renames):
    """renames: {func_import_index: new_field_name}. Rebuilds section 2.

    Function import indices are positional, so renaming never shifts any
    function index — that is the whole reason we reuse imports instead of
    appending new ones (appending would renumber the other 167k functions).
    """
    id_off, start, size = find_section(buf, 2)
    pos = start
    count, pos = read_uleb(buf, pos)
    out = bytearray()
    out += uleb(count)
    fidx = 0
    for _ in range(count):
        mlen, pos2 = read_uleb(buf, pos)
        module = buf[pos2:pos2 + mlen]
        pos = pos2 + mlen
        flen, pos2 = read_uleb(buf, pos)
        field = buf[pos2:pos2 + flen]
        pos = pos2 + flen
        kind = buf[pos]
        entry = bytearray()
        entry += uleb(len(module)) + module
        if kind == 0 and fidx in renames:
            field = renames[fidx].encode()
        entry += uleb(len(field)) + field
        entry.append(kind)
        pos += 1
        if kind == 0:
            v, pos = read_uleb(buf, pos)
            entry += uleb(v)
            fidx += 1
        elif kind == 1:
            entry += buf[pos:pos + 1]
            pos += 1
            fl = buf[pos]
            entry += bytes([fl])
            pos += 1
            v, pos = read_uleb(buf, pos)
            entry += uleb(v)
            if fl & 1:
                v, pos = read_uleb(buf, pos)
                entry += uleb(v)
        elif kind == 2:
            fl = buf[pos]
            entry += bytes([fl])
            pos += 1
            v, pos = read_uleb(buf, pos)
            entry += uleb(v)
            if fl & 1:
                v, pos = read_uleb(buf, pos)
                entry += uleb(v)
        elif kind == 3:
            entry += buf[pos:pos + 2]
            pos += 2
        elif kind == 4:
            v, pos = read_uleb(buf, pos)
            entry += uleb(v)
        else:
            raise SystemExit("unknown import kind %#x" % kind)
        out += entry
    return replace_section(buf, 2, bytes(out))


def body_splice_prefix(body, prefix):
    """Insert instructions after a body's local declarations."""
    pos = 0
    ngroups, pos = read_uleb(body, pos)
    for _ in range(ngroups):
        _, pos = read_uleb(body, pos)
        pos += 1
    return body[:pos] + prefix + body[pos:]


def func_body(buf, idx):
    imported = count_imported_funcs(buf)
    _, code_start, code_size = find_code(buf)
    _, bodies = code_bodies(buf, code_start, code_size)
    off, ln = bodies[idx - imported]
    return off, ln, buf[off:off + ln]


# ---------------------------------------------------------------- patches ----
# Function indices are resolved by tools/wasm_scan.py + shape analysis.
# Keep them in one place so a rebuild only needs a re-resolve pass.

MALLOC = 163490          # exports: func[163490] <malloc>

# Two imports the web client never uses (Emscripten's websocket API — Studio
# networking only). Repurposed in place: renaming an existing import keeps every
# function index stable, which appending a new import would not.
WS_SEND_BINARY = 202     # (i32 sockid, i32 ptr, i32 len) -> i32
WS_CLOSE        = 201    # (i32 sockid, i32 code, i32 reason) -> i32

CONSOLE_LOG = 246        # env.emscripten_console_log(ptr) -> void, unused import
MARKER_STR  = 953724     # long log string; suffix at +k marks trace slot k
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


def patch_udp_send():
    """func[127931](buf, len, sockaddr, &err) -> i32: hand the datagram to JS.

    RakNet's send wrapper keeps exactly what a real sendto() needs, so the body
    becomes a call into the repurposed import. The glue reads the sockaddr out
    of linear memory itself, so only the pointer crosses the boundary.
    """
    a = Asm()
    a.local_get(3).i32_const(0).i32_store(2, 0)   # *err = 0
    a.local_get(2)                                 # sockaddr ptr
    a.local_get(0)                                 # buf
    a.local_get(1)                                 # len
    a.call(WS_SEND_BINARY)
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
    "udp_send": (127931, patch_udp_send),
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
    renames = {}
    for i in range(len(argv) - 2):
        if argv[i] == "--rename-import":
            renames[int(argv[i + 1])] = argv[i + 2]
    trace = [int(argv[i + 1]) for i in range(len(argv) - 1) if argv[i] == "--trace"]
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
    for k, idx in enumerate(trace):
        defined = idx - imported
        body = new.get(defined)
        if body is None:
            _, _, body = func_body(buf, idx)
        mark = Asm().i32_const(MARKER_STR + k).call(CONSOLE_LOG)
        new[defined] = body_splice_prefix(body, bytes(mark.b))
        print("trace[%2d]      -> func[%d] marker +%d" % (k, idx, k))
    out = replace_functions(buf, new)
    if renames:
        out = rename_imports(out, renames)
        for i, n in sorted(renames.items()):
            print("import[%d] renamed -> %s" % (i, n))
    open(dst, "wb").write(out)
    print("wrote %s (%d -> %d bytes)" % (dst, len(buf), len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
