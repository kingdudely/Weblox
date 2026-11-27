// Bounds-checked access to guest linear memory.  All addresses are u32 from
// the wasm ABI; they are never host pointers and never hardcoded indices.
#pragma once

#include "mem.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace weblox::guest {

inline unsigned char* ptr(std::uint32_t address, std::size_t need = 1) {
  auto* base = static_cast<unsigned char*>(weblox_mem_base());
  if (!base || address > weblox_mem_size() ||
      need > weblox_mem_size() - address)
    return nullptr;
  return base + address;
}

inline const char* str(std::uint32_t address) {
  const char* start = reinterpret_cast<const char*>(ptr(address));
  if (!start || !*start) return "";
  std::size_t limit = static_cast<std::size_t>(weblox_mem_size() - address);
  for (std::size_t i = 0; i < limit; ++i)
    if (!start[i]) return start;
  return "";
}

inline std::string string(std::uint32_t address) { return std::string(str(address)); }

inline void put_u32(std::uint32_t address, std::uint32_t value) {
  if (auto* p = ptr(address, 4)) std::memcpy(p, &value, 4);
}

inline void put_u64(std::uint32_t address, std::uint64_t value) {
  if (auto* p = ptr(address, 8)) std::memcpy(p, &value, 8);
}

}  // namespace weblox::guest
