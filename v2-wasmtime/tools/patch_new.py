#!/usr/bin/env python3
"""patch_new.py — socket-patch the NEW Roblox web engine build.

Adapted from Weblox's tools/patch.py. Only the resolved indices and the two
transport splice sites changed; the ring ABI and stub bodies are identical.

Target build: version-1ef59424f70d4932 (/home/john/rr/WebClient.wasm,
102268042 bytes, imported funcs 341, defined funcs 154550, total 154891).

Verified absolute function indices (imports included):
    EBADF stub        395   () -> i32, errno=8
    ENOSYS stub       397   () -> i32, errno=138
    getaddrinfo       398   (i32) -> i32, returns EAI_FAIL (124)
    malloc export     150325
    udp_send          117578 (i32,i32,i32,i32) -> i32
    recv loop         117378 (i32) -> i32, body 6809 B
    port check        117374
    UpdateNetworkLoop 117377 (i32) -> i32
    RakPeer::Connect  117465 (10 i32) -> i32
    appended stubs    154891 select, 154892 recv

Everything is byte-level: patches are keyed on exact stub bodies and unique
byte patterns at known offsets *inside function bodies*, never on file
offsets. Every assertion prints what it checked; see README.md.

usage:
    # everything, with ring ABI JSON for the page bridge:
    patch_new.py in.wasm out.wasm --all --abi abi.json

    # or pick patches individually:
    patch_new.py in.wasm out.wasm \
        --patch getaddrinfo --patch nosocket --patch nofd \
        --patch udp_send --byte-patch portsub --transport --stash-peer
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
    def i64_const(self, v):        self.b += bytes([0x42]) + sleb(v); return self
    def i32_load(self, align=2, off=0):   self.b += bytes([0x28]) + uleb(align) + uleb(off); return self
    def i32_load8_u(self, align=0, off=0):  self.b += bytes([0x2D]) + uleb(align) + uleb(off); return self
    def i32_load16_u(self, align=1, off=0): self.b += bytes([0x2F]) + uleb(align) + uleb(off); return self
    def i32_and(self):             self.b += b"\x71"; return self
    def i32_mul(self):             self.b += b"\x6c"; return self
    def i32_sub(self):             self.b += b"\x6b"; return self
    def i32_ne(self):              self.b += b"\x47"; return self
    def i32_lt_u(self):            self.b += b"\x49"; return self
    def i32_gt_u(self):            self.b += b"\x4b"; return self
    def i32_ge_u(self):            self.b += b"\x4f"; return self
    def select_(self):             self.b += b"\x1b"; return self
    def memory_copy(self):
        self.b += bytes([0xFC, 0x0A, 0x00, 0x00]); return self
    def atomic_load(self, align=2, off=0):
        self.b += bytes([0xFE, 0x10]) + uleb(align) + uleb(off); return self
    def atomic_store(self, align=2, off=0):
        self.b += bytes([0xFE, 0x17]) + uleb(align) + uleb(off); return self
    def atomic_rmw_add(self, align=2, off=0):
        self.b += bytes([0xFE, 0x1E]) + uleb(align) + uleb(off); return self
    def atomic_wait32(self, align=2, off=0):
        self.b += bytes([0xFE, 0x01]) + uleb(align) + uleb(off); return self
    def atomic_notify(self, align=2, off=0):
        self.b += bytes([0xFE, 0x00]) + uleb(align) + uleb(off); return self
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


def defined_func_count(buf):
    for sid, start, size in sections(buf):
        if sid != 3:
            continue
        n, _ = read_uleb(buf, start)
        return n
    return 0


# ------------------------------------------------------------ assertions ----
# Every index in this file came from a one-off scan of the exact build named
# in the module docstring. Fail loudly (never silently patch the wrong site)
# if a different build is fed to the tool.

def check(label, ok, detail=""):
    if not ok:
        raise SystemExit("ASSERT FAILED: %s%s" % (label, ": " + detail if detail else ""))
    print("assert %-39s ok%s" % (label, " (%s)" % detail if detail else ""))


def assert_body(buf, idx, hex_body, label):
    _, _, body = func_body(buf, idx)
    want = bytes.fromhex(hex_body.replace(" ", ""))
    check(label, body == want, "func[%d] = %s" % (idx, body.hex()))


def assert_sig(buf, idx, params_hex, results_hex):
    imported = count_imported_funcs(buf)
    _, start, _ = find_section(buf, 3)
    pos = start
    n, pos = read_uleb(buf, pos)
    want_off = idx - imported
    t = None
    for j in range(n):
        t, pos = read_uleb(buf, pos)
        if j == want_off:
            break
    else:
        raise SystemExit("no func[%d] in function section" % idx)
    p, r = type_section(buf)[t]
    check("signature func[%d]" % idx,
          p.hex() == params_hex and r.hex() == results_hex,
          "type#%d (%s)->(%s)" % (t, p.hex(), r.hex()))


def assert_export_func(buf, name, idx):
    for sid, start, size in sections(buf):
        if sid != 7:
            continue
        pos = start
        n, pos = read_uleb(buf, pos)
        for _ in range(n):
            ln, pos = read_uleb(buf, pos)
            nm = buf[pos:pos + ln]
            pos += ln
            kind = buf[pos]
            pos += 1
            v, pos = read_uleb(buf, pos)
            if nm == name.encode():
                check("export %s" % name, kind == 0 and v == idx,
                      "kind %d func[%d]" % (kind, v))
                return
    raise SystemExit("no export named %s" % name)


def assert_import_func(buf, idx, module, field):
    for sid, start, size in sections(buf):
        if sid != 2:
            continue
        pos = start
        n, pos = read_uleb(buf, pos)
        fidx = 0
        for _ in range(n):
            ml, p = read_uleb(buf, pos)
            mod = buf[p:p + ml]
            p += ml
            fl, p = read_uleb(buf, p)
            fld = buf[p:p + fl]
            p += fl
            kind = buf[p]
            p += 1
            if kind == 0:
                if fidx == idx:
                    check("import[%d]" % idx, mod == module.encode() and fld == field.encode(),
                          "%s.%s" % (mod.decode(), fld.decode()))
                    return
                _, p = read_uleb(buf, p)
                fidx += 1
            elif kind == 1:
                flags = buf[p]
                p += 1
                _, p = read_uleb(buf, p)
                if flags & 1:
                    _, p = read_uleb(buf, p)
            elif kind == 2:
                flags = buf[p]
                p += 1
                _, p = read_uleb(buf, p)
                if flags & 1:
                    _, p = read_uleb(buf, p)
            elif kind == 3:
                p += 2
            elif kind == 4:
                _, p = read_uleb(buf, p)
            else:
                raise SystemExit("unknown import kind %#x" % kind)
            pos = p
    raise SystemExit("no function import[%d]" % idx)


def assert_site(buf, idx, pat_hex, expected_off, label):
    _, _, body = func_body(buf, idx)
    pat = bytes.fromhex(pat_hex)
    offs = [i for i in range(len(body)) if body.startswith(pat, i)]
    check("%s unique" % label, len(offs) == 1,
          "func[%d] count %d at %s" % (idx, len(offs), [hex(x) for x in offs]))
    check("%s offset" % label, offs[0] == expected_off,
          "%#x (want %#x)" % (offs[0], expected_off))


def type_section(buf):
    """Return [(params_bytes, results_bytes)] in type-index order."""
    for sid, start, size in sections(buf):
        if sid != 1:
            continue
        pos = start
        n, pos = read_uleb(buf, pos)
        out = []
        for _ in range(n):
            if buf[pos] != 0x60:
                raise SystemExit("unexpected functype %#x" % buf[pos])
            pos += 1
            np_, pos = read_uleb(buf, pos)
            params = buf[pos:pos + np_]
            pos += np_
            nr, pos = read_uleb(buf, pos)
            results = buf[pos:pos + nr]
            pos += nr
            out.append((bytes(params), bytes(results)))
        return out
    return []


def find_type(buf, params, results):
    """Reuse an existing type index; adding one would shift nothing but is
    unnecessary — 273 imports already cover most shapes."""
    for i, (p, r) in enumerate(type_section(buf)):
        if p == params and r == results:
            return i
    raise SystemExit("no existing type %r -> %r; add one to type_section() first"
                     % (params, results))


def add_functions(buf, bodies):
    """Append functions at the end of the function/code sections.

    Appending never renumbers an existing function index, which is the whole
    reason we append instead of inserting. Returns (buf, [new_indices]).
    """
    type_idx = []
    for params, results, _ in bodies:
        type_idx.append(find_type(buf, params, results))

    first = count_imported_funcs(buf) + defined_func_count(buf)

    _, fstart, fsize = find_section(buf, 3)
    pos = fstart
    n, pos = read_uleb(buf, pos)
    payload = bytearray(uleb(n + len(bodies))) + buf[pos:fstart + fsize]
    for t in type_idx:
        payload += uleb(t)
    buf = replace_section(buf, 3, bytes(payload))

    _, cstart, csize = find_code(buf)
    pos = cstart
    n, pos = read_uleb(buf, pos)
    payload = bytearray(uleb(n + len(bodies)))
    end_of_count = pos
    # copy every existing body verbatim
    for _ in range(n):
        ln, p2 = read_uleb(buf, pos)
        payload += buf[pos:p2 + ln]
        pos = p2 + ln
    for _, _, body in bodies:
        payload += uleb(len(body)) + body
    buf = replace_section(buf, 10, bytes(payload))
    return buf, list(range(first, first + len(bodies)))


# ------------------------------------------------------------- transport ----
# The engine is compiled without sockets: sendto/recvfrom/select are a shared
# stub that sets errno and returns -1. We cannot add imports (appending a
# function import would renumber all 167k functions), so the transport lives in
# linear memory instead:
#
#   wasm --(TX ring)--> page --(WebSocket)--> tools/udp_bridge.js --> UDP
#   wasm <--(RX ring)-- page <--(WebSocket)--
#
# Emscripten builds pthread modules with `shared: true`, so the page and every
# engine worker already share one SharedArrayBuffer. content.js captures it by
# wrapping WebAssembly.Memory at document_start, then pumps the two rings.
#
# The ring sits at the top of the 2 GiB linear memory: dlmalloc grows up from
# __heap_base (global 0 initialises to 0x17152a0), so the last few hundred KiB
# are the only region nothing else will claim.

RING_BASE = 0x7FF00000
RING_MAGIC = RING_BASE + 0x00        # page writes 'RAKN' when its pump is live
RING_TX_HEAD = RING_BASE + 0x04      # wasm writes, page reads
RING_TX_TAIL = RING_BASE + 0x08      # page writes
RING_RX_HEAD = RING_BASE + 0x0C      # page writes
RING_RX_TAIL = RING_BASE + 0x10      # wasm writes
RING_WAKE = RING_BASE + 0x14         # futex word: page notifies, recv waits
RING_SELECTS = RING_BASE + 0x18      # counters, for probing liveness
RING_RECVD = RING_BASE + 0x1C
RING_ADDR = RING_BASE + 0x20         # 16-byte sockaddr captured from SendTo
RING_PEER = RING_BASE + 0x30         # RakPeer* observed by UpdateNetworkLoop
RING_UPDATES = RING_BASE + 0x34      # UpdateNetworkLoop iterations
RING_CREQ = RING_BASE + 0x38         # page sets 1 to ask for a RakPeer::Connect
RING_CRES = RING_BASE + 0x3C         # result of that Connect
RING_HOST = RING_BASE + 0x40         # NUL-terminated ASCII IPv4, 48 bytes
RING_PORT = RING_BASE + 0x70         # u32 port
RING_SLOT = 1536                     # u32 len + 1500 payload, 4-byte aligned
RING_N = 64                          # power of two; index is masked
RING_TX0 = RING_BASE + 0x80
RING_RX0 = RING_TX0 + RING_N * RING_SLOT
RING_END = RING_RX0 + RING_N * RING_SLOT

MAX_DATAGRAM = 1500

# The page must not hardcode a second copy of the layout above. `--abi` writes
# the offsets out as JSON; rkhost injects it into the served page.
def ring_abi():
    return {
        "base": RING_BASE,
        "magic": RING_MAGIC - RING_BASE,
        "txHead": RING_TX_HEAD - RING_BASE,
        "txTail": RING_TX_TAIL - RING_BASE,
        "rxHead": RING_RX_HEAD - RING_BASE,
        "rxTail": RING_RX_TAIL - RING_BASE,
        "wake": RING_WAKE - RING_BASE,
        "selects": RING_SELECTS - RING_BASE,
        "recvd": RING_RECVD - RING_BASE,
        "addr": RING_ADDR - RING_BASE,
        "peer": RING_PEER - RING_BASE,
        "updates": RING_UPDATES - RING_BASE,
        "slot": RING_SLOT,
        "slots": RING_N,
        "tx0": RING_TX0 - RING_BASE,
        "rx0": RING_RX0 - RING_BASE,
        "maxDatagram": MAX_DATAGRAM,
    }


def patch_udp_send():
    """func[117578](buf, len, sockaddr, &err) -> i32: queue the datagram.

    RakNet's send wrapper still has every parameter a real sendto() needs, so
    this is the one place bytes leave the engine with a live buffer pointer. We
    append (sockaddr, len, payload) to the TX ring and report a full send; the
    page forwards it. The sockaddr is also parked in the ring header so the
    receive side can hand RakNet replies that appear to come from the peer it
    believes it is talking to.
    """
    a = Asm()
    a.local_get(3).i32_const(0).i32_store(2, 0)              # *err = 0
    a.i32_const(RING_ADDR).local_get(2).i32_const(16).memory_copy()

    # n = min(len, MAX_DATAGRAM)
    a.local_get(1).i32_const(MAX_DATAGRAM)
    a.local_get(1).i32_const(MAX_DATAGRAM).i32_lt_u()
    a.select_().local_set(4)

    # h = tx_head (single writer: RakNet's send path is serialised by its own lock)
    a.i32_const(RING_TX_HEAD).atomic_load(2, 0).local_set(5)
    a.i32_const(RING_TX0)
    a.local_get(5).i32_const(RING_N - 1).i32_and()
    a.i32_const(RING_SLOT).i32_mul().i32_add().local_set(6)

    a.local_get(6).i32_const(0).i32_store(2, 0)               # len = 0: not ready
    a.local_get(6).i32_const(4).i32_add()
    a.local_get(0).local_get(4).memory_copy()
    a.local_get(6).local_get(4).i32_store(2, 0)               # len last: publishes

    a.i32_const(RING_TX_HEAD)
    a.local_get(5).i32_const(1).i32_add().atomic_store(2, 0)
    a.local_get(4)
    return a.body([(3, I32)])


def fn_select():
    """select() replacement: optionally service connects, then report readable.

    The recv loop's only blocking point was select(); a bare `return 1` would
    spin a core. memory.atomic.wait32 is available because the module is built
    with shared memory, and the page wakes it with Atomics.notify when a
    datagram arrives, so the idle cost is one futex park.

    CONNECT_ON_SELECT also lets this stub issue RakPeer::Connect, because this
    thread is the only one we own that runs often and it already holds the peer
    pointer. Turn it off to let the engine's own NetworkClient drive RakNet.
    """
    a = Asm()
    a.i32_const(RING_SELECTS)
    a.i32_const(RING_SELECTS).atomic_load(2, 0).i32_const(1).i32_add()
    a.atomic_store(2, 0)

    if CONNECT_ON_SELECT:
        a.i32_const(RING_PEER).i32_load(2, 0).local_set(0)
        a.local_get(0).if_()
        a.i32_const(RING_CREQ).i32_load(2, 0).if_()
        a.i32_const(RING_CREQ).i32_const(0).i32_store(2, 0)
        a.local_get(0)
        a.i32_const(RING_HOST)
        a.i32_const(RING_PORT).i32_load(2, 0)
        # RakPeer::Connect(host, remotePort, passwordData, passwordDataLength,
        #                  remotePublicKey, timeOut, connectionAttempts,
        #                  connectionAttemptDelay, socketIndex[, mtu])
        for v in CONNECT_DEFAULTS:
            a.i32_const(v)
        a.i32_const(RING_CRES).i32_const(-12345).i32_store(2, 0)
        a.call(RAKPEER_CONNECT)
        a.local_set(1)
        a.i32_const(RING_CRES).local_get(1).i32_store(2, 0)
        a.end()
        a.end()

    a.i32_const(RING_RX_HEAD).atomic_load(2, 0)
    a.i32_const(RING_RX_TAIL).atomic_load(2, 0)
    a.i32_ne().if_().i32_const(1).ret().end()

    a.i32_const(RING_WAKE).i32_const(0).i64_const(1000000)
    a.atomic_wait32(2, 0).drop()
    a.i32_const(1)
    return a.body([(2, I32)])


def fn_recv():
    """func[154892] splice target: (dest, this) -> length, called from func[117378].

    (dest, this) -> n. Copies one queued datagram into `dest` (the packet object
    RakNet's RecvFromLoop allocated for exactly this purpose), copies the peer
    sockaddr into `this+232` where the surrounding code expects recvfrom to have
    left it, and returns the length. The untouched tail of func[117378] then
    stores the length, stamps the receive time, and parses the address — exactly
    as it would for a real recvfrom.
    """
    a = Asm()
    a.i32_const(RING_RX_TAIL).atomic_load(2, 0).local_set(2)
    a.i32_const(RING_RX_HEAD).atomic_load(2, 0).local_set(3)
    a.local_get(2).local_get(3).i32_eq().if_().i32_const(0).ret().end()

    a.i32_const(RING_RX0)
    a.local_get(2).i32_const(RING_N - 1).i32_and()
    a.i32_const(RING_SLOT).i32_mul().i32_add().local_set(4)

    a.local_get(4).i32_load(2, 0).local_set(5)
    a.local_get(5).i32_eqz().if_().i32_const(0).ret().end()
    a.local_get(5).i32_const(MAX_DATAGRAM - 8)
    a.local_get(5).i32_const(MAX_DATAGRAM - 8).i32_lt_u()
    a.select_().local_set(5)

    a.local_get(0).local_get(4).i32_const(4).i32_add()
    a.local_get(5).memory_copy()
    a.local_get(1).i32_const(232).i32_add()
    a.i32_const(RING_ADDR).i32_const(16).memory_copy()

    a.i32_const(RING_RX_TAIL)
    a.local_get(2).i32_const(1).i32_add().atomic_store(2, 0)
    a.i32_const(RING_RECVD)
    a.i32_const(RING_RECVD).atomic_load(2, 0).local_get(5).i32_add()
    a.atomic_store(2, 0)
    a.local_get(5)
    return a.body([(4, I32)])


UPDATE_LOOP = 117377


def patch_stash_peer():
    """Borrow func[117377] (RakNet::UpdateNetworkLoop) to publish its `this`.

    The web build never calls RakPeer::Connect, so the only way to drive the
    real stack is to call into it ourselves -- which needs the peer pointer.
    The update loop is handed exactly that pointer, so it records it here.
    Local 0, not a parameter read: this body is a prefix, so locals are intact.
    """
    a = Asm()
    a.i32_const(RING_PEER).local_get(0).i32_store(2, 0)
    a.i32_const(RING_UPDATES)
    a.i32_const(RING_UPDATES).atomic_load(2, 0).i32_const(1).i32_add()
    a.atomic_store(2, 0)
    return bytes(a.b)


# RakNet's connect, driven by us. The web build selects a dummy transport and
# never calls this, so the real RakPeer the engine started would otherwise sit
# idle. Signature is (this, host, port, ...) -> i32; the trailing arguments are
# passwords/public keys/socket index, none of which apply here.
RAKPEER_CONNECT = 117465
CONNECT_ARITY = 10
# args 3..10 of RakPeer::Connect, after (host, remotePort)
CONNECT_DEFAULTS = [0, 0, 0, 0, 12, 500, 0]
# Set false to let the engine drive RakNet itself; true splices a Connect into
# the recv loop's select() stub.
CONNECT_ON_SELECT = False

# Both sites are in the recv loop func[117378] (body 6809 B). The old call
# target 395 is the ()->i32 EBADF stub. At the recv site `20 02` is the
# outLen* address that stays on the stack for the following i32.store; the
# two extra values are the real arguments (dest = local1, this = local0).
# At the select site `37 03 c0 01` is an i64.store (no argument to select).
RECV_SITE = (117378, "2002108b03", 0x4EA)
SELECT_SITE = (117378, "3703c001108b03", 0x4C9)
EXPECTED_TOTAL = 154891
SELECT_NEW = 154891
RECV_NEW = 154892


def apply_transport(buf):
    """Splice the recv loop's select()/recvfrom() calls and append the stubs."""
    imported = count_imported_funcs(buf)
    total = imported + defined_func_count(buf)
    check("imported func count", imported == 341, "%d" % imported)
    check("module func count", total == EXPECTED_TOTAL, "%d" % total)

    sel_idx = total
    recv_idx = total + 1
    check("appended indices", (sel_idx, recv_idx) == (SELECT_NEW, RECV_NEW),
          "select func[%d], recv func[%d]" % (sel_idx, recv_idx))

    idx, pat_hex, off = RECV_SITE
    assert_site(buf, idx, pat_hex, off, "recv splice")
    pat = bytes.fromhex(pat_hex)
    target, _ = read_uleb(pat, 3)
    check("recv splice call target", target == EBADF_STUB, "func[%d]" % target)
    _, _, body = func_body(buf, idx)
    body = body.replace(
        pat, bytes.fromhex("2002") + bytes.fromhex("2001") + bytes.fromhex("2000")
        + b"\x10" + uleb(recv_idx))

    idx2, pat2_hex, off2 = SELECT_SITE
    assert_site(buf, idx2, pat2_hex, off2, "select splice")
    pat2 = bytes.fromhex(pat2_hex)
    target2, _ = read_uleb(pat2, 5)
    check("select splice call target", target2 == EBADF_STUB, "func[%d]" % target2)
    body = body.replace(pat2, pat2[:-3] + b"\x10" + uleb(sel_idx))

    buf = replace_functions(buf, {idx - imported: body})
    buf, new = add_functions(buf, [
        (b"", b"\x7f", fn_select()),
        (b"\x7f\x7f", b"\x7f", fn_recv()),
    ])
    print("transport        -> func[%d] select, func[%d] recv (ring @ %#x..%#x)"
          % (new[0], new[1], RING_BASE, RING_END))
    return buf


# ---------------------------------------------------------------- patches ----
# Function indices are resolved by shape/behaviour scan of the exact build in
# the module docstring. Keep them in one place; a rebuild needs a re-resolve.

MALLOC = 150325          # exports: func[150325] <malloc>, (i32)->i32

# Research note said 314 for emscripten_console_log; in this build wasm-tools
# shows import func[313] = env.emscripten_console_log and func[314] =
# env.emscripten_console_warn. Trace support is disabled below because the old
# MARKER_STR address (953724) was not re-resolved for this build.
CONSOLE_LOG = 313
MARKER_STR = None

GETADDRINFO = 398        # (i32)->i32, writes NULL to *arg and returns EAI_FAIL

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


ENOSYS_STUB = 397        # ()->i32, sets errno=138 and returns -1


def patch_nosocket():
    """Make the ENOSYS catch-all report success, to find the next wall.

    Diagnostic only: socket()/bind()/setsockopt() then return 0, sendto() claims
    to have sent, recvfrom() claims an empty datagram.
    """
    a = Asm()
    a.i32_const(0)
    return a.body()


EBADF_STUB = 395         # ()->i32, sets errno=8 and returns -1


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
    # func[117374] checks the socket's bound port against a global threshold
    # and aborts the join when it is too low. getsockname() writes nothing in
    # this build, so the port reads 0 and the check always trips. Replacing
    # the loaded threshold with 0 turns `if (port < threshold)` into
    # `if (port < 0)`, which can never fire. Unique at body+8641 (0x21c1).
    "portsub": (117374, "41c0e19304280200", "4100", 8641),
}

UDP_SEND = 117578        # (i32,i32,i32,i32)->i32, TX ring producer
RECV_LOOP = 117378       # (i32)->i32, body 6809 B, contains both splices
PORT_CHECK = 117374      # port threshold check, unique byte patch at +8641

PATCHES = {
    "getaddrinfo": (GETADDRINFO, patch_getaddrinfo),
    "udp_send": (UDP_SEND, patch_udp_send),
    "nosocket": (ENOSYS_STUB, patch_nosocket),
    "nofd": (EBADF_STUB, patch_nofd),
}

# Exact byte bodies of the three stubs we replace. These are the assertions
# that make an index-only re-resolve safe: if any body differs, stop.
STUB_BODIES = {
    EBADF_STUB: "00 10 b6 93 09 41 08 36 02 00 41 7f 0b",          # errno 8
    ENOSYS_STUB: "00 10 b6 93 09 41 8a 01 36 02 00 41 7f 0b",      # errno 138
    GETADDRINFO: "00 20 00 04 40 20 00 41 00 36 02 00 0b 41 7c 0b",
}
STUB_SIGS = {
    EBADF_STUB: ("", "7f"),
    ENOSYS_STUB: ("", "7f"),
    GETADDRINFO: ("7f", "7f"),
}


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    names = [argv[i + 1] for i in range(len(argv) - 1) if argv[i] == "--patch"]
    byte_names = [argv[i + 1] for i in range(len(argv) - 1) if argv[i] == "--byte-patch"]
    if "--all" in argv:
        names = list(PATCHES)
        byte_names = list(BYTE_PATCHES)
    buf = open(src, "rb").read()
    imported = count_imported_funcs(buf)

    # Preflight: refuse to patch a build whose indices/bodies do not match the
    # ones this file was resolved against.
    check("imported func count", imported == 341, "%d" % imported)
    check("module func count", imported + defined_func_count(buf) == EXPECTED_TOTAL,
          "%d" % (imported + defined_func_count(buf)))
    for idx, hx in STUB_BODIES.items():
        assert_body(buf, idx, hx, "stub body func[%d]" % idx)
    for idx, (p, r) in STUB_SIGS.items():
        assert_sig(buf, idx, p, r)
    assert_sig(buf, MALLOC, "7f", "7f")
    assert_sig(buf, UDP_SEND, "7f7f7f7f", "7f")
    assert_sig(buf, RECV_LOOP, "7f", "7f")
    assert_sig(buf, UPDATE_LOOP, "7f", "7f")
    assert_sig(buf, RAKPEER_CONNECT, "7f" * 10, "7f")
    assert_export_func(buf, "malloc", MALLOC)
    assert_import_func(buf, CONSOLE_LOG, "env", "emscripten_console_log")
    assert_import_func(buf, 314, "env", "emscripten_console_warn")
    for idx in (PORT_CHECK,):
        _, _, bd = func_body(buf, idx)
        check("func[%d] body len" % idx, len(bd) == 9093, "%d B" % len(bd))
    _, _, bd = func_body(buf, RECV_LOOP)
    check("func[%d] body len" % RECV_LOOP, len(bd) == 6809, "%d B" % len(bd))

    changed = {}
    for name in byte_names:
        if name not in BYTE_PATCHES:
            raise SystemExit("unknown byte patch %r" % name)
        idx, pat_hex, rep_hex, exp_off = BYTE_PATCHES[name]
        assert_site(buf, idx, pat_hex, exp_off, "byte patch %s" % name)
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
    if "--trace" in argv:
        raise SystemExit("--trace is disabled: MARKER_STR was not re-resolved "
                         "for the new build")
    transport = "--transport" in argv or "--all" in argv
    stash_peer = "--stash-peer" in argv or "--all" in argv
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
        idx, _, _, _ = BYTE_PATCHES[name]
        new[idx - imported] = changed[name]
    out = replace_functions(buf, new)
    if renames:
        out = rename_imports(out, renames)
        for i, n in sorted(renames.items()):
            print("import[%d] renamed -> %s" % (i, n))
    if transport:
        out = apply_transport(out)
    if stash_peer:
        out = replace_functions(out, {
            UPDATE_LOOP - count_imported_funcs(out): body_splice_prefix(
                func_body(out, UPDATE_LOOP)[2], patch_stash_peer())})
    open(dst, "wb").write(out)
    print("wrote %s (%d -> %d bytes)" % (dst, len(buf), len(out)))

    if "--abi" in argv:
        i = argv.index("--abi")
        path = argv[i + 1] if i + 1 < len(argv) else None
        if not path:
            raise SystemExit("--abi needs a path")
        import json
        with open(path, "w") as f:
            json.dump(ring_abi(), f, separators=(",", ":"))
        print("wrote %s (ring ABI)" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
