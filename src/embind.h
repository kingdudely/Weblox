// embind registry for the clean C11 host.
//
// The engine registers its host-facing API at __wasm_call_ctors time via the
// `_embind_register_*` imports.  Those registrations are captured here and
// invoked later through the instance's own indirect function table.  No import
// indices, no table indices and no guest addresses are hardcoded.
#ifndef WEBLOX_EMBIND_H
#define WEBLOX_EMBIND_H

#include "host.h"

#include <stdint.h>

// Variadic argument wrapper.  Calls end with WEBLOX_ARG_END, e.g.
//   weblox_embind_call(inst, "set_place_id",
//                      weblox_arg_i64(189707), WEBLOX_ARG_END);
// Strings are copied into guest memory as embind's `std::string` wire struct
// ([u32 length][bytes][NUL]) before the call and freed afterwards.
typedef enum {
  WEBLOX_KIND_END = 0,
  WEBLOX_KIND_I32,
  WEBLOX_KIND_I64,
  WEBLOX_KIND_F64,
  WEBLOX_KIND_STRING
} weblox_arg_kind;

typedef struct {
  weblox_arg_kind kind;
  union {
    int32_t i32;
    int64_t i64;
    double f64;
    const char* str;
  } as;
} weblox_arg;

static inline weblox_arg weblox_arg_i32(int32_t value) {
  weblox_arg a;
  a.kind = WEBLOX_KIND_I32;
  a.as.i32 = value;
  return a;
}

static inline weblox_arg weblox_arg_i64(int64_t value) {
  weblox_arg a;
  a.kind = WEBLOX_KIND_I64;
  a.as.i64 = value;
  return a;
}

static inline weblox_arg weblox_arg_f64(double value) {
  weblox_arg a;
  a.kind = WEBLOX_KIND_F64;
  a.as.f64 = value;
  return a;
}

static inline weblox_arg weblox_arg_string(const char* value) {
  weblox_arg a;
  a.kind = WEBLOX_KIND_STRING;
  a.as.str = value ? value : "";
  return a;
}

#define WEBLOX_ARG_END ((weblox_arg){WEBLOX_KIND_END, {0}})

// Returns 1 when the binding was found, its argument count matched and the
// invoker was callable; 0 otherwise.  Errors are logged with a [embind] tag.
int weblox_embind_call(w2c_WebClient* inst, const char* name, ...);

// Introspection (useful for bring-up logs).
int weblox_embind_count(void);
const char* weblox_embind_name(int index);
void weblox_embind_dump(void);

// Clear the registry before re-instantiating in the same process.
void weblox_embind_reset(void);

#endif
