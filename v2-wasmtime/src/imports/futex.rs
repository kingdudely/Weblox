//! Atomics.waitAsync.  The wait suspends the calling fibre; the ticker in
//! `fibre::block_on` resumes it, which is what the page gets from the JSPI
//! promise.
use wasmtime::{Caller, Linker, SharedMemory};

use crate::state::St;

fn word(mem: &SharedMemory, addr: u32) -> u32 {
    unsafe { *(mem.data()[addr as usize].get() as *const u32) }
}

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    l.func_wrap("env", "__asyncjs__rbxFutexWaitMainJspi",
        |mut c: Caller<'_, St>, addr: i32, expected: i32, timeout: f64| -> wasmtime::Result<i32> {
            crate::trace::hit("futex_wait");
            if std::env::var_os("WC_FUTEX").is_some() {
                eprintln!("[futc] {} enter", std::thread::current().name().unwrap_or("?"));
            }
            let mem = c.data().mem.clone();
            let addr = addr as u32;
            let expected = expected as u32;
            if word(&mem, addr) != expected {
                return Ok(1);
            }
            let dur = if timeout > 0.0 && timeout < 1e12 {
                Some(std::time::Duration::from_secs_f64(timeout / 1000.0))
            } else {
                None
            };
            if std::env::var_os("WC_FUTEX").is_some() {
                eprintln!("[fut ] {} addr={addr:#x} expect={expected:#x} val={:#x} timeout={timeout}",
                          std::thread::current().name().unwrap_or("?"), word(&mem, addr));
            }
            // An unbounded wait still gets a short deadline: `Atomics.waitAsync`
            // wakes on a notify we cannot observe, so spurious wake-ups are the
            // only way to stay correct.
            let deadline = Some(std::time::Instant::now() + dur.unwrap_or(std::time::Duration::from_millis(2)));
            let mut n: u64 = 0;
            loop {
                if std::env::var_os("WC_FUTEX").is_some() && n % 200 == 0 {
                    eprintln!("[fut*] {addr:#x} iter={n} val={:#x}", word(&mem, addr));
                }
                n += 1;
                if let Ok(t) = std::env::var("WC_TRAP_AT") {
                    if let Ok(t) = t.parse::<u64>() {
                        let ok = match std::env::var("WC_TRAP_THREAD") {
                            Ok(name) => std::thread::current().name() == Some(name.as_str()),
                            Err(_) => true,
                        };
                        let late = match std::env::var("WC_TRAP_AFTER") {
                            Ok(ms) => {
                                use std::sync::OnceLock;
                                static START: OnceLock<std::time::Instant> = OnceLock::new();
                                START.get_or_init(std::time::Instant::now).elapsed().as_millis()
                                    >= ms.parse::<u128>().unwrap_or(0)
                            }
                            Err(_) => true,
                        };
                        let ok = ok && (std::env::var_os("WC_TRAP_SUBMIT").is_none()
                            || crate::imports::boot::submit_thread());
                        if n == t && ok && late {
                            eprintln!("[pc] thread={}", std::thread::current().name().unwrap_or("?"));
                            unsafe {
                                let mut fp: usize;
                                std::arch::asm!("mov {}, rbp", out(reg) fp);
                                for _ in 0..14 {
                                    if fp == 0 {
                                        break;
                                    }
                                    let ret = *((fp + 8) as *const usize);
                                    eprintln!("[pc] {ret:x}");
                                    fp = *(fp as *const usize);
                                }
                            }
                            return Err(wasmtime::Error::msg("futex spin"));
                        }
                    }
                }
                if n == 20000 {
                    eprintln!("[fut*] spin addr={addr:#x} expect={expected:#x} val={:#x} timeout={timeout}", word(&mem, addr));
                    crate::diag::dump_futex(&mem, addr);
                    for (t, c, sp, w) in crate::fibre::TRACE_T.lock().unwrap().iter() {
                        eprintln!("[dump]   thread {t} cur={c:#x} sp={sp:#x} waiting={w}");
                    }
                }
                if word(&mem, addr) != expected {
                    return Ok(0);
                }
                if let Some(d) = deadline {
                    if std::time::Instant::now() >= d {
                        return Ok(2);
                    }
                }
                // drain anything the mailbox is holding, then spin: a sleep
                // would need the turn pump, which is busy inside `main`
                let _ = crate::fibre::block_on(crate::imports::mailbox::pump_mailbox(&mut c));
                // the page leaves this call suspended and keeps running the
                // main loop, which is what services the HTTP queue; standing
                // still here instead starves it
                if c.data().is_main {
                    crate::nested_frame();
                } else {
                    std::thread::yield_now();
                }
            }
        })?;
    Ok(())
}
