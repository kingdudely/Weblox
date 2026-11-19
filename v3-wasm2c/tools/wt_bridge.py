#!/usr/bin/env python3
"""wt_bridge.py - one RbxTransport/WebTransport session, framed over stdio.

The host's `rbx_wasm_rna_*` imports drive this process; it is the browser's
`new WebTransport(url)` in another address space - with the one difference the
game edge needs: every UDP datagram is RUPP-wrapped.  RUPP is applied at the
datagram layer, so it also covers the QUIC handshake, exactly like the native
client and the WebLox ring.

    stdin  commands (binary, newline-framed):
           C <port> <url>\n        connect (url is the engine's /wt?... URL)
           D <n>\n<n>              datagram
           S <sid>\n               open a client-unidirectional stream
           W <sid> <n>\n<n>        write on it
           F <sid>\n               fin
           X\n                     close
    stdout events:
           R\n                     connect ready (CONNECT 200)
           E <msg>\n               connect failed
           D <n>\n<n>              datagram
           A <sid>\n               server opened a stream
           S <sid> <n>\n<n>        stream data
           F <sid>\n               stream fin
           X <code>\n              session closed

Join fields come in `WT_JOIN` (the joinScript).  The dial target is always the
native UDMUX address; the SNI is the derived browser hostname, whose hex
labels carry the RCC address and port.
"""
import asyncio
import base64
import json
import os
import ssl
import struct
import sys

from aioquic.buffer import Buffer
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import DatagramReceived, HeadersReceived, WebTransportStreamDataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted

OUT = sys.stdout.buffer
state = {"conn": None, "h3": None, "wt": None, "ready": False, "hs": False, "next_sid": 3, "proto": None}
streams = {}

JS = json.loads(os.environ.get("WT_JOIN") or "{}")
RCC = (JS.get("ServerConnections") or [{}])[0]
RCC_IP = RCC.get("Address") or JS.get("MachineAddress") or "0.0.0.0"
RCC_PORT = int(JS.get("NetStackPort") or 0)
DATA_PORT = int(RCC.get("Port") or JS.get("ServerPort") or 0)
UDMUX = (JS.get("UdmuxEndpoints") or [{}])[0].get("Address") or "0.0.0.0"
JOIN_TOK = base64.b64decode(JS.get("NetStackTokenValue") or JS.get("TokenValue") or "")
SESSION = None


def dbg(*a):
    if os.environ.get("WT_DEBUG"):
        print("[wt]", *a, file=sys.stderr, flush=True)


def emit(b):
    OUT.write(b); OUT.flush()


def tlv_block(sub):
    addr = bytes(int(x) for x in RCC_IP.split("."))
    port = DATA_PORT if sub == 2 and DATA_PORT else RCC_PORT
    tok = SESSION if (sub == 2 and SESSION) else JOIN_TOK
    tlv1 = bytes([1, 17, sub]) + tok
    tlv2 = bytes([2, 6]) + addr + struct.pack(">H", port)
    body = tlv1 + tlv2
    return bytes([1, 0]) + struct.pack(">H", 4 + len(body)) + body


USE_RUPP = os.environ.get("WT_RUPP", "0") != "0"


def rupp_wrap(d):
    if not USE_RUPP:
        return d
    return tlv_block(2 if SESSION else 1) + d


def rupp_learn(d):
    global SESSION
    if len(d) < 8 or d[0] != 1 or d[1] != 0:
        return
    end = (d[2] << 8) | d[3]
    if end < 4 or end > len(d):
        return
    p = 4
    while p + 2 <= end:
        k, ln = d[p], d[p + 1]
        if p + 2 + ln > end:
            return
        if k == 1 and ln == 17 and d[p + 2] == 2:
            SESSION = d[p + 3:p + 19]
            return
        p += 2 + ln


def rupp_strip(d):
    if not USE_RUPP:
        return d
    if len(d) < 8 or d[0] != 1 or d[1] != 0:
        return d
    end = (d[2] << 8) | d[3]
    if end < 4 or end > len(d):
        return d
    return d[end:]


class Proto(asyncio.DatagramProtocol):
    def __init__(self, conn):
        self.conn = conn
        self.transport = None
        self.tick = None
        state["proto"] = self

    def connection_made(self, transport):
        self.transport = transport
        self.schedule()

    def schedule(self):
        loop = asyncio.get_event_loop()
        try:
            t = self.conn.get_timer()
        except Exception:
            t = None
        delay = 0.01 if t is None else max(0.0, t - loop.time())
        self.tick = loop.call_later(min(max(delay, 0.002), 0.05), self.on_timer)

    def on_timer(self):
        dbg("tick")
        self.pump()
        self.schedule()

    def datagram_received(self, data, addr):
        st = rupp_strip(data)
        if st[:1] == b"\xf0":
            from aioquic.quic.packet import get_retry_integrity_tag
            dcid = st[6:6 + st[5]]
            tag = st[-16:]
            calc = get_retry_integrity_tag(st[:-16], self.conn._peer_cid.cid, version=1)
            dbg("retry dcid", dcid.hex(), "host", self.conn.host_cid.hex(), "tagok", tag == calc,
                "is_client", self.conn._is_client)
            def _dbg_retry(fn, *a, **kw):
                h = kw.get("header")
                print("[rp]", self.conn._is_client, self.conn._retry_count,
                      h.destination_cid == self.conn.host_cid,
                      flush=True, file=sys.stderr)
                return fn(*a, **kw)
            import types
            if not hasattr(self.conn, "_dbgpatched"):
                self.conn._dbgpatched = True
                self.conn._receive_retry_packet = types.MethodType(
                    lambda me, *a, **kw: _dbg_retry(type(me).__mro__[0]._orig if False else __import__("aioquic.quic.connection", fromlist=["QuicConnection"]).QuicConnection._receive_retry_packet, me, *a, **kw),
                    self.conn)
            from aioquic.quic.packet import pull_quic_header
            try:
                h = pull_quic_header(Buffer(data=st), host_cid_length=len(self.conn.host_cid))
                dbg("parsed dcid", h.destination_cid.hex(), "type", h.packet_type,
                    "tag", h.integrity_tag.hex() == tag.hex())
            except Exception as ex:
                dbg("parse fail", repr(ex))
        dbg("rx", len(data))
        if state["hs"]:
            rupp_learn(data)
        try:
            self.conn.receive_datagram(st, addr, asyncio.get_event_loop().time())
        except Exception as e:
            emit(b"E handle %s\n" % str(e).encode()[:120])
        self.pump()
        dbg("after", self.conn._retry_count, len(self.conn._peer_token or b""))

    def pump(self):
        if not self.transport:
            return
        now = asyncio.get_event_loop().time()
        try:
            self.conn.handle_timer(now)
        except Exception:
            pass
        out = self.conn.datagrams_to_send(now)
        if out:
            for d, _ in out:
                try:
                    from aioquic.quic.packet import pull_quic_header as _ph
                    _h = _ph(Buffer(data=d), host_cid_length=8)
                    dbg("txpkt", _h.packet_type, "dcid", _h.destination_cid.hex(), "scid", _h.source_cid.hex(), "tok", len(_h.token or b""))
                except Exception as _e:
                    dbg("txpkt?", repr(_e)[:60])
        for d, addr in out:
            self.transport.sendto(rupp_wrap(d), addr)
        while (e := self.conn.next_event()) is not None:
            self.on_event(e)
        state["in_pump"] = False
        if state.pop("want_wt", False) and not state["h3"]:
            self.start_wt()

    def on_event(self, e):
        if isinstance(e, HandshakeCompleted):
            dbg("handshake complete")
            state["hs"] = True
            state["want_wt"] = True
        dbg("event", type(e).__name__, getattr(e, "error_code", ""))
        if isinstance(e, ConnectionTerminated):
            dbg("TERM t=%.2f" % (asyncio.get_event_loop().time() % 1000),
                e.error_code, "frame", getattr(e, "frame_type", ""),
                "reason", getattr(e, "reason_phrase", b"")[:80],
                "peer_idle", self.conn._remote_max_idle_timeout)
            emit(b"X %d\n" % e.error_code)
            if self.tick:
                self.tick.cancel()
            return
        h3 = state["h3"]
        if h3 is None:
            return
        for he in h3.handle_event(e):
            dbg("h3", type(he).__name__, getattr(he, "stream_id", ""))
            if isinstance(he, HeadersReceived):
                if he.stream_id == state["wt"]:
                    st = dict(he.headers).get(b":status")
                    if st == b"200":
                        state["ready"] = True
                        emit(b"R\n")
                    else:
                        emit(b"E status %s\n" % (st or b"?"))
            elif isinstance(he, DatagramReceived):
                d = rupp_strip(he.data)
                emit(b"D %d\n" % len(d)); emit(d)
            elif isinstance(he, WebTransportStreamDataReceived):
                sid = streams.get(he.stream_id)
                if sid is None:
                    sid = state["next_sid"]; state["next_sid"] += 4
                    streams[he.stream_id] = sid
                    emit(b"A %d\n" % sid)
                if he.data:
                    emit(b"S %d %d\n" % (sid, len(he.data))); emit(he.data)
                if he.end_stream:
                    emit(b"F %d\n" % sid); streams.pop(he.stream_id, None)
                self.pump()

    def start_wt(self):
        host, port, path = state["url"]
        state["h3"] = H3Connection(self.conn, enable_webtransport=True)
        state["wt"] = self.conn.get_next_available_stream_id()
        dbg("CONNECT sid", state["wt"], "host", host, "path", path[:40])
        try:
            state["h3"].send_headers(state["wt"], [
            (b":scheme", b"https"),
            (b":method", b"CONNECT"),
            (b":authority", ("%s:%d" % (host, port)).encode()),
            (b":path", path.encode()),
            (b":protocol", b"webtransport"),
            (b"sec-webtransport-http3-draft02", b"1"),
            (b"origin", b"https://play.roblox.com"),
            ])
        except Exception as ex:
            dbg("CONNECT failed", repr(ex)[:120])
        self.pump()
        if "ping" not in state:
            async def ping():
                while state.get("conn") and not state["conn"]._state.name.endswith("CLOSED"):
                    try:
                        state["conn"].send_ping(0)
                        dbg("ping")
                        self.pump()
                    except Exception as ex:
                        dbg("ping fail", repr(ex)[:80])
                    await asyncio.sleep(1.0)
            state["ping"] = asyncio.ensure_future(ping())

    def send_datagram(self, data):
        h3 = state["h3"]
        h3.send_datagram(state["wt"], data)
        self.pump()

    def open_stream(self, sid):
        qid = state["h3"].create_webtransport_stream(state["wt"], is_unidirectional=True)
        streams[qid] = sid
        self.pump()

    def write(self, sid, data, fin=False):
        for qid, s in list(streams.items()):
            if s == sid:
                self.conn.send_stream_data(qid, data, fin)
                self.pump()
                if fin:
                    streams.pop(qid, None)
                return


def parse_url(url):
    rest = url.split("://", 1)[1]
    hostport, _, path = rest.partition("/")
    path = "/" + path
    if ":" in hostport:
        host, _, port = hostport.rpartition(":")
        port = int(port)
    else:
        host, port = hostport, 443
    return host, port, path


async def do_connect(line):
    _, port, url = line.split(b" ", 2)
    host, uport, path = parse_url(url.decode())
    port = int(port) or uport
    loop = asyncio.get_event_loop()
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["h3"], server_name=host)
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535
    cfg.idle_timeout = 60.0
    conn = QuicConnection(configuration=cfg)
    state["conn"] = conn
    dest = UDMUX if USE_RUPP else target_ip(host)
    try:
        _, proto = await loop.create_datagram_endpoint(lambda: Proto(conn), remote_addr=(dest, port))
    except Exception as e:
        emit(b"E %s\n" % str(e).encode()[:160]); return
    state["url"] = (host, port, path)
    conn.connect((dest, port), loop.time())
    proto.pump()


async def main():
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin.buffer)
    while True:
        line = await reader.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        op = line[:1]; p = state.get("proto")
        if op == b"C":
            await do_connect(line)
        elif op == b"D" and p:
            p.send_datagram(await reader.readexactly(int(line[2:])))
        elif op == b"S" and p:
            p.open_stream(int(line[2:]))
        elif op == b"W" and p:
            _, sid, n = line.split(b" ")
            p.write(int(sid), await reader.readexactly(int(n)))
        elif op == b"F" and p:
            p.write(int(line[2:]), b"", True)
        elif op == b"X":
            if p:
                p.conn.close(0, b"")
                p.pump()
            break


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
