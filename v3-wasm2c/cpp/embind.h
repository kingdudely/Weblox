// Calling the engine's embind bindings from the C++ host.
//
// The engine registers its host-facing API (set_place_id,
// set_preloaded_flags, ...) through the `_embind_register_*` imports while its
// constructors run.  embind.cpp captures those registrations and `call()`
// invokes a registered function by name through the module's own indirect
// function table.  No guest index or guest address is ever hardcoded.
//
// Calling convention (mirrors v3-wasm2c/embind.c and the dynCall used by the
// generated WebClient.js glue):
//
//   invoker(module_instance, id, arg0, arg1, ...)
//
// The host-side slot array is `[id, arg0, ...]`; `slots[0]` is the method id
// captured at registration time.  Argument k takes its wire type from
// signature byte `sig[k + 2]` (`sig[0]` is the return type and `sig[1]` is the
// hidden id/receiver slot).  String arguments are guest buffers laid out as
// `[u32 length][bytes][NUL]`, allocated with `w2c_WebClient_malloc` and freed
// after the call.
//
// Example prepare step, ported from v2-wasmtime/src/main.rs::prepare() (the
// order matters: these are the values WasmPlayer.js installs before
// createWebClient()):
//
//   #include "embind.h"
//   void prepare(w2c_WebClient* inst, const char* flags_json) {
//     namespace eb = weblox::embind;
//     if (!eb::ready(inst)) return;             // ctors must have run
//     eb::call(inst, "set_pretend_platform", {eb::Arg::str("desktop")});
//     eb::call(inst, "set_roblox_base_url",
//              {eb::Arg::str("https://www.roblox.com/")});
//     eb::call(inst, "set_channel", {eb::Arg::str("")});
//     eb::call(inst, "set_browser_tracker_id", {eb::Arg::str("")});
//     eb::call(inst, "set_account_locales",
//              {eb::Arg::str("en-us"), eb::Arg::str("en-us")});
//     eb::call(inst, "set_preloaded_flags", {eb::Arg::str(flags_json)});
//     eb::call(inst, "set_place_id", {eb::Arg::i64(189707)});
//   }
//
// `call()` installs its own wasm_rt_try unwind target, so callers do not need
// to wrap it in a guarded() helper.  It returns 1 on success and 0 when the
// name is unknown, the arity disagrees with the signature, a guest allocation
// fails, or wasm throws.  Non-void results are ignored (as in the Rust host).
// A single f32/f64 argument is passed with a typed signature; bindings with
// several float arguments use the embind.c integer-slot convention and are
// not currently used by the join path.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

struct w2c_WebClient;

namespace weblox::embind {

/// One embind argument.  Scalars are kept as their wire bits; strings are
/// copied, so temporaries are safe for the duration of `call()`.
class Arg {
 public:
  Arg() = default;

  static Arg i32(std::uint32_t v) {
    Arg a;
    a.kind_ = Kind::I32;
    a.bits_ = v;
    return a;
  }
  static Arg i64(std::uint64_t v) {
    Arg a;
    a.kind_ = Kind::I64;
    a.bits_ = v;
    return a;
  }
  static Arg f32(float v) {
    Arg a;
    a.kind_ = Kind::F32;
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    a.bits_ = bits;
    return a;
  }
  static Arg f64(double v) {
    Arg a;
    a.kind_ = Kind::F64;
    std::memcpy(&a.bits_, &v, sizeof a.bits_);
    return a;
  }
  static Arg str(const char* s) {
    Arg a;
    a.kind_ = Kind::Str;
    a.str_ = s ? s : "";
    return a;
  }
  static Arg str(const std::string& s) {
    Arg a;
    a.kind_ = Kind::Str;
    a.str_ = s;
    return a;
  }

  /// True when the binding signature asks for a guest std::string pointer.
  bool is_string() const { return kind_ == Kind::Str; }
  /// Wire bits for scalar slots (i32 zero-extended, f32/f64 bit patterns).
  std::uint64_t bits() const { return bits_; }
  const std::string& string_value() const { return str_; }

 private:
  enum class Kind { I32, I64, F32, F64, Str };
  Kind kind_ = Kind::I32;
  std::uint64_t bits_ = 0;
  std::string str_;
};

/// True once the engine's constructor pass has registered bindings and the
/// module's indirect function table is live.
bool ready(w2c_WebClient* inst);

/// Invoke a registered binding.  Returns 1 on success, 0 on failure.
/// Guarded internally against wasm exceptions (wasm_rt_try).
int call(w2c_WebClient* inst, const char* name,
         std::initializer_list<Arg> args);

/// Log every captured registration and type to stderr for diagnosis.
void dump(w2c_WebClient* inst = nullptr);

}  // namespace weblox::embind
