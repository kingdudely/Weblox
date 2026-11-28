// JSPI Atomics.waitAsync bridge.
//
// The guest calls this when a futex wait would block the browser's main
// thread.  On the main thread the wait parks the calling fibre and hands
// control back to the host event loop (atomics.c owns the waiter and its
// wake-up); workers use the ordinary SDL condition-variable wait.
//
// Bounded runs are enforced by main.c's WEBLOX_RUN_MS deadline, so there is no
// watchdog here: a normal infinite wait must not terminate the process.
#include "atomics.h"
#include "fibers.h"
#include "guest.h"
#include "abort_debug.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

u32 w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi(struct w2c_env* env,
                                                    u32 address, u32 value,
                                                    f64 timeout_ms) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi", (u64)(uintptr_t)env, (u64)address, (u64)value, (u64)timeout_ms);
  (void)env;
  // This host has one shared linear memory (g_mem, used by every instance).
  wasm_rt_shared_memory_t* memory = &g_mem;

  // Atomics.waitAsync clamps negative durations to zero and treats NaN or a
  // huge timeout as infinite, matching the browser glue.
  s64 timeout_ns = -1;
  if (!(timeout_ms > 0.0)) {
    timeout_ns = 0;
  } else if (timeout_ms < (f64)(INT64_MAX / SDL_NS_PER_MS)) {
    timeout_ns = (s64)ceil(timeout_ms * (f64)SDL_NS_PER_MS);
  }
  fprintf(stderr, "[futex] wait addr=%u want=%u timeout=%.1fms main=%d "
                  "fiber=%p key=%u\n",
          address, value, timeout_ms, weblox_fiber_on_main_thread(),
          weblox_fiber_current_handle(), weblox_fiber_current_key());

  if (!weblox_fiber_on_main_thread())
    return weblox_atomic_wait32(memory, address, value, (u64)timeout_ns);
  return weblox_atomic_wait_js(memory, address, value, timeout_ns);
}
