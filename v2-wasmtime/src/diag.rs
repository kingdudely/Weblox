//! Stall diagnostics: a heartbeat every completed turn and the last suspending
//! import, so a watchdog can say *what* a stuck turn is parked on.
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::Instant;

static EVENTS: AtomicU64 = AtomicU64::new(0);
static NESTED: AtomicU64 = AtomicU64::new(0);
static NOTIFY: AtomicU64 = AtomicU64::new(0);
static LIVE: AtomicU64 = AtomicU64::new(0);
static START: Mutex<Option<Instant>> = Mutex::new(None);
static LAST: Mutex<Option<String>> = Mutex::new(None);

pub fn start() {
    *START.lock().unwrap() = Some(Instant::now());
    *LAST.lock().unwrap() = None;
}

pub fn event(what: String) {
    *LAST.lock().unwrap() = Some(what);
    EVENTS.fetch_add(1, Ordering::Relaxed);
}

pub fn worker_up() {
    LIVE.fetch_add(1, Ordering::Relaxed);
}

pub fn worker_down() {
    LIVE.fetch_sub(1, Ordering::Relaxed);
}

pub fn notify() {
    NOTIFY.fetch_add(1, Ordering::Relaxed);
}

pub fn nested() {
    NESTED.fetch_add(1, Ordering::Relaxed);
}

pub fn checkpoint() -> u64 {
    EVENTS.fetch_add(1, Ordering::Relaxed)
}

/// Once a second: turn rate, nested count, and whatever is being awaited.
pub fn watch(secs: u64) {
    let mut prev = checkpoint();
    let mut stalls = 0u32;
    let mut dumped = false;
    loop {
        std::thread::sleep(std::time::Duration::from_millis(1000));
        let now = EVENTS.load(Ordering::Relaxed);
        let ms = START.lock().unwrap().map(|s| s.elapsed().as_millis()).unwrap_or(0);
        let last = LAST.lock().unwrap().clone().unwrap_or_else(|| "<none>".into());
        eprintln!("[rate {ms}ms] events/s={} nested={} notify={} live_workers={} clock={:.0} last={last}",
                  now - prev, NESTED.load(Ordering::Relaxed), NOTIFY.load(Ordering::Relaxed), LIVE.load(Ordering::Relaxed),
                  crate::imports::boot::perf_ms());
        if !dumped {
            if let Ok(at) = std::env::var("WC_DUMP_AT") {
                if ms as u64 >= at.parse::<u64>().unwrap_or(u64::MAX) {
                    dumped = true;
                    let names: Vec<String> = crate::fibre::TRACE_T
                        .lock().unwrap().iter().map(|(t, _, _, _)| t.clone()).collect();
                    for t in names {
                        let h = crate::trace::hist_of(&t);
                        let tail: Vec<&str> = h.iter().rev().take(16).cloned().collect();
                        eprintln!("[at  ] {t} last: {}", tail.join(" "));
                    }
                }
            }
        }
        if now == prev {
            stalls += 1;
            if stalls as u64 >= secs {
                eprintln!("[stall] nothing moved for {stalls}s");
                let names: Vec<String> = {
                    let m = crate::fibre::TRACE_T.lock().unwrap();
                    m.iter().map(|(t, _, _, _)| t.clone()).collect()
                };
                for t in names {
                    let h = crate::trace::hist_of(&t);
                    let tail: Vec<&str> = h.iter().rev().take(14).cloned().collect();
                    eprintln!("[stall] {t} last: {}", tail.join(" "));
                }
                for (t, c, sp, w) in crate::fibre::TRACE_T.lock().unwrap().iter() {
                    eprintln!("[stall]   thread {t} cur={c:#x} sp={sp:#x} waiting={w}");
                }
                stalls = 0;
            }
        } else {
            stalls = 0;
        }
        prev = now;
    }
}

pub fn last() -> String {
    LAST.lock().unwrap().clone().unwrap_or_else(|| "<none>".into())
}

/// Dump the word a stuck futex is parked on, plus whoever points at it.
pub fn dump_futex(mem: &wasmtime::SharedMemory, addr: u32) {
    let d = mem.data();
    let base = addr as usize;
    let word = unsafe { *(d[base].get() as *const u32) };
    eprintln!("[dump] futex 0x{addr:x} = {word:#x}");
    let lo = base.saturating_sub(64) & !3;
    for row in 0..8 {
        let at = lo + row * 16;
        let mut hex = String::new();
        let mut asc = String::new();
        for i in 0..16 {
            let b = unsafe { *d[at + i].get() };
            hex.push_str(&format!("{b:02x} "));
            asc.push(if (0x20..0x7f).contains(&b) { b as char } else { '.' });
        }
        eprintln!("[dump] {at:08x}  {hex} |{asc}|");
    }
    // who points at this word (4-byte pointers)?
    let mut hits = 0;
    for (lo, hi) in [(addr & !0xf, (addr & !0xf) + 0x20)] {
        let needle = lo.to_le_bytes();
        let mut at = 0usize;
        while at + 4 < d.len() && hits < 10 {
            if d[at + 0].get() as u32 & 0xff == needle[0] as u32 {
                let v = unsafe { *(d[at].get() as *const u32) };
                if (lo..hi).contains(&v) {
                    eprintln!("[dump]   ptr 0x{v:x} -> word at 0x{at:08x}");
                    hits += 1;
                }
            }
            at += 4;
        }
    }
    if hits == 0 {
        eprintln!("[dump]   no pointer to it");
    }
}

/// Wide dump: find the object header (vtable / static pointer) near a word.
pub fn dump_wide(mem: &wasmtime::SharedMemory, addr: u32) {
    let d = mem.data();
    let start = (addr as usize & !0xf).saturating_sub(0x200);
    eprintln!("[wide] around 0x{addr:x}");
    for row in 0..40 {
        let at = start + row * 16;
        let mut hex = String::new();
        let mut asc = String::new();
        for i in 0..16 {
            let b = unsafe { *d[at + i].get() };
            hex.push_str(&format!("{b:02x} "));
            asc.push(if (0x20..0x7f).contains(&b) { b as char } else { '.' });
        }
        eprintln!("[wide] {at:08x}  {hex} |{asc}|");
    }
}
