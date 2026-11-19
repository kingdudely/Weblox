//! Tiny ring of the most recent import calls, for "what happened before it
//! threw" without a full tracer.
use std::cell::RefCell;

const N: usize = 48;

static HIST: std::sync::Mutex<Vec<&'static str>> = std::sync::Mutex::new(Vec::new());

thread_local! {
    static RING: RefCell<[(&'static str, u32); N]> = const { RefCell::new([("", 0); N]) };
    static SEQ: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

pub fn enabled() -> bool {
    std::env::var_os("WC_TRACE").is_some()
}

pub fn hit(name: &'static str) {
    if enabled() {
        let mut h = HIST.lock().unwrap();
        if h.last() == Some(&name) {
            return;
        }
        if h.len() == 4096 {
            h.remove(0);
        }
        h.push(name);
    }
    let s = SEQ.with(|c| {
        let v = c.get() + 1;
        c.set(v);
        v
    });
    RING.with(|r| r.borrow_mut()[(s as usize) % N] = (name, s));
}

pub fn hist() -> Vec<&'static str> {
    HIST.lock().unwrap().clone()
}

pub fn dump() {
    RING.with(|r| {
        let r = r.borrow();
        let s = SEQ.with(|c| c.get()) as usize;
        eprintln!("[trace] last imports (oldest first, runs collapsed):");
        let mut prev = "";
        let mut run = 0;
        for i in 0..N {
            let (n, _) = r[(s + N - i) % N];
            if n.is_empty() {
                continue;
            }
            if n == prev {
                run += 1;
                continue;
            }
            if run > 0 {
                eprintln!("[trace]   {prev} x{}", run + 1);
            }
            prev = n;
            run = 0;
        }
        if run > 0 {
            eprintln!("[trace]   {prev} x{}", run + 1);
        }
    });
}
