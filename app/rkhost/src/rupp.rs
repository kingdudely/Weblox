//! RUPP header fix-ups.
//!
//! The engine's RakNet emits correct RUPP framing, but three fields in the TLV
//! block are session data that only the join script knows:
//!
//! ```text
//! TLV1 subtype 1   NetStackTokenValue            (handshake)
//! TLV1 subtype 2   the token the server sends in Reply2
//! TLV2             RCC address + NetStackPort while handshaking,
//!                  RCC address + ServerConnections[0].Port after that
//! ```
//!
//! The engine fills these with its own random values, and the edge silently
//! drops anything that disagrees. This lives in rkhost rather than in the page
//! because the join script is parsed here anyway and every datagram already
//! passes through this process.

use crate::Cfg;

/// Bytes the session needs in order to rewrite a datagram it owns.
pub struct Rupp {
    join: Option<[u8; 16]>,
    session: Option<[u8; 16]>,
    rcc: Option<([u8; 4], u16)>,
    data_port: u16,
}

impl Rupp {
    pub fn new() -> Self {
        Self { join: None, session: None, rcc: None, data_port: 0 }
    }

    /// The bridge connects before the engine joins, so the join-script values
    /// arrive later. Pick them up until both are known.
    pub fn refresh(&mut self, cfg: &Cfg) {
        if self.join.is_none() {
            self.join = cfg.rupp_token.lock().unwrap().as_deref().and_then(hex16);
        }
        if self.rcc.is_none() {
            self.rcc = cfg
                .rcc
                .lock()
                .unwrap()
                .as_ref()
                .and_then(|(ip, port)| parse_ipv4(ip).map(|a| (a, *port)));
        }
        if self.data_port == 0 {
            self.data_port = cfg.data_port.lock().unwrap().unwrap_or(0);
        }
    }

    /// Rewrite the fields we own, in place.
    pub fn stamp(&mut self, d: &mut [u8]) {
        if self.join.is_none() && self.rcc.is_none() {
            return;
        }
        let Some(end) = tlv_end(d) else { return };
        let mut p = 4;
        let mut sub = 0u8;
        while p + 2 <= end {
            let kind = d[p];
            let len = d[p + 1] as usize;
            if p + 2 + len > end {
                break;
            }
            if kind == 1 && len == 17 {
                sub = d[p + 2];
                match (sub, self.join, self.session) {
                    (1, Some(join), _) => d[p + 3..p + 19].copy_from_slice(&join),
                    (2, _, Some(session)) => d[p + 3..p + 19].copy_from_slice(&session),
                    _ => {}
                }
            } else if kind == 2 && len == 6 {
                if let Some((addr, netstack)) = self.rcc {
                    let port = if sub == 2 && self.data_port != 0 { self.data_port } else { netstack };
                    d[p + 2..p + 6].copy_from_slice(&addr);
                    d[p + 6..p + 8].copy_from_slice(&port.to_be_bytes());
                }
            }
            p += 2 + len;
        }
    }

    /// Remember the session token the server issues in Reply2.
    pub fn learn(&mut self, d: &[u8]) {
        let Some(end) = tlv_end(d) else { return };
        let mut p = 4;
        while p + 2 <= end {
            let kind = d[p];
            let len = d[p + 1] as usize;
            if p + 2 + len > end {
                return;
            }
            if kind == 1 && len == 17 && d[p + 2] == 2 {
                let mut tok = [0u8; 16];
                tok.copy_from_slice(&d[p + 3..p + 19]);
                self.session = Some(tok);
                return;
            }
            p += 2 + len;
        }
    }
}

/// Length of the TLV block this datagram declares, bounded by the datagram.
fn tlv_end(d: &[u8]) -> Option<usize> {
    if d.len() < 8 || d[0] != 0x01 || d[1] != 0x00 {
        return None;
    }
    let total = ((d[2] as usize) << 8) | d[3] as usize;
    if total < 4 || total > d.len() {
        return None;
    }
    Some(total)
}

fn hex16(s: &str) -> Option<[u8; 16]> {
    if s.len() != 32 {
        return None;
    }
    let mut out = [0u8; 16];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(s.get(i * 2..i * 2 + 2)?, 16).ok()?;
    }
    Some(out)
}

fn parse_ipv4(s: &str) -> Option<[u8; 4]> {
    let mut out = [0u8; 4];
    let mut n = 0;
    for part in s.split('.') {
        if n == 4 {
            return None;
        }
        out[n] = part.parse().ok()?;
        n += 1;
    }
    (n == 4).then_some(out)
}
