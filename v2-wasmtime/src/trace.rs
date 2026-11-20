//! Tiny ring of the most recent import calls, for "what happened before it
//! threw" without a full tracer.
use std::cell::RefCell;

const N: usize = 48;

static HIST: std::sync::Mutex<Option<std::collections::HashMap<String, Vec<&'static str>>>> =
    std::sync::Mutex::new(None);

thread_local! {
    static RING: RefCell<[(&'static str, u32); N]> = const { RefCell::new([("", 0); N]) };
    static SEQ: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

pub fn enabled() -> bool {
    std::env::var_os("WC_TRACE").is_some()
}

pub fn hit(name: &'static str) {
    if enabled() {
        let mut g = HIST.lock().unwrap();
        let m = g.get_or_insert_with(std::collections::HashMap::new);
        let h = m.entry(std::thread::current().name().unwrap_or("?").to_string()).or_default();
        if h.last() != Some(&name) {
            if h.len() == 40 {
                h.remove(0);
            }
            h.push(name);
        }
    }
    let s = SEQ.with(|c| {
        let v = c.get() + 1;
        c.set(v);
        v
    });
    RING.with(|r| r.borrow_mut()[(s as usize) % N] = (name, s));
}

pub fn hist_of(thread: &str) -> Vec<&'static str> {
    HIST.lock()
        .unwrap()
        .as_ref()
        .and_then(|m| m.get(thread))
        .cloned()
        .unwrap_or_default()
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
