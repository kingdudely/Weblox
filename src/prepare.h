// Browser-equivalent setup performed before the engine main loop.
#ifndef WEBLOX_PREPARE_H
#define WEBLOX_PREPARE_H

#include "host.h"

/* Exact call sequence for main.c (all symbols come from WC.h):
 *
 *   w2c_WebClient inst;
 *   memset(&inst, 0, sizeof inst);
 *   wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);
 *   weblox_instance = &inst;
 *   w2c_WebClient_0x5F_wasm_call_ctors(&inst);  // embind registrations happen here
 *   if (weblox_prepare(&inst) != 0) { ... }     // log/report, then continue
 *   // ... pump the main loop (weblox_deferred_pump / main_turn) ...
 *   wasm2c_WebClient_free(&inst);
 *
 * weblox_prepare returns 0 on success and -1 if any step failed (details are
 * logged).  It reads WC_PLACE (default 189707), WC_SETTINGS (inline JSON or a
 * file path), WC_META, WC_CONTENT, WC_ASSET_BASE and WC_NOBIND.  It must be
 * called after __wasm_call_ctors so the embind table is populated.
 */
int weblox_prepare(w2c_WebClient* inst);

#endif
