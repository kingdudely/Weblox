/* impl/runtime.c - keepalive counter and the "unwind" escape used by the
 * engine to leave main() while keeping the runtime alive.
 *
 * The glue implements emscripten_exit_with_live_runtime as
 * `{ RuntimeKeepalive.push(); throw 'unwind' }`; native code cannot throw a
 * string across the wasm boundary, so this throws a host-owned wasm exception
 * tag instead.  wc_protected() recognises it and reports a clean exit.
 */
#include "../host/host.h"

int wc_keepalive;

/* host-owned tag; never registered with the module, only compared by us */
static int g_unwind_tag;
void* wc_unwind_tag(void) { return &g_unwind_tag; }

void wc_impl_emscripten_runtime_keepalive_push(struct w2c_env* a0) { (void)a0; wc_keepalive++; }
void wc_impl_0x5Femscripten_runtime_keepalive_clear(struct w2c_env* a0) { (void)a0; wc_keepalive = 0; }

void wc_impl_emscripten_exit_with_live_runtime(struct w2c_env* a0) {
  (void)a0;
  wc_keepalive++;
  u32 zero = 0;
  wasm_rt_load_exception(&g_unwind_tag, 4, &zero);
  wasm_rt_throw();
}
