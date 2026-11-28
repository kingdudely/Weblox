// Shared-memory atomics: Atomics.wait/notify on top of SDL3 mutexes and
// conditions.  Every address is a guest u32 translated with guest.h; there are
// no host pointers or hardcoded guest offsets in this file.
#ifndef WEBLOX_ATOMICS_H
#define WEBLOX_ATOMICS_H

#include "host.h"

// timeout is in nanoseconds.  (u64)-1 / values that wrap to a negative s64
// mean "wait forever"; 0 means "check once".  Return codes mirror JS:
// 0 = woken, 1 = value differed, 2 = timed out.
u32 weblox_atomic_wait32(wasm_rt_shared_memory_t* memory, u32 address,
                         u32 expected, u64 timeout);
u32 weblox_atomic_wait64(wasm_rt_shared_memory_t* memory, u32 address,
                         u64 expected, u64 timeout);
u32 weblox_atomic_notify(wasm_rt_shared_memory_t* memory, u32 address,
                         u32 count);

// JSPI bridge: f64 milliseconds (NaN/negative-clamped, like Atomics.waitAsync)
// converted to nanoseconds.  The main thread pumps SDL events and runs the
// deferred queue inline on this very fiber while it waits.
u32 weblox_atomic_wait_js(wasm_rt_shared_memory_t* memory, u32 address,
                          u32 expected, s64 timeout_ns);

// Host-pump entry: resume parked main-thread waiters whose notify arrived,
// whose deadline expired, or whose guest word changed.  Must be called from
// the host stack; safe to call from any thread (no-ops off the main thread).
void weblox_atomic_pump(void);

int weblox_atomic_init(void);   // idempotent; returns 1 on success
void weblox_atomic_free(void);  // safe to call more than once

#endif
