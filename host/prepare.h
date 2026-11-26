// Engine bootstrap for the C++ host, ported from
// v2-wasmtime/src/main.rs::prepare() (and guest_argv()).
//
// The browser host sets a handful of embind globals, registers the VFS tree
// described by meta.json, and only then calls __main_argc_argv.  Doing the same
// here is what lets initialize() get past the settings/place-id lookups.
//
// `state.vmctx` is the store/vmctx both for the embind invokers (via the
// indirect table in HostState) and for the exported rbx_fs_* functions.
#pragma once

#include <cstdint>
#include <string>

namespace host {
struct HostState;
}

namespace weblox {

// Run the full prepare() sequence. Returns false if a required embind binding
// was missing or a guest allocation failed; each failure is reported on stderr
// with `[prep]`. `cookie` is accepted for parity with the host call site (the
// RNA route fetch already uses it) and is never printed or copied to wasm.
bool prepare(host::HostState& state, uint64_t place_id,
             const std::string& cookie, const std::string& meta_path,
             const std::string& content_base);

// malloc(16) for "/content/.keep\0" plus an argv array [str, 0], exactly like
// v2-wasmtime/src/main.rs::guest_argv(). Returns 0 when malloc fails/traps.
uint32_t guest_argv(host::HostState& state);

}  // namespace weblox
