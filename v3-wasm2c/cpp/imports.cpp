// Real C++ implementations for core imports.  Symbols have C linkage so the
// generated C library finds them exactly like the generated stubs would.

extern "C" {
#include "WC.h"
}

#include "mem.h"
#include "platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {
const char* guest_ptr(u32 address) {
  auto* base = static_cast<const char*>(weblox_mem_base());
  if (!base || address >= weblox_mem_size()) return "";
  return base + address;
}

std::string guest_string(u32 address) {
  const char* start = guest_ptr(address);
  if (!*start) return {};
  std::uint64_t limit = weblox_mem_size() - address;
  std::string out;
  for (std::uint64_t i = 0; i < limit && start[i]; ++i) out.push_back(start[i]);
  return out;
}
}  // namespace

extern "C" void w2c_env_emscripten_console_log(struct w2c_env*, u32 ptr) {
  std::fprintf(stderr, "[log ] %s\n", guest_string(ptr).c_str());
}

extern "C" void w2c_env_emscripten_console_warn(struct w2c_env*, u32 ptr) {
  std::fprintf(stderr, "[warn] %s\n", guest_string(ptr).c_str());
}

extern "C" void w2c_env_emscripten_console_error(struct w2c_env*, u32 ptr) {
  std::fprintf(stderr, "[err ] %s\n", guest_string(ptr).c_str());
}

extern "C" f64 w2c_env_emscripten_get_now(struct w2c_env*) {
  return static_cast<f64>(weblox::platform::now_ms());
}

extern "C" f64 w2c_env_emscripten_date_now(struct w2c_env*) {
  using namespace std::chrono;
  return static_cast<f64>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

extern "C" u32 w2c_wasi__snapshot__preview1_random_get(
    struct w2c_wasi__snapshot__preview1*, u32 ptr, u32 len) {
  auto* base = static_cast<unsigned char*>(weblox_mem_base());
  if (!base || ptr + len > weblox_mem_size()) return 1;
  std::random_device rd;
  for (u32 i = 0; i < len; ++i) base[ptr + i] = static_cast<unsigned char>(rd());
  return 0;
}

extern "C" u32 w2c_wasi__snapshot__preview1_environ_sizes_get(
    struct w2c_wasi__snapshot__preview1*, u32 count_ptr, u32 size_ptr) {
  auto* base = static_cast<unsigned char*>(weblox_mem_base());
  if (!base) return 1;
  *reinterpret_cast<u32*>(base + count_ptr) = 0;
  *reinterpret_cast<u32*>(base + size_ptr) = 0;
  return 0;
}

extern "C" u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1*, u32, u32) {
  return 0;
}

extern "C" u32 w2c_wasi__snapshot__preview1_clock_time_get(
    struct w2c_wasi__snapshot__preview1*, u32 clock_id, u64, u32 out_ptr) {
  auto* base = static_cast<unsigned char*>(weblox_mem_base());
  if (!base) return 1;
  using namespace std::chrono;
  u64 ns;
  if (clock_id == 0) {
    ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
             .count();
  } else {
    ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
             .count();
  }
  *reinterpret_cast<u64*>(base + out_ptr) = ns;
  return 0;
}

extern "C" void w2c_wasi__snapshot__preview1_proc_exit(
    struct w2c_wasi__snapshot__preview1*, u32 code) {
  std::exit(static_cast<int>(code));
}
