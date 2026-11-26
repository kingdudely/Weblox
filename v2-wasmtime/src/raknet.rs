//! Host-side RakNet datagram seam.
//!
//! The patched guest has no sockets: RakNet's `sendto` copies a datagram into
//! a ring in shared linear memory and its `select`/`recvfrom` pair waits on
//! and drains a second ring. This module is the other end of those rings: one
//! native UDP socket, one thread pushing the TX ring to it, one thread pulling
//! replies into the RX ring. Nothing here knows about RakNet packets beyond
//! the three RUPP fields the join script owns (see `Rupp`).
//!
//! Layout (absolute offsets in the guest's 2 GiB shared memory):
//!
//! ```text
//! 0x7FF00000 + 0x00  magic 'RAKN'    0x04 tx head   0x08 tx tail
//!            + 0x0C  rx head        0x10 rx tail    0x14 wake (futex)
//!            + 0x20  16-byte sockaddr captured from the guest's sendto
//!            + 0x30  RakPeer*        0x34 updates    0x38 connect request
//!            + 0x3C  connect result  0x40 host[48]   0x70 port (u32)
//!            + 0x80  TX slots        + rx0 RX slots
//! ```
//!
//! Each slot is `u32 len` followed by the payload. The publisher writes the
//! payload before the length and advances the head/tail last, so a zero length
//! always means "not ready".
use std::net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use wasmtime::SharedMemory;

pub const RING_BASE: u32 = 0x7FF0_0000;
pub const MAGIC_VALUE: u32 = 0x4B52_414E; // the 'RAKN' word from ring.js

pub const MAGIC: u32 = 0x00;
pub const TX_HEAD: u32 = 0x04;
pub const TX_TAIL: u32 = 0x08;
pub const RX_HEAD: u32 = 0x0C;
pub const RX_TAIL: u32 = 0x10;
pub const WAKE: u32 = 0x14;
pub const SELECTS: u32 = 0x18;
pub const RECVD: u32 = 0x1C;
pub const ADDR: u32 = 0x20;
pub const PEER: u32 = 0x30;
pub const UPDATES: u32 = 0x34;
pub const CREQ: u32 = 0x38;
pub const CRES: u32 = 0x3C;
pub const HOST: u32 = 0x40;
pub const HOST_LEN: usize = 48;
pub const PORT: u32 = 0x70;
pub const TX0: u32 = 0x80;

/// The brief's ABI. `WC_RK_SLOT=1536` selects the slot stride used by the
/// Weblox `tools/patch.py` ABI (`abi.js` reports 1536); the default matches the
/// host-side seam specification this module was written against.
pub const SLOT: u32 = 1524;
pub const NSLOT: u32 = 64;
pub const MAX_DATAGRAM: usize = 1500;
pub const RX0: u32 = TX0 + NSLOT * SLOT;

static STARTED: AtomicBool = AtomicBool::new(false);
static STOP: AtomicBool = AtomicBool::new(false);
static TX_COUNT: AtomicU64 = AtomicU64::new(0);
static RX_COUNT: AtomicU64 = AtomicU64::new(0);
static DROP_COUNT: AtomicU64 = AtomicU64::new(0);
static ERROR_COUNT: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, Default)]
pub struct Stats {
    pub tx: u64,
    pub rx: u64,
    pub dropped: u64,
    pub errors: u64,
}

pub fn stats() -> Stats {
    Stats {
        tx: TX_COUNT.load(Ordering::Relaxed),
        rx: RX_COUNT.load(Ordering::Relaxed),
        dropped: DROP_COUNT.load(Ordering::Relaxed),
        errors: ERROR_COUNT.load(Ordering::Relaxed),
    }
}

/// Stop the pumps. Mostly useful for tests; a running game never calls it.
pub fn stop() {
    STOP.store(true, Ordering::Release);
}

fn debug() -> bool {
    std::env::var_os("WC_RAKNET_DBG").is_some()
}

// ------------------------------------------------------------------- ring ---

/// Guest-memory side of the seam. All accesses go through `SharedMemory`'s
/// `UnsafeCell` view, like the other host modules; the head/tail/length words
/// are read and written with real atomics so they match the guest's
/// `memory.atomic.*` instructions.
pub struct Ring {
    mem: SharedMemory,
    slot: u32,
    rx0: u32,
}

impl Ring {
    pub fn new(mem: SharedMemory) -> Self {
        let slot = std::env::var("WC_RK_SLOT")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(SLOT)
            .clamp(8, 64 * 1024);
        let rx0 = TX0 + NSLOT * slot;
        Self { mem, slot, rx0 }
    }

    pub fn slot(&self) -> u32 {
        self.slot
    }
    pub fn rx0(&self) -> u32 {
        self.rx0
    }

    fn word(&self, addr: u32) -> Option<&AtomicU32> {
        let a = addr as usize;
        let d = self.mem.data();
        if a % 4 != 0 || a + 4 > d.len() {
            return None;
        }
        // SAFETY: SharedMemory guarantees a suitably aligned, stable base;
        // the guest performs atomic accesses on the same words.
        unsafe { Some(&*(d[a].get() as *const u8).cast::<AtomicU32>()) }
    }

    pub fn load_at(&self, addr: u32) -> u32 {
        self.word(addr)
            .map(|w| w.load(Ordering::Acquire))
            .unwrap_or(0)
    }

    pub fn load(&self, off: u32) -> u32 {
        self.load_at(RING_BASE + off)
    }

    pub fn store_at(&self, addr: u32, v: u32) {
        if let Some(w) = self.word(addr) {
            w.store(v, Ordering::Release);
        } else if debug() {
            eprintln!("[rk ] write out of bounds at {addr:#x}");
        }
    }

    pub fn store(&self, off: u32, v: u32) {
        self.store_at(RING_BASE + off, v);
    }

    /// Copy bytes out of guest memory; returns the number actually copied.
    pub fn read_at(&self, addr: u32, dst: &mut [u8]) -> usize {
        let a = addr as usize;
        let d = self.mem.data();
        let n = dst.len().min(MAX_DATAGRAM).min(d.len().saturating_sub(a));
        if n != 0 {
            // SAFETY: `n` is bounded by both slices and the guest buffer.
            unsafe { std::ptr::copy_nonoverlapping(d[a].get() as *const u8, dst.as_mut_ptr(), n) };
        }
        n
    }

    /// Copy bytes into guest memory; returns the number actually written.
    pub fn write_at(&self, addr: u32, src: &[u8]) -> usize {
        let a = addr as usize;
        let d = self.mem.data();
        let n = src.len().min(d.len().saturating_sub(a));
        if n != 0 {
            // SAFETY: `n` is bounded by both slices and the guest buffer.
            unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), d[a].get(), n) };
        }
        n
    }

    pub fn write(&self, off: u32, src: &[u8]) -> usize {
        self.write_at(RING_BASE + off, src)
    }

    /// Wake the guest's `select()` replacement, which parks on WAKE.
    pub fn wake(&self) {
        let _ = self.mem.atomic_notify((RING_BASE + WAKE) as u64, 1);
    }

    /// Publish the route the guest should connect to. The patched `select`
    /// consumes CREQ once it has stashed a `RakPeer*`, and reports the result
    /// in CRES. MAGIC is written last: it is our "pump is live" flag.
    pub fn install(&self, host: IpAddr, port: u16) {
        let text = host.to_string();
        let mut buf = [0u8; HOST_LEN];
        let n = text.len().min(HOST_LEN - 1);
        buf[..n].copy_from_slice(&text.as_bytes()[..n]);
        self.write(HOST, &buf);
        self.store(PORT, port as u32);
        self.store(CREQ, 0);
        self.store(CRES, -12345i32 as u32);
        self.store(MAGIC, MAGIC_VALUE);
        if debug() {
            eprintln!(
                "[rk ] route {host}:{port} slot={} rx0={:#x}",
                self.slot, self.rx0
            );
        }
    }

    /// Ask the patched engine to call `RakPeer::Connect(host, port)`. Harmless
    /// when the guest drives its own connect (the word is just ignored).
    pub fn arm_connect(&self) {
        self.store(CREQ, 1);
    }

    pub fn peer(&self) -> u32 {
        self.load(PEER)
    }
    pub fn cres(&self) -> i32 {
        self.load(CRES) as i32
    }
    pub fn updates(&self) -> u32 {
        self.load(UPDATES)
    }
    pub fn selects(&self) -> u32 {
        self.load(SELECTS)
    }
}

// ------------------------------------------------------------------- rupp ---

const TLV1: u8 = 1;
const TLV2: u8 = 2;
const TOKEN_TLV_LEN: u8 = 17; // subtype + 16-byte token
const ADDR_TLV_LEN: u8 = 6; // 4-byte address + 2-byte port

fn tlv_end(d: &[u8]) -> Option<usize> {
    if d.len() < 8 || d[0] != 0x01 || d[1] != 0x00 {
        return None;
    }
    let total = ((d[2] as usize) << 8) | d[3] as usize;
    if !(4..=d.len()).contains(&total) {
        return None;
    }
    Some(total)
}

fn hex16(text: &str) -> Option<[u8; 16]> {
    if text.len() != 32 || !text.bytes().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let mut out = [0u8; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(out)
}

fn b64_decode(text: &str) -> Option<Vec<u8>> {
    let bs = text.as_bytes();
    if bs.is_empty() || bs.len() % 4 != 0 {
        return None;
    }
    let val = |c: u8| match c {
        b'A'..=b'Z' => Some(c - b'A'),
        b'a'..=b'z' => Some(c - b'a' + 26),
        b'0'..=b'9' => Some(c - b'0' + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        b'=' => Some(0),
        _ => None,
    };
    let pad = bs.iter().rev().take_while(|&&c| c == b'=').count();
    if pad > 2 {
        return None;
    }
    let mut out = Vec::with_capacity(bs.len() / 4 * 3);
    for chunk in bs.chunks(4) {
        let a = val(chunk[0])? as u32;
        let b = val(chunk[1])? as u32;
        let c = val(chunk[2])? as u32;
        let d = val(chunk[3])? as u32;
        let n = (a << 18) | (b << 12) | (c << 6) | d;
        out.extend_from_slice(&[(n >> 16) as u8, (n >> 8) as u8, n as u8]);
    }
    out.truncate(out.len().saturating_sub(pad));
    Some(out)
}

fn b64_16(text: &str) -> Option<[u8; 16]> {
    let v = b64_decode(text)?;
    if v.len() != 16 {
        return None;
    }
    let mut out = [0u8; 16];
    out.copy_from_slice(&v);
    Some(out)
}

fn ipv4(text: &str) -> Option<[u8; 4]> {
    let mut out = [0u8; 4];
    let mut n = 0;
    for part in text.split('.') {
        if n == 4 {
            return None;
        }
        out[n] = part.parse().ok()?;
        n += 1;
    }
    (n == 4).then_some(out)
}

/// The three fields of a join script the edge validates. Direct port of
/// `iwa/rupp.js`.
#[derive(Debug, Default, Clone)]
pub struct Rupp {
    join: Option<[u8; 16]>,
    session: Option<[u8; 16]>,
    rcc: Option<([u8; 4], u16)>,
    data_port: u16,
}

impl Rupp {
    pub fn new() -> Self {
        Self::default()
    }

    /// Take the values a join script names. Safe to call repeatedly: values
    /// already learned are never overwritten.
    pub fn set_join_script(&mut self, script: &Value) -> &mut Self {
        // `joinScript` is sometimes a JSON-encoded string inside the reply.
        let decoded;
        let obj = match script.as_str() {
            Some(s) => match serde_json::from_str::<Value>(s) {
                Ok(v) => {
                    decoded = v;
                    &decoded
                }
                Err(_) => return self,
            },
            None => script,
        };
        if self.join.is_none() {
            let tok = obj
                .get("NetStackTokenValue")
                .or_else(|| obj.get("TokenValue"))
                .and_then(Value::as_str);
            self.join = tok.and_then(|t| if t.len() == 32 { hex16(t) } else { b64_16(t) });
        }
        if self.rcc.is_none() {
            let conn = obj.get("ServerConnections").and_then(|s| s.get(0));
            let addr = obj
                .get("MachineAddress")
                .and_then(Value::as_str)
                .or_else(|| conn.and_then(|c| c.get("Address")).and_then(Value::as_str));
            let netstack = obj
                .get("NetStackPort")
                .and_then(Value::as_u64)
                .filter(|p| (1..=u16::MAX as u64).contains(p))
                .map(|p| p as u16);
            if let (Some(a), Some(p)) = (addr.and_then(ipv4), netstack) {
                self.rcc = Some((a, p));
            }
        }
        if self.data_port == 0 {
            let conn = obj.get("ServerConnections").and_then(|s| s.get(0));
            let port = conn
                .and_then(|c| c.get("Port"))
                .or_else(|| obj.get("ServerPort"))
                .and_then(Value::as_u64)
                .filter(|p| (1..=u16::MAX as u64).contains(p))
                .unwrap_or(0);
            self.data_port = port as u16;
        }
        self
    }

    /// Rewrite the fields this session owns, in place.
    pub fn stamp(&mut self, d: &mut [u8]) {
        if self.join.is_none() && self.rcc.is_none() {
            return;
        }
        let Some(end) = tlv_end(d) else { return };
        let mut p = 4usize;
        let mut sub = 0u8;
        while p + 2 <= end {
            let kind = d[p];
            let len = d[p + 1] as usize;
            if p + 2 + len > end {
                break;
            }
            if kind == TLV1 && len == TOKEN_TLV_LEN as usize {
                sub = d[p + 2];
                if sub == 1 {
                    if let Some(t) = self.join {
                        d[p + 3..p + 19].copy_from_slice(&t);
                    }
                } else if sub == 2 {
                    if let Some(t) = self.session {
                        d[p + 3..p + 19].copy_from_slice(&t);
                    }
                }
            } else if kind == TLV2 && len == ADDR_TLV_LEN as usize {
                if let Some((addr, netstack)) = self.rcc {
                    // NetStackPort is only right while handshaking; after
                    // that the session is routed by ServerConnections[0].Port.
                    let port = if sub == 2 && self.data_port != 0 {
                        self.data_port
                    } else {
                        netstack
                    };
                    d[p + 2..p + 6].copy_from_slice(&addr);
                    d[p + 6] = (port >> 8) as u8;
                    d[p + 7] = (port & 0xff) as u8;
                }
            }
            p += 2 + len;
        }
    }

    /// Remember the session token the server issues in Reply2.
    pub fn learn(&mut self, d: &[u8]) {
        let Some(end) = tlv_end(d) else { return };
        let mut p = 4usize;
        while p + 2 <= end {
            let kind = d[p];
            let len = d[p + 1] as usize;
            if p + 2 + len > end {
                return;
            }
            if kind == TLV1 && len == TOKEN_TLV_LEN as usize && d[p + 2] == 2 {
                let mut t = [0u8; 16];
                t.copy_from_slice(&d[p + 3..p + 19]);
                self.session = Some(t);
                return;
            }
            p += 2 + len;
        }
    }
}

// ------------------------------------------------------------ join script ---

/// Everything the host needs to point the ring and the RUPP stamper at a game
/// server. `parse_join` is shared with the HTTP hook and with `start`.
#[derive(Debug, Clone)]
pub struct JoinTarget {
    /// `UdmuxEndpoints[0]` when present, otherwise `ServerConnections[0]`.
    pub host: IpAddr,
    pub port: u16,
    pub machine: Option<Ipv4Addr>,
    pub netstack_port: Option<u16>,
    pub token: Option<[u8; 16]>,
    pub data_port: Option<u16>,
}

pub fn parse_join(script: &Value) -> Result<JoinTarget, String> {
    let decoded;
    let obj = if let Some(s) = script.as_str() {
        decoded = serde_json::from_str::<Value>(s).map_err(|e| format!("joinScript: {e}"))?;
        &decoded
    } else {
        // Bind a reference either way; the string form is decoded above.
        decoded = Value::Null;
        &decoded
    };
    let obj = if obj.is_null() { script } else { obj };

    let peer = obj
        .get("UdmuxEndpoints")
        .and_then(|u| u.get(0))
        .or_else(|| obj.get("ServerConnections").and_then(|s| s.get(0)))
        .ok_or("no peer endpoint")?;
    let address = peer
        .get("Address")
        .and_then(Value::as_str)
        .ok_or("peer has no Address")?;
    let host = address
        .parse::<IpAddr>()
        .map_err(|e| format!("bad peer Address {address:?}: {e}"))?;
    let port = peer
        .get("Port")
        .and_then(Value::as_u64)
        .filter(|p| (1..=u16::MAX as u64).contains(p))
        .ok_or("peer has no Port")? as u16;

    let machine = obj
        .get("MachineAddress")
        .and_then(Value::as_str)
        .and_then(ipv4)
        .map(|a| Ipv4Addr::from(a));
    let netstack_port = obj
        .get("NetStackPort")
        .and_then(Value::as_u64)
        .filter(|p| (1..=u16::MAX as u64).contains(p))
        .map(|p| p as u16);
    let tok = obj
        .get("NetStackTokenValue")
        .or_else(|| obj.get("TokenValue"))
        .and_then(Value::as_str);
    let token = tok.and_then(|t| if t.len() == 32 { hex16(t) } else { b64_16(t) });
    let data_port = obj
        .get("ServerConnections")
        .and_then(|s| s.get(0))
        .and_then(|c| c.get("Port"))
        .or_else(|| obj.get("ServerPort"))
        .and_then(Value::as_u64)
        .filter(|p| (1..=u16::MAX as u64).contains(p))
        .map(|p| p as u16);

    Ok(JoinTarget {
        host,
        port,
        machine,
        netstack_port,
        token,
        data_port,
    })
}

// ------------------------------------------------------------------- pump ---

fn slot_base(ring: &Ring, tx: bool, seq: u32) -> u32 {
    let start = if tx { TX0 } else { ring.rx0() };
    RING_BASE + start + (seq & (NSLOT - 1)) * ring.slot()
}

fn tx_loop(ring: Arc<Ring>, sock: Arc<UdpSocket>, peer: SocketAddr, rupp: Arc<Mutex<Rupp>>) {
    let mut last_cres = ring.cres();
    let mut idle_ms = 2u64;
    while !STOP.load(Ordering::Acquire) {
        let head = ring.load(TX_HEAD);
        let mut tail = ring.load(TX_TAIL);
        let mut sent = 0u32;
        while tail != head {
            let base = slot_base(&ring, true, tail);
            let len = ring.load_at(base) as usize;
            // Zero means the guest has not finished publishing this slot.
            if len == 0 || len > MAX_DATAGRAM {
                break;
            }
            let mut dgram = vec![0u8; len];
            if ring.read_at(base + 4, &mut dgram) != len {
                break;
            }
            {
                let mut r = rupp.lock().unwrap();
                r.stamp(&mut dgram);
            }
            if let Err(e) = sock.send_to(&dgram, peer) {
                ERROR_COUNT.fetch_add(1, Ordering::Relaxed);
                if debug() {
                    eprintln!("[rk ] send failed: {e}");
                }
                break;
            }
            tail = tail.wrapping_add(1);
            ring.store(TX_TAIL, tail);
            TX_COUNT.fetch_add(1, Ordering::Relaxed);
            sent += 1;
        }
        let cres = ring.cres();
        if cres != last_cres && cres != -12345 {
            eprintln!("[rk ] Connect result {cres}");
            last_cres = cres;
        }
        if sent == 0 {
            std::thread::sleep(Duration::from_millis(idle_ms));
            idle_ms = idle_ms.min(10) + 1;
        } else {
            idle_ms = 2;
        }
    }
}

fn rx_loop(ring: Arc<Ring>, sock: Arc<UdpSocket>, rupp: Arc<Mutex<Rupp>>) {
    let mut buf = [0u8; MAX_DATAGRAM];
    while !STOP.load(Ordering::Acquire) {
        let (n, from) = match sock.recv_from(&mut buf) {
            Ok(v) => v,
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                continue
            }
            Err(e) => {
                ERROR_COUNT.fetch_add(1, Ordering::Relaxed);
                if debug() {
                    eprintln!("[rk ] recv failed: {e}");
                }
                std::thread::sleep(Duration::from_millis(20));
                continue;
            }
        };
        if n == 0 {
            continue;
        }
        {
            let mut r = rupp.lock().unwrap();
            r.learn(&buf[..n]);
        }
        let head = ring.load(RX_HEAD);
        let tail = ring.load(RX_TAIL);
        if head.wrapping_sub(tail) >= NSLOT {
            DROP_COUNT.fetch_add(1, Ordering::Relaxed);
            if debug() {
                eprintln!("[rk ] RX ring full (from {from})");
            }
            continue;
        }
        let base = slot_base(&ring, false, head);
        // Publish length last: the guest's recv stub treats zero as empty.
        ring.store_at(base, 0);
        ring.write_at(base + 4, &buf[..n.min(MAX_DATAGRAM)]);
        ring.store_at(base, n.min(MAX_DATAGRAM) as u32);
        ring.store(RX_HEAD, head.wrapping_add(1));
        ring.wake(); // wake the guest's select()/recv thread
        RX_COUNT.fetch_add(1, Ordering::Relaxed);
    }
}

/// Bind one native UDP socket and connect the guest's rings to it.
///
/// The socket is bound to an ephemeral local port and kept for the whole
/// session: a fresh port per send would look to the RCC like the session
/// jumping hosts. Blocking sockets are sufficient — the pumps are host
/// threads, so no JSPI/async bridge is involved.
pub fn start(host: IpAddr, port: u16, script: Value, mem: SharedMemory) -> std::io::Result<()> {
    if STARTED.swap(true, Ordering::AcqRel) {
        return Err(std::io::Error::other("RakNet ring already started"));
    }
    let socket = UdpSocket::bind("0.0.0.0:0")?;
    socket.set_read_timeout(Some(Duration::from_millis(250)))?;
    socket.set_write_timeout(Some(Duration::from_millis(250)))?;
    let socket = Arc::new(socket);
    let peer = SocketAddr::new(host, port);

    let ring = Arc::new(Ring::new(mem));
    ring.install(host, port);

    let mut r = Rupp::new();
    r.set_join_script(&script);
    if debug() {
        eprintln!(
            "[rk ] join token={} machine={:?} netstack={:?} data={:?}",
            r.join
                .map(|t| t.iter().map(|b| format!("{b:02x}")).collect::<String>())
                .unwrap_or_default(),
            r.rcc.map(|x| x.0),
            r.rcc.map(|x| x.1),
            r.data_port,
        );
    }
    let rupp = Arc::new(Mutex::new(r));

    let tx = {
        let ring = ring.clone();
        let sock = socket.clone();
        let rupp = rupp.clone();
        std::thread::Builder::new()
            .name("rk-tx".into())
            .spawn(move || tx_loop(ring, sock, peer, rupp))?
    };
    let rx = {
        let ring = ring.clone();
        let sock = socket.clone();
        let rupp = rupp.clone();
        std::thread::Builder::new()
            .name("rk-rx".into())
            .spawn(move || rx_loop(ring, sock, rupp))?
    };
    // Now that the socket and both pumps are live, ask the patched guest to
    // connect. It consumes CREQ when its select() stub sees a RakPeer*.
    ring.arm_connect();
    eprintln!(
        "[rk ] udp {peer} local {} ({} slots x {} B)",
        socket
            .local_addr()
            .map(|a| a.to_string())
            .unwrap_or_else(|_| "?".into()),
        NSLOT,
        ring.slot()
    );
    drop(tx);
    drop(rx);
    Ok(())
}
