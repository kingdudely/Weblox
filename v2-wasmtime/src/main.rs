//! Headless host for Roblox's WebClient.wasm on wasmtime.
//!
//! The engine is the stock browser module; this process supplies what Chrome
//! and WebClient.js supply: a shared memory, the `env` imports, the embind
//! bootstrap, and an event loop.  Suspending imports run as wasmtime async
//! host functions, which is the same suspend/resume the browser gets from
//! `WebAssembly.Suspending`.
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use futures::executor::block_on;
use wasmtime::*;

mod binds;
mod diag;
mod fibre;
mod trace;
mod imports;
mod state;

use state::{St, CANVAS_H, CANVAS_W};

fn wasm_path() -> String { std::env::var("WC_WASM").unwrap_or_else(|_| "/home/john/rr/WebClient.wasm".into()) }
const CACHE: &str = "data/cache/webclient.cwasm";
const CACHE_STAMP: &str = "data/cache/webclient.stamp";
const PAGES: u32 = 32768; // 2 GiB, as the page does (FIntWasmMemoryPages)
const WEB_BASE: &str = "https://www.roblox.com/";
const ASSET_BASE: &str =
    "https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/";

fn place_arg() -> Result<i64> {
    let a = std::env::var("WC_PLACE")
        .ok()
        .or_else(|| std::env::args().skip(1).find(|a| a.chars().all(|c| c.is_ascii_digit())))
        .unwrap_or_default();
    let a = a.trim().to_string();
    if a.is_empty() || !a.chars().all(|c| c.is_ascii_digit()) {
        return Err(anyhow!("usage: WC_COOKIE='...' ./alcohol <placeId>"));
    }
    Ok(a.parse()?)
}

/// `.ROBLOSECURITY`, from the environment or the default cookie file.
fn cookie() -> Result<String> {
    let c = match std::env::var("WC_COOKIE") {
        Ok(v) if !v.trim().is_empty() => v,
        _ => {
            let f = std::env::var("WC_COOKIE_FILE").unwrap_or_else(|_| "/home/john/king.txt".into());
            std::fs::read_to_string(&f).map_err(|e| anyhow!("no cookie (WC_COOKIE or {f}: {e})"))?
        }
    };
    let c = c.trim().trim_start_matches(".ROBLOSECURITY=").to_string();
    if c.is_empty() {
        return Err(anyhow!("empty .ROBLOSECURITY value"));
    }
    Ok(c)
}

fn config() -> Result<Config> {
    let dev = std::env::var_os("WC_DEV").is_some();
    let mut c = Config::new();
    c.wasm_threads(true);
    c.shared_memory(true);
    c.async_support(true);
    // Every guest fibre runs on its own host stack but wasmtime keeps one stack
    // limit per store, so the prologue check must be loose enough that a stale
    // limit from the stack we came from never trips it.  Overflow is caught by
    // the fibre guard page instead.
    c.max_wasm_stack(1 << 30);
    c.async_stack_size(1 << 30);
    c.wasm_tail_call(true);
    c.wasm_simd(true);
    c.wasm_relaxed_simd(true);
    c.wasm_bulk_memory(true);
    c.wasm_reference_types(true);
    c.wasm_multi_value(true);
    c.wasm_exceptions(true);
    c.cranelift_opt_level(if dev { OptLevel::None } else { OptLevel::Speed });
    c.parallel_compilation(true);
    // The unwinder walks one contiguous stack; our fibres are separate stacks
    // per activation, so leave it off unless asked (it panics on a trap).
    c.wasm_backtrace(std::env::var_os("WC_BT").is_some());
    c.wasm_backtrace_details(if dev {
        WasmBacktraceDetails::Enable
    } else {
        WasmBacktraceDetails::Disable
    });

    // The CPU is the runtime: no per-access software masking. Guard pages make
    // the MMU the bounds check and the null page the trap.
    unsafe {
        c.cranelift_flag_set("enable_heap_access_spectre_mitigation", "false");
        c.cranelift_flag_set("enable_table_access_spectre_mitigation", "false");
    }
    c.cranelift_nan_canonicalization(false);
    c.memory_reservation(PAGES as u64 * 65536);
    c.memory_guard_size(2 << 30);
    c.guard_before_linear_memory(true);
    if std::env::var_os("WC_NATIVE").is_some() {
        // only valid while the cached artifact and the host agree
        unsafe { c.cranelift_flag_enable("has_avx2") };
    }
    Ok(c)
}

fn stamp() -> String {
    let md = std::fs::metadata(wasm_path()).map(|m| (m.len(), m.modified().ok())).ok();
    format!("{:?}", md)
}

fn module(engine: &Engine) -> Result<Module> {
    let st = stamp();
    if let Ok(prev) = std::fs::read_to_string(CACHE_STAMP) {
        if prev == st {
            let buf = std::fs::read(CACHE)?;
            return Ok(unsafe { Module::deserialize(engine, &buf) }?);
        }
    }
    eprintln!("[wasm] compiling {} (cached builds are instant after this)", wasm_path());
    let bytes = std::fs::read(wasm_path())?;
    let t = Instant::now();
    let pre = engine.precompile_module(&bytes)?;
    eprintln!("[wasm] compiled in {} ms ({} bytes)", t.elapsed().as_millis(), pre.len());
    std::fs::create_dir_all("data/cache")?;
    std::fs::write(CACHE, &pre)?;
    std::fs::write(CACHE_STAMP, st)?;
    Ok(unsafe { Module::deserialize(engine, &pre) }?)
}

fn main() {
    if let Err(e) = run() {
        eprintln!("[fatal] {e:#}");
        let mut src = e.source();
        while let Some(s) = src {
            eprintln!("[fatal]   caused by: {s}");
            src = s.source();
        }
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let place = place_arg()?;
    let cookie = cookie()?;
    let engine = Engine::new(&config()?)?;
    let module = module(&engine)?;
    let mem = SharedMemory::new(&engine, MemoryType::shared(PAGES, PAGES))?;

    let mut linker = Linker::new(&engine);
    imports::embind::register(&mut linker)?;
    imports::boot::register(&mut linker)?;
    imports::fetch::register(&mut linker)?;
    imports::http::register(&mut linker)?;
    imports::futex::register(&mut linker)?;
    fibre::register(&mut linker)?;
    imports::mailbox::register(&mut linker)?;
    imports::stubs::register(&mut linker)?;
    imports::thread::register(&mut linker)?;

    let mut store = Box::new(Store::new(&engine, St::new(mem.clone())));
    let sp = fibre::PStore(&mut *store as *mut Store<St>);
    store.data_mut().rt = Some(fibre::Rt::new(sp));
    store.data_mut().engine = Some(engine.clone());
    diag::start();
    imports::boot::start_clock();
    if trace::enabled() {
        std::thread::spawn(|| loop {
            std::thread::sleep(std::time::Duration::from_secs(2));
            let h = trace::hist();
            let t: Vec<&str> = h.iter().rev().take(12).rev().cloned().collect();
            eprintln!("[t] {}", t.join(" "));
        });
    }
    store.data_mut().is_main = true;
    store.data_mut().module = Some(module.clone());
    store.data_mut().place = place;
    store.data_mut().cookie = Some(cookie);
    linker.define(&mut *store, "env", "memory", Extern::SharedMemory(mem))?;
    let inst = fibre::block_on(linker.instantiate_async(&mut *store, &module))?;
    store.data_mut().inst = Some(inst);
    if std::env::var_os("WC_SLOT0").is_some() {
        let t = inst.get_table(&mut *store, "__indirect_function_table").unwrap();
        if let Some(f) = t.get(&mut *store, std::env::var("WC_SLOT0").unwrap().parse::<u64>().unwrap()) {
            t.set(&mut *store, 0, f)?;
            eprintln!("[tbl ] slot0 set; now {}", match t.get(&mut *store, 0) { Some(wasmtime::Ref::Func(Some(_))) => "func", _ => "null" });
        }
    }
    if std::env::var_os("WC_TBL").is_some() {
        let t = inst.get_table(&mut *store, "__indirect_function_table").unwrap();
        let n = t.size(&mut *store);
        for i in [0u64, 446] {
            eprintln!("[tbl ] slot {i} = {:?}", t.get(&mut *store, i));
        }
        let mut s = String::new();
        for i in 0..6u64 {
            s.push_str(match t.get(&mut *store, i) {
                Some(wasmtime::Ref::Func(Some(_))) => " F",
                Some(wasmtime::Ref::Func(None)) => " n",
                Some(_) => " ?",
                None => " -",
            });
        }
        eprintln!("[tbl ] size={n} slots={s}");
    }

    let s: &mut Store<St> = &mut store;
    let ctors = inst.get_typed_func::<(), ()>(&mut *s, "__wasm_call_ctors")?;
    if std::env::var_os("WC_NOCtors").is_none() {
    if let Err(e) = ctors.call(&mut *s, ()) {
        eprintln!("[fatal] ctors threw; registered {} bindings so far", store.data().binds.len());
        dump_exception(&mut *store);
        trace::dump();
        return Err(e.into());
    }
    eprintln!("[wasm] ctors ok, {} bindings registered", store.data().binds.len());
    } else { eprintln!("[wasm] ctors skipped, {} binds pre-existing", store.data().binds.len()); }
    if let Err(e) = fibre::block_on(prepare(&mut store, &inst)) {
        eprintln!("[prep] failed: {e:#}");
        return Err(e);
    }

    let main_fn = inst.get_typed_func::<(i32, i32), i32>(&mut *store, "__main_argc_argv")?;
    let argv = fibre::block_on(guest_argv(&mut *store, &inst))?;
    let p = store.data().rt.as_ref().expect("rt").store;
    let mf = main_fn;
    let argv = argv;
    let task: fibre::Task = Box::pin(async move {
        if let Err(e) = mf.call(p.get(), (1, argv as i32)) {
            static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            if N.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 5 {
                eprintln!("[main] threw: {e:#}");
            }
        }
    });
    eprintln!("[wasm] main on fibre");
    fibre::call_fibre(&mut *store, 0x7000_0001, 4 * 1024 * 1024, task)?;
    eprintln!("[wasm] main fibre done");
    let rc = match Ok::<i32, wasmtime::Error>(0) {
        Ok(r) => r,
        Err(_) if store.data().exited => {
            eprintln!("[wasm] main exited to live runtime");
            0
        }
        Err(e) => {
            eprintln!("[fatal] main trapped: {e:#}");
            dump_exception(&mut *store);
        trace::dump();
            eprintln!(
                "[fatal] binds={} assets={} files={} preload={}",
                store.data().binds.len(),
                store.data().assets.len(),
                store.data().files.len(),
                store.data().preload.len()
            );
            return Err(e.into());
        }
    };
    eprintln!("[wasm] main returned {rc}");

    let iter = inst.get_typed_func::<(), ()>(&mut *store, "main_loop_iteration")?;
    let secs: f64 = std::env::var("WC_LOOP_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(60_000.0);
    let deadline = Instant::now() + Duration::from_secs_f64(secs / 1000.0);
    std::thread::spawn(|| diag::watch(3));
    let mut turns = 0u64;
    fibre::host_root(&mut *store);
    while Instant::now() < deadline {
        if turns % 100 == 0 {
            eprintln!("[iter] turn {turns} at {}ms", deadline.saturating_duration_since(Instant::now()).as_millis());
        }
        turns += 1;

        let fid = 0x8000_0000 | (turns as u32 & 0xffff);
        let p = store.data().rt.as_ref().expect("rt").store;
        let it = iter.clone();
        let task: fibre::Task = Box::pin(async move {
            let st = p.get();
            imports::mailbox::drop_in(&mut *st).await;
            if let Err(e) = it.call(&mut *st, ()) {
                static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if n < 5 {
                    eprintln!("[iter] threw: {e:#}");
                }
            }
        });
        if let Err(e) = fibre::call_fibre(&mut *store, fid, 2 * 1024 * 1024, task) {
            eprintln!("[iter] turn {turns} failed: {e:#}");
            fibre::shutdown(&mut *store);
            return Err(e.into());
        }
        diag::checkpoint();
        std::thread::sleep(Duration::from_millis(1));
    }
    eprintln!("[wasm] {turns} turns; last event {}", diag::last());
    trace::dump();
    eprintln!("[wasm] loop done");
    for (t, c, sp) in fibre::TRACE_T.lock().unwrap().iter() {
        eprintln!("[dump] thread {t} cur={c:#x} sp={sp:#x}");
    }
    fibre::shutdown(&mut *store);
    Ok(())
}

fn dump_exception(store: &mut Store<St>) {
    let Some(e) = store.take_pending_exception() else {
        return;
    };
    match e.tag(&mut *store) {
        Ok(tag) => {
            let ty = tag.ty(&mut *store).clone();
            eprintln!("[exn ] tag={:?}", tag);
            eprintln!("[exn ] type={ty:?}");
            for (i, p) in ty.ty().params().enumerate() {
                if let Ok(v) = e.field(&mut *store, i) {
                    eprintln!("[exn ]   [{i}] {p:?} = {v:?}");
                    if let Val::I32(ptr) = v {
                        let d = store.data().mem.data();
                        {
                            let mut b = String::new();
                            for k in 0..96usize {
                                let c = unsafe { *d[ptr as usize + 4 + k].get() };
                                b.push(if (0x20..0x7f).contains(&c) { c as char } else { '.' });
                            }
                            eprintln!("[exn ]   raw+4: {b}");
                        }
                        for off in [4u32, 8, 12] {
                            let w = unsafe { *(d[ptr as usize + off as usize].get() as *const u32) };
                            if w > 0x1000 {
                                let mut b = String::new();
                                for k in 0..200usize {
                                    let c = unsafe { *d[w as usize + k].get() };
                                    if c == 0 { break }
                                    b.push(if (0x20..0x7f).contains(&c) { c as char } else { '.' });
                                }
                                eprintln!("[exn ]   +{off} -> {w:#x}: {b}");
                            }
                        }
                        let d = store.data().mem.data();
                        let base = ptr as usize;
                        let mut run = String::new();
                        for k in 0..256usize {
                            let b = unsafe { *d[base + k].get() };
                            if (0x20..0x7f).contains(&b) {
                                run.push(b as char);
                            } else {
                                if run.len() >= 4 {
                                    eprintln!("[exn ]     str @+{k}: {run:?}");
                                }
                                run.clear();
                            }
                        }
                    }
                }
            }
        }
        Err(err) => eprintln!("[exn ] no tag: {err}"),
    }
}

async fn guest_argv(store: &mut Store<St>, inst: &Instance) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let p = malloc.call(&mut *store, (16,))? as u32;
    let arr = malloc.call(&mut *store, (8,))? as u32;
    let d = store.data().mem.data();
    for (i, b) in b"/content/.keep\0".iter().enumerate() {
        unsafe { *d[p as usize + i].get() = *b };
    }
    unsafe { *(d[arr as usize].get() as *mut u32) = p };
    unsafe { *(d[arr as usize + 4].get() as *mut u32) = 0 };
    Ok(arr)
}

/// What WasmPlayer.js does before createWebClient().
async fn prepare(store: &mut Store<St>, inst: &Instance) -> Result<()> {
    let a = str_arg(store, inst, "desktop").await?;
    binds::call(store, inst, "set_pretend_platform", &[a]).await?;
    let a = str_arg(store, inst, WEB_BASE).await?;
    binds::call(store, inst, "set_roblox_base_url", &[a]).await?;
    let a = str_arg(store, inst, "").await?;
    binds::call(store, inst, "set_channel", &[a]).await?;
    let a = str_arg(store, inst, "").await?;
    binds::call(store, inst, "set_browser_tracker_id", &[a]).await?;
    let a = str_arg(store, inst, "en-us").await?;
    let b = str_arg(store, inst, "en-us").await?;
    binds::call(store, inst, "set_account_locales", &[a, b]).await?;

    // The page preloads the live WebClientApp settings and only then adds the
    // two host keys; the transport/join flags live in that blob.
    let flags = settings().await;
    let fp = binds::guest_string_bytes(store, inst, flags.as_bytes()).await?;
    binds::call(store, inst, "set_preloaded_flags", &[imports::embind::Arg::I32(fp)]).await?;

    let place: i64 = std::env::var("WC_PLACE").ok().and_then(|v| v.parse().ok()).unwrap_or(189707);
    binds::call(store, inst, "set_place_id", &[imports::embind::Arg::I64(place as u64)]).await?;

    // VFS bootstrap: the metadata tree, then the patch model the loader expects
    // to find already present.
    let fs_base = std::env::var("WC_CONTENT").unwrap_or_else(|_| "data/content".into());
    let reg = inst.get_typed_func::<(i32, i32), i32>(&mut *store, "rbx_fs_register_asset")?;
    let mut n = 0;
    let meta = if std::env::var_os("WC_NOREG").is_some() { String::new() } else { std::fs::read_to_string("data/meta.json").unwrap_or_default() };
    for ent in meta.split('{').skip(1) {
        let path = field(ent, "path");
        let size: u32 = field(ent, "size").parse().unwrap_or(1);
        if path.is_empty() {
            continue;
        }
        let p = import_bytes(store, inst, path.as_bytes()).await?;
        reg.call(&mut *store, (p as i32, size as i32))?;
        n += 1;
    }
    eprintln!("[fs  ] registered {n} asset paths under {fs_base}");

    let pre = inst.get_typed_func::<(i32, i32, i32), i32>(&mut *store, "rbx_fs_preload_asset")?;
    for rel in ["/models/InExperience/InExperience.rbxm",
                "/models/InExperience/InExperience_checksum"] {
        let disk = format!("{fs_base}{rel}");
        let Ok(body) = std::fs::read(&disk) else { continue };
        let p = import_bytes(store, inst, rel.as_bytes()).await?;
        let d = import_bytes(store, inst, &body).await?;
        let rc = pre.call(&mut *store, (p as i32, d as i32, body.len() as i32))?;
        eprintln!("[fs  ] preload {rel} ({} bytes) -> {rc}", body.len());
    }

    let app = import_bytes(store, inst, br#"{"WebLogin":"2","RobloxLocaleId":"en-us","GameLocaleId":"en-us"}"#).await?;
    let add = inst.get_typed_func::<(i32, i32, i32), i32>(&mut *store, "rbx_fs_add_file")?;
    let path = import_bytes(store, inst, b"/appData/LocalStorage/appStorage.json").await?;
    add.call(&mut *store, (path as i32, app as i32, 61))?;

    let sb = inst.get_typed_func::<(i32,), ()>(&mut *store, "rbx_fs_set_base_url")?;
    let bp = import_bytes(store, inst, ASSET_BASE.as_bytes()).await?;
    sb.call(&mut *store, (bp as i32,))?;
    eprintln!("[prep] base now {:?}", store.data().base);

    eprintln!("[prep] place={place} canvas={CANVAS_W}x{CANVAS_H} assets={} preload={} files={}", store.data().assets.len(), store.data().preload.len(), store.data().files.len());
    Ok(())
}

/// WebClientApp settings, as `WasmPlayer.js` fetches them, with the two keys
/// the page always overrides.  Falls back to the bare minimum.
async fn settings() -> String {
    const URL: &str = "https://clientsettingscdn.roblox.com/v2/settings/application/WebClientApp";
    const MIN: &str = r#"{"applicationSettings":{"BaseUrl":"https://www.roblox.com/","FIntWasmMemoryPages":"32768"},"RobloxLocaleId":"en-us","GameLocaleId":"en-us"}"#;
    let raw = std::process::Command::new("curl")
        .args(["-fsSL", "--max-time", "10", URL])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned());
    let Some(body) = raw else {
        eprintln!("[prep] settings fetch failed");
        return MIN.to_string();
    };
    let Some(open) = body.find("\"applicationSettings\":{").map(|i| i + "\"applicationSettings\":".len()) else {
        eprintln!("[prep] settings had no applicationSettings");
        return MIN.to_string();
    };
    // walk to the matching brace of the applicationSettings object
    let mut depth = 0i32;
    let mut in_str = false;
    let mut esc = false;
    let mut close = None;
    for (k, ch) in body[open..].char_indices() {
        match ch {
            _ if esc => esc = false,
            '\\' if in_str => esc = true,
            '"' => in_str = !in_str,
            '{' if !in_str => depth += 1,
            '}' if !in_str => {
                depth -= 1;
                if depth == 0 {
                    close = Some(open + k);
                    break;
                }
            }
            _ => {}
        }
    }
    let Some(close) = close else {
        eprintln!("[prep] settings object unterminated");
        return MIN.to_string();
    };
    eprintln!("[prep] {} bytes of settings", body.len());
    let mut out = String::with_capacity(body.len() + 128);
    out.push_str(&body[..=open]);
    out.push_str(r#""BaseUrl":"https://www.roblox.com/","FIntWasmMemoryPages":"32768","#);
    out.push_str(&body[open + 1..close]);
    out.push_str(r#"},"RobloxLocaleId":"en-us","GameLocaleId":"en-us"}"#);
    out
}

fn field(ent: &str, name: &str) -> String {
    let key = format!("\"{name}\":");
    let Some(i) = ent.find(&key) else { return String::new() };
    let rest = &ent[i + key.len()..];
    let rest = rest.trim_start();
    if let Some(r) = rest.strip_prefix('"') {
        let Some(j) = r.find('"') else { return String::new() };
        return r[..j].to_string();
    }
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    rest[..end].to_string()
}

async fn str_arg(store: &mut Store<St>, inst: &Instance, s: &str) -> Result<imports::embind::Arg> {
    Ok(imports::embind::Arg::I32(binds::guest_string(store, inst, s).await?))
}

// small alias so the flags call reads clearly
/* arg shorthand */

/// Allocate guest memory and copy `b` plus a NUL (engine strings).
async fn import_bytes(store: &mut Store<St>, inst: &Instance, b: &[u8]) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let p = malloc.call(&mut *store, (b.len() as i32 + 1,))? as u32;
    if p == 0 {
        return Err(anyhow!("guest malloc failed"));
    }
    let d = store.data().mem.data();
    for (i, x) in b.iter().enumerate() {
        unsafe { *d[p as usize + i].get() = *x };
    }
    unsafe { *d[p as usize + b.len()].get() = 0 };
    Ok(p)
}
