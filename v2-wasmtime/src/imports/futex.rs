//! Attomics.waitAsync. A worker blocks for real; the main thread suspends its
//! fibre and lets the executor run whatever could wake it, which is the same
//! thing JSPI gives the page.
use anyhow::Result;
use wasmtime::{Caller, Linker, SharedMemory};

use crate::state::St;

fn word(mem: &SharedMemory, addr: u32) -> u32 {
    unsafe { *(mem.data()[addr as usize].get() as *const u32) }
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap_async("env", "__asyncjs__rbxFutexWaitMainJspi",
        |mut c: Caller<'_, St>, (addr, expected, timeout): (i32, i32, f64)|
        Box::new(async move {
            crate::trace::hit("futex_wait");
            let mem = c.data().mem.clone();
            let addr = addr as u32;
            let expected = expected as u32;
            crate::diag::event(format!(
                "futex addr=0x{addr:x} expected=0x{expected:x} timeout={timeout}"));

            if word(&mem, addr) != expected {
                return Ok(1i32);
            }
            let dur = if timeout > 0.0 && timeout < 1e12 {
                Some(std::time::Duration::from_secs_f64(timeout / 1000.0))
            } else {
                None
            };

            if !c.data().is_main {
                // a worker has no executor of its own
                let r = mem.atomic_wait32(addr as u64, expected, dur);
                static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if n < 20 {
                    eprintln!("[wfut] worker addr=0x{addr:x} exp={expected} -> {r:?}");
                }
                return Ok(match r {
                    Ok(wasmtime::WaitResult::Ok) => 0,
                    Ok(wasmtime::WaitResult::Mismatch) => 1,
                    _ => 2,
                });
            }

            let deadline = dur.map(|d| std::time::Instant::now() + d);
            let mut iters = 0u64;
            loop {
                iters += 1;
                if iters % 50 == 0 && std::env::var_os("WC_DUMP").is_some() {
                    crate::diag::dump_futex(&mem, addr);
                }
                if word(&mem, addr) != expected {
                    return Ok(0i32);
                }
                if let Some(d) = deadline {
                    if std::time::Instant::now() >= d {
                        return Ok(2i32);
                    }
                }
                crate::imports::mailbox::drop_in(&mut c).await;
                crate::fibre::sleep(&mut c, std::time::Duration::from_millis(1)).await;
            }
        }))?;
    Ok(())
}
