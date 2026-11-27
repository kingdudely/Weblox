// WebTransport (RNA) layer for the clean C11 host.
//
// The engine's rbx_wasm_rna_* imports are implemented here.  Host-to-engine
// notifications (on_connect_success, on_receive_ring_slot, ...) go through a
// signal callback that the host wires to its embind/deferred-call bridge; no
// wasm table index or guest address is hardcoded in this layer.
#ifndef WEBLOX_WEBTRANSPORT_H
#define WEBLOX_WEBTRANSPORT_H

#include "host.h"

#include <stdbool.h>

typedef void (*wt_signal_fn)(const char* name, const u32* args, u32 argc);

bool wt_init(void);
void wt_shutdown(void);

// Drive QUIC/h3 I/O and feed received data into the engine's receive ring.
void wt_pump(void);

// Install the host-to-engine callback sink (usually embind deferred calls).
void wt_set_signal(wt_signal_fn fn);

u32 wt_active_connections(void);

// Compare a peer certificate SHA-256 (lowercase hex) with pins from joinScript.
bool wt_pin_matches(u32 connection_id, const char* sha256_hex);

#endif
