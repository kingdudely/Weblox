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
                        if n == t {
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
                // drain anything the mailbox is holding, then park this fibre
                let _ = crate::fibre::block_on(crate::imports::mailbox::pump_mailbox(&mut c));
                crate::fibre::block_on(crate::fibre::sleep(&mut c, std::time::Duration::from_millis(1)));
            }
        })?;
    Ok(())
}
