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
        |mut c: Caller<'_, St>, addr: i32, expected: i32, timeout: f64| -> i32 {
            crate::trace::hit("futex_wait");
            let mem = c.data().mem.clone();
            let addr = addr as u32;
            let expected = expected as u32;
            if word(&mem, addr) != expected {
                return 1;
            }
            let dur = if timeout > 0.0 && timeout < 1e12 {
                Some(std::time::Duration::from_secs_f64(timeout / 1000.0))
            } else {
                None
            };
            let deadline = dur.map(|d| std::time::Instant::now() + d);
            loop {
                if word(&mem, addr) != expected {
                    return 0;
                }
                if let Some(d) = deadline {
                    if std::time::Instant::now() >= d {
                        return 2;
                    }
                }
                // drain anything the mailbox is holding, then park this fibre
                let _ = crate::fibre::block_on(crate::imports::mailbox::drop_in(&mut c));
                crate::fibre::block_on(crate::fibre::sleep(&mut c, std::time::Duration::from_millis(1)));
            }
        })?;
    Ok(())
}
