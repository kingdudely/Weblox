// VFS / asset bootstrap for the C++ host.
//
// Port of the second half of v2-wasmtime/src/main.rs::prepare(): register the
// meta.json asset tree, preload the InExperience model, install appStorage.json
// and set the CDN base URL.  Everything is done through exports declared in
// WC.h; no import stubs or guest addresses are involved.
//
// Call once, after `__wasm_call_ctors` (so embind bindings and guest malloc are
// live) and before `__main_argc_argv`.  The caller keeps ownership of `inst`.

#pragma once

struct w2c_WebClient;

namespace weblox::vfs {

// Returns false only when a required guest allocation fails (including a null
// instance).  Missing assets, malformed manifest entries and failed engine
// calls are logged with a "[vfs]" prefix and do not abort the bootstrap.
bool bootstrap(w2c_WebClient* inst);

}  // namespace weblox::vfs
