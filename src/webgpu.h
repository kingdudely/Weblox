// WebGPU backend behind Dawn's C API.  The pinned WC.h exposes no wgpu*
// imports yet; this facade is the single place future generated imports can
// be wired to, so no other host file needs to know about Dawn.
#ifndef WEBLOX_WEBGPU_H
#define WEBLOX_WEBGPU_H

#include "host.h"

#include <stdbool.h>

bool webgpu_init(void);
bool webgpu_ready(void);
void webgpu_shutdown(void);

// Opaque WGPUInstance, for future surface/swap-chain integration.
void* webgpu_instance_handle(void);

// Generic import entry point: arguments are raw wasm ABI values from WC.h.
// Unimplemented operations log once with a [gpu] prefix and return 0.
u32 webgpu_dispatch(const char* name, const u32* args, u32 argc);

#endif
