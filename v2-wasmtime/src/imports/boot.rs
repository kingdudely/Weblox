//! Imports the boot path needs: console, misc timing/entropy, host info, the
//! EM_ASM table, and the VFS hooks. Everything here mirrors what
//! WebClient.js/WasmPlayer.js does for the same entry.
use std::io::Write;

use std::cell::Cell;

use wasmtime::{Caller, Linker, SharedMemory};

thread_local! {
    static CLK: Cell<u32> = const { Cell::new(0) };
    /// set once this thread logged the start-game task
    static SUBMIT: Cell<bool> = const { Cell::new(false) };
}

pub fn submit_thread() -> bool {
    SUBMIT.with(|c| c.get())
}

use crate::state::{rbytes, rstr, wbytes, wstr, St, CANVAS_H, CANVAS_W};

/// The engine's clock: one tick per host frame, so it is stable inside a frame
/// and still advances (a frozen clock starves the scheduler, a wall clock makes
/// frame pacing nondeterministic).
static FRAME_MS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1000);

/// The engine clock is wall time, not frames: this host runs the turn loop far
/// faster than 60 Hz, and a frame-counted clock runs away with it.
pub fn advance_frame() {}

/// Set once the engine has an audio sink; the main loop then calls the mix
/// export the way `onaudioprocess` would.
pub static AUDIO: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Set once the join thread has its data model; a boot that has not got this
/// far is wedged (the engine can deadlock one of its own start-up handshakes)
/// and gets restarted.
pub static BOOTED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Replace this process with a fresh one: the guest keeps all its state in
/// memory, so a retry has to start over, and the page reloads for the same
/// reason.
pub fn restart() -> ! {
    use std::os::unix::process::CommandExt;
    eprintln!("[boot] stalled, restarting");
    let argv: Vec<std::ffi::OsString> = std::env::args_os().collect();
    let err = std::process::Command::new(&argv[0]).args(&argv[1..]).exec();
    eprintln!("[boot] exec failed: {err}");
    std::process::exit(1);
}

pub fn start_clock() {}

struct MainJob {
    key: u32,
    sig: String,
    args: Vec<u8>,
    tx: std::sync::mpsc::Sender<f64>,
}
static JOBS: std::sync::Mutex<Vec<MainJob>> = std::sync::Mutex::new(Vec::new());

/// Run one queued "on main thread" job.  Called from the main thread's safe
/// points, which is where the page's glue would have run it too.
pub fn run_main_jobs() {
    let sp = crate::fibre::store_ptr();
    let cx = unsafe { &mut *sp };
    loop {
        let job = {
            let mut q = JOBS.lock().unwrap();
            if q.is_empty() { return }
            q.remove(0)
        };
        let sigp = {
            let mut v = job.sig.clone().into_bytes();
            v.push(0);
            let d = cx.data().mem.data();
            let at = SCRATCH.with(|c| c.get());
            if at == 0 {
                let base = d.len() - 0x2000;
                for (i, b) in v.iter().enumerate() {
                    unsafe { *d[base + i].get() = *b };
                }
                SCRATCH.with(|c| c.set(base as u32));
                base as u32
            } else {
                for (i, b) in v.iter().enumerate() {
                    unsafe { *d[at as usize + i].get() = *b };
                }
                at
            }
        };
        let args = job.args;
        let buf = SCRATCH2.with(|c| { let v = c.get(); if v == 0 { let b = cx.data().mem.data().len() as u32 - 0x1000; c.set(b); b } else { v } });
        {
            let d = cx.data().mem.data();
            for (i, b) in args.iter().enumerate() {
                unsafe { *d[buf as usize + i].get() = *b };
            }
        }
        eprintln!("[mt  ] job key={} args={}", job.key, args.len());
        let v = asm_eval(cx, job.key, sigp, buf);
        let _ = job.tx.send(v);
    }
}

thread_local! {
    static SCRATCH: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    static SCRATCH2: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

/// Copy a signature's packed arguments out of guest memory.
fn pack_args(cx: &wasmtime::Store<St>, sig: &str, buf: u32) -> Vec<u8> {
    let d = cx.data().mem.data();
    let mut v = Vec::new();
    let mut off = buf as usize;
    for ch in sig.bytes() {
        let wide = ch == b'j' || ch == b'f' || ch == b'd';
        if wide && off % 8 != 0 { off += 4; }
        let n = if wide { 8 } else { 4 };
        for i in 0..n { v.push(unsafe { *d[off + i].get() }); }
        off += n;
    }
    v
}

fn start() -> &'static std::time::Instant {
    static S: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    S.get_or_init(std::time::Instant::now)
}

pub fn perf_ms() -> f64 {
    let _ = &FRAME_MS;
    // the page runs at 60 Hz and this host runs the loop far faster; a fixed
    // multiple keeps timers ordered without a runaway clock
    1000.0 + start().elapsed().as_secs_f64() * 1000.0
}


pub fn dump_pcs_pub() { dump_pcs() }

fn dump_pcs() {
    unsafe {
        let mut fp: usize;
        std::arch::asm!("mov {}, rbp", out(reg) fp);
        for _ in 0..20 {
            if fp == 0 {
                break;
            }
            let ret = *((fp + 8) as *const usize);
            eprintln!("[pc] {ret:x}");
            fp = *(fp as *const usize);
        }
    }
}

fn rstr_ctx(c: &wasmtime::Store<St>, p: u32) -> String {
    let d = c.data().mem.data();
    if p == 0 { return String::new(); }
    let mut v = Vec::new();
    let mut i = p as usize;
    loop {
        let b = unsafe { *d[i].get() };
        if b == 0 || v.len() > 4096 { break }
        v.push(b);
        i += 1;
    }
    String::from_utf8_lossy(&v).into_owned()
}

fn now_ms() -> f64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    let d = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    d.as_secs_f64() * 1000.0
}

fn cores() -> i32 {
    std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1) as i32
}

/// ASM_CONSTS: the module passes a numeric key, a signature string and a packed
/// argument buffer; the glue decodes the signature and spreads the values.
fn asm_eval(c: &mut wasmtime::Store<St>, key: u32, sigp: u32, buf: u32) -> f64 {
    let sig = rstr_ctx(c, sigp);
    let mut off = buf as usize;
    let mut vals: Vec<f64> = Vec::new();
    for ch in sig.bytes() {
        let wide = ch == b'j' || ch == b'f' || ch == b'd';
        if wide && off % 8 != 0 {
            off += 4;
        }
        let d = c.data().mem.data();
        let v = match ch {
            b'f' => unsafe { (*(d.as_ptr().add(off) as *const f32)) as f64 },
            b'd' | b'j' => unsafe { (*(d.as_ptr().add(off) as *const u64)) as f64 },
            _ => unsafe { (*(d.as_ptr().add(off) as *const u32)) as f64 },
        };
        vals.push(v);
        off += if wide { 8 } else { 4 };
    }
    asm_invoke(c, key, &vals)
}

/// Run one of the engine's ASM_CONSTS entries with decoded arguments.
pub fn asm_invoke(c: &mut wasmtime::Store<St>, key: u32, vals: &[f64]) -> f64 {
    c.data_mut().asm_last = key;
    let a = |i: usize| *vals.get(i).unwrap_or(&0.0) as u32;

    match key {
        // The keys are the `ASM_CONSTS` table indices from this build's glue.
        9098847 => 0.0,                            // navigator.webdriver
        9098887 => 0.0,                            // main() entered
        9099052 => 0.0,                            // experience left
        9099118 => {                               // engine loading status
            let kind = if a(0) != 0 { "FAILURE" } else { "status" };
            let src = match a(1) { 1 => "join", 2 => "teleport", _ => "gui-service" };
            eprintln!("[asm] loading {kind} source={src} msg={:?} err={} status={}",
                      rstr_ctx(&*c, a(2)), a(3) as i32, a(4) as i32);
            0.0
        }
        9099404 | 9099461 => 0.0,                  // native exit
        9099518 => {                               // teleport failed
            eprintln!("[asm] teleport failed status={}", a(0) as i32);
            0.0
        }
        9099595 => 0.0,                            // loading screen removed
        9099687 => { crate::trace::hit("perf_now"); crate::pump_frame(); perf_ms() }  // performance.now
        9099717 => {                               // join failed
            eprintln!("[asm] JOIN FAILED status={}", a(0) as i32);
            0.0
        }
        9099790 => 0.0,                            // wasmPreInitTime
        9099832 => {                               // experience joined
            eprintln!("[asm] EXPERIENCE JOINED userId={}", a(0) as i32);
            0.0
        }
        9099902 => 0.0,                            // leave
        9111863 => {                               // crash report
            let d = c.data().mem.data();
            let raw = |p: u32| (0..200u32).map(|i| unsafe { *d[p as usize + i as usize].get() }).take_while(|b| *b != 0).collect::<Vec<u8>>();
            eprintln!("[asm] CRASH {:?} {:?} imp={} asm={}", String::from_utf8_lossy(&raw(a(0))), String::from_utf8_lossy(&raw(a(1))), c.data().imp_last, c.data().asm_last);
            for i in 1..6 {
                let v = a(i);
                if v > 0x1000 {
                    let d = c.data().mem.data();
                    let mut out = Vec::new();
                    let mut k = v as usize;
                    while k < d.len() && out.len() < 160 {
                        let b = unsafe { *d[k].get() };
                        if b == 0 { break }
                        if (0x20..0x7f).contains(&b) { out.push(b) } else { out.push(b'.') }
                        k += 1;
                    }
                    if out.len() > 3 { eprintln!("[asm]   arg{i}->{}: {}", v, String::from_utf8_lossy(&out)); }
                }
            }
            0.0
        }
        9119019 => 0.0,                            // saveIxpSettings
        9119097 => {                               // cors: non-allowlisted header
            eprintln!("[asm] cors: non-allowlisted header {:?}", rstr_ctx(&*c, a(0)));
            0.0
        }
        9118661 => 1.0,                            // a WebGPU device exists (webgpu.rs)
        9118945 => 0.0,                            // device lost
        9119603 => 0.0,                            // wasmOffsetConverter
        9119660 => 0.0,                            // install getRandomValue
        9120436 => {                               // getRandomValue
            let mut b = [0u8; 4];
            let _ = getrandom(&mut b);
            u32::from_le_bytes(b) as f64
        }
        9111979 => {
            crate::imports::thread::mark_late_ready();
            let g = { let d = c.data().mem.data(); unsafe { *(d[8692188usize].get() as *const u32) } };
            eprintln!("[asm] fmod args={} {} g={g}", a(0) as i32, a(1) as i32);
            0.0
        }
        // audio context probes: pretend they worked
        9112441 => 0.0,                          // prefer the synchronous ScriptProcessor path
        9117655 | 9118217 | 9115514 => 48000.0,    // AudioContext sample rate probes
        9114286 => crate::audio::init(),
        9118423 => { crate::audio::connect(); 0.0 },
        9117856 => { crate::audio::create_processor(a(0) as usize); 0.0 },
        9112554 => 0.0,
        9114701 | 9117856 | 9118423 | 9112730 | 9113734 | 9113952 => 0.0,
        9114224 => 0.0,                            // worklet node
        9117920 => { crate::audio::copy(&*c, a(0), a(1), a(2), a(3)); 0.0 }
        9115094 | 9115715 | 9116344 => 0.0,
        9116868 | 9115514 => 48000.0,              // sample rate
        9117251 | 9117632 => { crate::audio::suspend(); 0.0 }
        9118195 => { crate::audio::resume(); 0.0 }
        9117293 => { crate::audio::stop(); 0.0 }
        9115926 | 9117579 | 9118718 => 0.0,
        _ => {
            let d = c.data().mem.data();
            for probe in [9099118u32, 9111979u32] {
                let b: Vec<u8> = (probe as usize..probe as usize + 40).map(|i| unsafe { *d[i].get() }).collect();
                let t: String = b.iter().map(|x| if (0x20..0x7f).contains(x) { *x as char } else { '.' }).collect();
                eprintln!("[asm*] {probe} = {t:?}");
            }
            let a0 = (key as usize).saturating_sub(48);
            let bytes: Vec<u8> = (a0..key as usize + 160).map(|i| unsafe { *d[i].get() }).collect();
            let txt: String = bytes.iter().map(|b| if (0x20..0x7f).contains(b) { *b as char } else { '.' }).collect();
            eprintln!("[asm] unknown key {key} args={} {}", a(0) as i32, a(1) as i32);
            0.0
        }
    }
}

/// The glue's `_emscripten_*` table, in the module's own order, for calls a
/// worker queues on the main thread.  Only the browser-facing entries matter
/// here; the rest are document event registration this host has no use for.
pub fn main_thread_call(index: u32, a: &[f64]) -> f64 {
    let a0 = |i: usize| *a.get(i).unwrap_or(&0.0) as u32;
    match index {
        10 => 1.0,          // devicePixelRatio
        13 => 0.0,          // navigator.getGamepads().length
        12 => -7.0,         // no live gamepad state
        15 => 0.0,          // getGamepads
        43 | 44 => 0.0,     // WebSocket close/free
        _ => {
            if std::env::var_os("WC_MBX").is_some() {
                eprintln!("[mbx ] main call {index} args={:?} a0={}", a, a0(0));
            }
            0.0
        }
    }
}

fn getrandom(b: &mut [u8]) -> std::io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(b)
}

const INFO: &[(&str, &str)] = &[
    ("hostOS", "5"),
    ("osName", "Linux"),
    ("osVersion", ""),
    ("deviceModel", ""),
    ("browserName", "Chrome"),
    ("browserVersion", "140.0.0.0"),
    ("gpuVendor", "Mesa"),
    ("gpuRenderer", "llvmpipe"),
    ("screenWidthDip", "1280"),
    ("screenHeightDip", "720"),
    ("screenWidthPx", "1280"),
    ("screenHeightPx", "720"),
    ("dpi", "96"),
    ("mobile", "0"),
    ("memoryMB", "2048"),
];

pub fn register(l: &mut Linker<St>) -> anyhow::Result<()> {
    // console
    l.func_wrap("env", "emscripten_console_log", |_c: Caller<'_, St>, p: i32| {
        let s = rstr(&_c, p as u32);
        if s.contains("join: initialized") || s.contains("joinGameSucceeded") {
            BOOTED.store(true, std::sync::atomic::Ordering::Relaxed);
        }
        if let Ok(k) = std::env::var("WC_BT_MSG") {
            if s.contains(&k) {
                let mem = _c.data().mem.clone();
                let d = mem.data();
                let needle = k.as_bytes();
                let end = mem.data_size().min(400 << 20);
                let mut at = 0x400;
                while at + needle.len() < end {
                    let mut j = 0;
                    while j < needle.len() && unsafe { *(d[at + j].get() as *const u8) } == needle[j] {
                        j += 1;
                    }
                    if j == needle.len() {
                        eprintln!("[pc] === {k} at {at:#x}");
                        break;
                    }
                    at += 1;
                }
                dump_pcs();
            }
        }
        eprintln!("[log ] {s}");
    })?;
    l.func_wrap("env", "emscripten_console_warn", |_c: Caller<'_, St>, p: i32| {
        let s = rstr(&_c, p as u32);
        if std::env::var_os("WC_BT2").is_some() && s.contains("abort") {
            dump_pcs();
        }
        eprintln!("[warn] {s}");
    })?;
    l.func_wrap("env", "emscripten_console_error", |_c: Caller<'_, St>, p: i32| {
        eprintln!("[err ] {}", rstr(&_c, p as u32));
    })?;
    l.func_wrap("env", "emscripten_outn", |c: Caller<'_, St>, p: i32, n: i32| {
        let b = rbytes(&c, p as u32, n as u32);
        let _ = std::io::stdout().write_all(&b);
    })?;
    l.func_wrap("env", "emscripten_errn", |c: Caller<'_, St>, p: i32, n: i32| {
        let b = rbytes(&c, p as u32, n as u32);
        let _ = std::io::stderr().write_all(&b);
    })?;
    l.func_wrap("env", "_emscripten_log_formatted", |c: Caller<'_, St>,
     _flags: i32, fmt: i32| {
        let s = rstr(&c, fmt as u32);
        if std::env::var_os("WC_BT2").is_some() && s.contains("abort") {
            unsafe {
                let mut fp: usize;
                std::arch::asm!("mov {}, rbp", out(reg) fp);
                for _ in 0..18 {
                    if fp == 0 { break }
                    let ret = *((fp + 8) as *const usize);
                    eprintln!("[pc] {ret:x}");
                    fp = *(fp as *const usize);
                }
            }
        }
        if s.contains("submitStartGameTask") {
            SUBMIT.with(|f| f.set(true));
        }
        eprintln!("[log ] {s}");
    })?;

    // The glue throws an "unwind" here; main() must stop, not continue.
    l.func_wrap("env", "emscripten_exit_with_live_runtime", |mut c: Caller<'_, St>| -> wasmtime::Result<()> {
        c.data_mut().exited = true;
        Err(wasmtime::Error::msg("unwind"))
    })?;

    // failure
    l.func_wrap("env", "__assert_fail", |c: Caller<'_, St>, cond: i32, file: i32,
     line: i32, func: i32| -> wasmtime::Result<()> {
        let msg = format!("Assertion failed: {}, at: {}:{} {}",
            rstr(&c, cond as u32), rstr(&c, file as u32), line, rstr(&c, func as u32));
        eprintln!("[assert] {msg}");
        Err(wasmtime::Error::msg(msg))
    })?;
    l.func_wrap("env", "_abort_js", || -> wasmtime::Result<()> {
        eprintln!("[abort]");
        Err(wasmtime::Error::msg("abort"))
    })?;

    // clock / entropy / heap
    l.func_wrap("env", "emscripten_get_now", |mut c: Caller<'_, St>| {
        crate::trace::hit("get_now");
        crate::pump_frame();
        crate::imports::mailbox::pump_now(&mut c);
        perf_ms()
    })?;
    l.func_wrap("env", "emscripten_date_now", || {
        crate::pump_frame();
        let v = now_ms();
        static DN: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let n = DN.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if n < 8 {
            eprintln!("[clk ] date_now -> {v:.0}");
        }
        v
    })?;
    // wasi/emscripten convention: 0 on success
    l.func_wrap("env", "random_get", |mut c: Caller<'_, St>, p: i32, n: i32| -> i32 {
        crate::pump_frame();
        let mut b = vec![0u8; n.max(0) as usize];
        if getrandom(&mut b).is_err() {
            return -1;
        }
        wbytes(&mut c, p as u32, &b);
        0
    })?;
    l.func_wrap("env", "emscripten_get_heap_max", || i32::MIN)?;
    l.func_wrap("env", "emscripten_resize_heap", |c: Caller<'_, St>, want: i32| -> i32 {
        (want as u32 as u64 <= c.data().mem.data().len() as u64) as i32
    })?;
    l.func_wrap("env", "emscripten_num_logical_cores", || cores())?;
    l.func_wrap("env", "get_hardware_concurrency", || cores())?;
    l.func_wrap("env", "HaveOffsetConverter", || 0i32)?;
    l.func_wrap("env", "RBX_setFullWasmStackTrace", || 0.0f64)?;
    l.func_wrap("env", "RBX_restoreWasmStackTraceLimit", |_n: f64| {})?;

    // EM_ASM
    l.func_wrap("env", "emscripten_asm_const_int",
        |mut c: Caller<'_, St>, key: i32, sig: i32, buf: i32| -> i32 {
            crate::trace::hit("asm_int");
            (({ let sp = crate::fibre::store_ptr(); asm_eval(unsafe { &mut *sp }, key as u32, sig as u32, buf as u32) })) as u32 as i32
        })?;
    l.func_wrap("env", "emscripten_asm_const_double",
        |mut c: Caller<'_, St>, key: i32, sig: i32, buf: i32| -> f64 {
            ({ let sp = crate::fibre::store_ptr(); asm_eval(unsafe { &mut *sp }, key as u32, sig as u32, buf as u32) })
        })?;
    l.func_wrap("env", "emscripten_asm_const_int_sync_on_main_thread",
        |mut c: Caller<'_, St>, key: i32, sig: i32, buf: i32| -> i32 {
            crate::trace::hit("asm_main");
            eprintln!("[asmk] {}", key);
            if c.data().is_main {
                return (({ let sp = crate::fibre::store_ptr(); asm_eval(unsafe { &mut *sp }, key as u32, sig as u32, buf as u32) })) as u32 as i32;
            }
            let sigs = rstr(&c, sig as u32);
            let sp = crate::fibre::store_ptr(); let args = pack_args(unsafe { &*sp }, &sigs, buf as u32);
            let (tx, rx) = std::sync::mpsc::channel();
            JOBS.lock().unwrap().push(MainJob { key: key as u32, sig: sigs, args, tx });
            match rx.recv_timeout(std::time::Duration::from_secs(30)) {
                Ok(v) => v as u32 as i32,
                Err(_) => 0,
            }
        })?;
    l.func_wrap("env", "emscripten_asm_const_async_on_main_thread",
        |mut c: Caller<'_, St>, key: i32, sig: i32, buf: i32| {
            if c.data().is_main {
                ({ let sp = crate::fibre::store_ptr(); asm_eval(unsafe { &mut *sp }, key as u32, sig as u32, buf as u32) });
                return;
            }
            let sigs = rstr(&c, sig as u32);
            let sp = crate::fibre::store_ptr(); let args = pack_args(unsafe { &*sp }, &sigs, buf as u32);
            let (tx, _rx) = std::sync::mpsc::channel();
            JOBS.lock().unwrap().push(MainJob { key: key as u32, sig: sigs, args, tx });
        })?;

    // canvas / device
    l.func_wrap("env", "emscripten_get_canvas_element_size",
        |mut c: Caller<'_, St>, _t: i32, w: i32, h: i32| -> i32 {
            wbytes(&mut c, w as u32, &CANVAS_W.to_le_bytes());
            wbytes(&mut c, h as u32, &CANVAS_H.to_le_bytes());
            1
        })?;
    l.func_wrap("env", "emscripten_set_canvas_element_size", |_a: i32, _b: i32, _c: i32| 1i32)?;
    l.func_wrap("env", "get_effective_device_pixel_ratio", || 1.0f64)?;
    l.func_wrap("env", "emscripten_get_device_pixel_ratio", || 1.0f64)?;

    // host info
    l.func_wrap("env", "read_host_info_int", |c: Caller<'_, St>, k: i32| -> i32 {
        let key = rstr(&c, k as u32);
        match key.as_str() {
            "cpuLogicalCount" => cores(),
            "hostOS" => 5,
            "mobile" => 0,
            "screenWidthDip" | "screenWidthPx" => CANVAS_W as i32,
            "screenHeightDip" | "screenHeightPx" => CANVAS_H as i32,
            "dpi" => 96,
            "memoryMB" => 2048,
            _ => 0,
        }
    })?;
    l.func_wrap("env", "read_host_info_string", |mut c: Caller<'_, St>, k: i32,
     dst: i32, cap: i32| {
        let key = rstr(&c, k as u32);
        let v = INFO.iter().find(|(n, _)| *n == key).map(|(_, v)| *v).unwrap_or("");
        wstr(&mut c, dst as u32, v, cap as u32);
    })?;
    l.func_wrap("env", "has_keyboard", || 1i32)?;
    l.func_wrap("env", "has_mouse", || 1i32)?;
    l.func_wrap("env", "is_mobile_browser", || 0i32)?;
    l.func_wrap("env", "is_touch_mobile", || 0i32)?;
    l.func_wrap("env", "is_touch_screen_device", || 0i32)?;
    l.func_wrap("env", "js_web_client_page_visible", || 1i32)?;

    // VFS
    l.func_wrap("env", "rbx_fs_set_base_url", |mut c: Caller<'_, St>, p: i32| {
        c.data_mut().base = rstr(&c, p as u32);
        eprintln!("[fs  ] base url {}", c.data().base);
    })?;
    l.func_wrap("env", "rbx_fs_register_asset", |mut c: Caller<'_, St>, p: i32, size: i32| {
        let path = rstr(&c, p as u32);
        c.data_mut().assets.insert(path, size as u32);
    })?;
    l.func_wrap("env", "rbx_fs_add_file", |mut c: Caller<'_, St>, p: i32, data: i32,
     len: i32| {
        let path = rstr(&c, p as u32);
        let b = rbytes(&c, data as u32, len as u32);
        c.data_mut().files.insert(path, b);
    })?;
    l.func_wrap("env", "rbx_fs_preload_asset", |mut c: Caller<'_, St>, p: i32,
     data: i32, len: i32| {
        let path = rstr(&c, p as u32);
        let b = rbytes(&c, data as u32, len as u32);
        eprintln!("[fs  ] preload {path} ({} bytes)", b.len());
        c.data_mut().preload.insert(path, b);
    })?;
    // civil time. The glue writes a 7-word breakdown (sec, min, hour, day,
    // month, year-1900, yday); we run in UTC and report "UTC" for the zone.
    l.func_wrap("env", "_gmtime_js", |mut c: Caller<'_, St>, secs: i64, p: i32| -> i32 {
        let n = c.data_mut().gm_calls;
        c.data_mut().gm_calls += 1;
        let cn = CLK.with(|c| { let x = c.get(); c.set(x + 1); x });
        if cn < 30 {
            eprintln!("[clk ] gmtime in={secs}");
        }
        if n < 6 {
            let (y, mo, d, h, mi, se) = civil_from(secs);
            eprintln!("[gm  ] in={secs} -> {y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{se:02}");
        }
        civil_write(&mut c, secs as f64, p as u32) as i32
    })?;
    l.func_wrap("env", "_localtime_js", |mut c: Caller<'_, St>, secs: i64, p: i32| -> i32 {
        civil_write(&mut c, secs as f64, p as u32) as i32
    })?;
    l.func_wrap("env", "_mktime_js", |mut c: Caller<'_, St>, p: i32| -> i64 {
        let mut w = [0u32; 9];
        {
            let d = c.data().mem.data();
            for (i, slot) in w.iter_mut().enumerate() {
                *slot = unsafe { *(d[p as usize + i * 4].get() as *const u32) };
            }
        }
        let (y, mo, d, h, mi, se) = (w[5] as i64 + 1900, w[4], w[3], w[2] as i64, w[1] as i64, w[0] as i64);
        let out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
        let n = c.data().mk_calls;
        c.data_mut().mk_calls += 1;
        let cn = CLK.with(|c| { let x = c.get(); c.set(x + 1); x });
        if cn < 30 {
            eprintln!("[clk ] mktime");
        }
        if n < 8 {
            eprintln!("[mkt ] {y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{se:02} -> {out} (wday={} yday={} isdst={})", w[6], w[7], w[8]);
        }
        out
    })?;
    l.func_wrap("env", "_tzset_js",
        |mut c: Caller<'_, St>, a: i32, b: i32, d: i32, e: i32| {
            let cn = CLK.with(|c| { let x = c.get(); c.set(x + 1); x });
            if cn < 30 {
                eprintln!("[clk ] tzset {a:x} {b:x} {d:x} {e:x}");
            }
            for p in [a, b, d, e] {
                if p != 0 {
                    wstr(&mut c, p as u32, "UTC", 8);
                }
            }
        })?;

    // main-thread pthread/TLS bring-up: the glue calls the module's own
    // _emscripten_thread_init + tls_init here (lo), so everything after this
    // point has a valid __pthread and TLS block.
    l.func_wrap_async("env", "_emscripten_init_main_thread_js",
        |c: Caller<'_, St>, (ptr,): (i32,)| {
            let p = c.data().rt.as_ref().expect("rt").store;
            let fut: Box<dyn std::future::Future<Output = wasmtime::Result<()>> + Send> = Box::new(async move {
                let st = p.get();
                let inst = st.data().inst.ok_or_else(|| wasmtime::Error::msg("no instance"))?;
                let ti = inst.get_typed_func::<(i32, i32, i32, i32, i32, i32), ()>(
                    &mut *st, "_emscripten_thread_init")?;
                ti.call_async(&mut *st, (ptr, 1, 1, 1, 262144, 0)).await?;
                let tls = inst.get_typed_func::<(), (i32,)>(&mut *st, "_emscripten_tls_init")?;
                tls.call_async(&mut *st, ()).await?;
                // with TLS live, the module knows its own pthread: arm the main
                // mailbox exactly as the glue's `Er` does
                if let Ok(ps) = inst.get_typed_func::<(), (i32,)>(&mut *st, "pthread_self") {
                    if let Ok((me,)) = ps.call_async(&mut *st, ()).await {
                        if me != 0 {
                            let me = me as u32;
                            st.data_mut().main_ptr = Some(me);
                            unsafe { *(st.data().mem.data()[me as usize + 112].get() as *mut u32) = 1 }
                            eprintln!("[mbx ] main pthread {me:#x} (armed)");
                        }
                    }
                }
                eprintln!("[wasm] main thread init, tls done");
                Ok(())
            });
            fut
        })?;

    // wasi_snapshot_preview1: the module only needs wall clock, no environ, exit
    l.func_wrap("wasi_snapshot_preview1", "clock_time_get",
        |mut c: Caller<'_, St>, _id: i32, _prec: i64, out: i32| -> i32 {
            let ns = (now_ms() * 1e6) as u64;
            wbytes(&mut c, out as u32, &ns.to_le_bytes());
            0
        })?;
    l.func_wrap("wasi_snapshot_preview1", "environ_sizes_get",
        |mut c: Caller<'_, St>, n: i32, sz: i32| -> i32 {
            wbytes(&mut c, n as u32, &0u32.to_le_bytes());
            wbytes(&mut c, sz as u32, &0u32.to_le_bytes());
            0
        })?;
    l.func_wrap("wasi_snapshot_preview1", "environ_get", |_a: i32, _b: i32| 0i32)?;
    l.func_wrap("wasi_snapshot_preview1", "proc_exit", |c: Caller<'_, St>, code: i32| -> wasmtime::Result<()> {
        eprintln!("[wasi] exit {code}");
        Err(wasmtime::Error::msg("proc_exit"))
    })?;
    l.func_wrap("wasi_snapshot_preview1", "random_get",
        |mut c: Caller<'_, St>, p: i32, n: i32| -> i32 {
            let mut b = vec![0u8; n.max(0) as usize];
            if getrandom(&mut b).is_err() {
                return 29; // ENOSYS-ish
            }
            wbytes(&mut c, p as u32, &b);
            0
        })?;
    Ok(())
}

fn civil_write(c: &mut Caller<'_, St>, secs: f64, p: u32) -> u32 {
    let t = secs as i64;
    let (y, mo, d, h, mi, se) = civil_from(t);
    let wday = weekday(t);
    let yday = day_of_year(y, mo, d);
    // musl wasm32 struct tm order: sec, min, hour, mday, mon, year, wday, yday, isdst
    let words = [se, mi, h, d as i64, mo as i64, y - 1900, wday, yday, 0];
    for (i, w) in words.iter().enumerate() {
        wbytes(c, p + i as u32 * 4, &(*w as u32).to_le_bytes());
    }
    0
}

// --- civil date maths (Howard Hinnant's algorithms, UTC only) ---
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = ((m as i64 + 9) % 12) as i64;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

fn civil_from(secs: i64) -> (i64, u32, u32, i64, i64, i64) {
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let z = days + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = if m <= 2 { y + 1 } else { y };
    (y, m, d, rem / 3600, (rem % 3600) / 60, rem % 60)
}

/// 0 = Sunday, matching struct tm.
fn weekday(secs: i64) -> i64 {
    (secs.div_euclid(86400) + 4).rem_euclid(7)
}

fn day_of_year(y: i64, m: u32, d: u32) -> i64 {
    const CUM: [i64; 13] = [0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    CUM[m as usize] + if leap && m > 2 { 1 } else { 0 } + d as i64 - 1
}
