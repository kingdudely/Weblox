// Bounds-checked access to guest linear memory.  Addresses are u32 values
// from the wasm ABI; never host pointers and never hardcoded constants.
#ifndef WEBLOX_GUEST_H
#define WEBLOX_GUEST_H

#include "host.h"

#include <stddef.h>
#include <string.h>

static inline unsigned char* guest_ptr(u32 address, size_t need) {
  unsigned char* base = (unsigned char*)weblox_mem_base();
  uint64_t size = weblox_mem_size();
  if (!base || address > size || need > size - address) return NULL;
  return base + address;
}

// Returns a NUL-terminated view into guest memory, or "" if invalid.
static inline const char* guest_str(u32 address) {
  const char* start = (const char*)guest_ptr(address, 1);
  if (!start || !*start) return "";
  uint64_t limit = weblox_mem_size() - address;
  for (uint64_t i = 0; i < limit; ++i)
    if (!start[i]) return start;
  return "";
}

static inline void guest_put_u32(u32 address, u32 value) {
  unsigned char* p = guest_ptr(address, 4);
  if (p) memcpy(p, &value, 4);
}

static inline void guest_put_u64(u32 address, u64 value) {
  unsigned char* p = guest_ptr(address, 8);
  if (p) memcpy(p, &value, 8);
}

#endif
