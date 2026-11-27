// Host identity and device queries.  Ported from v3-wasm2c/host_info.c to the
// C++ host; the values mirror WasmPlayer.js's hostInfo so the engine sees a
// ordinary desktop browser.  All values are available behind abstractions,
// never as guest pointers supplied by the caller beyond the ABI addresses.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "platform.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
constexpr std::uint32_t kCanvasW = 1280;
constexpr std::uint32_t kCanvasH = 720;

unsigned logical_cores() {
  static const unsigned n = [] {
    unsigned h = std::thread::hardware_concurrency();
    return h ? h : 1u;
  }();
  return n;
}

const char* info_string(const char* key) {
  if (!key) return "";
  if (!std::strcmp(key, "hostOS") || !std::strcmp(key, "osName"))
    return "Linux";
  if (!std::strcmp(key, "browserName")) return "Chrome";
  if (!std::strcmp(key, "browserVersion")) return "140.0.0.0";
  if (!std::strcmp(key, "gpuVendor")) return "Mesa";
  if (!std::strcmp(key, "gpuRenderer")) return "llvmpipe";
  if (!std::strcmp(key, "screenWidthDip") || !std::strcmp(key, "screenWidthPx"))
    return "1280";
  if (!std::strcmp(key, "screenHeightDip") ||
      !std::strcmp(key, "screenHeightPx"))
    return "720";
  if (!std::strcmp(key, "dpi")) return "96";
  if (!std::strcmp(key, "memoryMB")) return "2048";
  if (!std::strcmp(key, "cpuLogicalCount")) return "1";
  return "";
}
}  // namespace

extern "C" {

u32 w2c_env_read_host_info_int(struct w2c_env*, u32 key) {
  const char* k = weblox::guest::str(key);
  if (!std::strcmp(k, "hostOS")) return 5;  // Linux
  if (!std::strcmp(k, "mobile")) return 0;
  if (!std::strcmp(k, "screenWidthDip") || !std::strcmp(k, "screenWidthPx"))
    return kCanvasW;
  if (!std::strcmp(k, "screenHeightDip") ||
      !std::strcmp(k, "screenHeightPx"))
    return kCanvasH;
  if (!std::strcmp(k, "dpi")) return 96;
  if (!std::strcmp(k, "memoryMB")) return 2048;
  if (!std::strcmp(k, "cpuLogicalCount")) return logical_cores();
  return static_cast<u32>(std::atoi(info_string(k)));
}

void w2c_env_read_host_info_string(struct w2c_env*, u32 key, u32 out,
                                   u32 capacity) {
  if (!capacity) return;
  const char* value = info_string(weblox::guest::str(key));
  auto* dst = weblox::guest::ptr(out, capacity);
  if (!dst) return;
  std::size_t n = std::strlen(value);
  if (n > capacity - 1) n = capacity - 1;
  std::memcpy(dst, value, n);
  dst[n] = 0;
}

u32 w2c_env_get_hardware_concurrency(struct w2c_env*) { return logical_cores(); }
u32 w2c_env_emscripten_num_logical_cores(struct w2c_env*) {
  return logical_cores();
}

// The module reserves the full 4 GiB up front, so any u32 request fits.
u32 w2c_env_emscripten_get_heap_max(struct w2c_env*) { return 0xFFFFFFFFu; }
u32 w2c_env_emscripten_resize_heap(struct w2c_env*, u32 want) {
  return static_cast<u64>(want) <= weblox_mem_size() ? 1u : 0u;
}

u32 w2c_env_emscripten_get_canvas_element_size(struct w2c_env*, u32, u32 w,
                                               u32 h) {
  weblox::guest::put_u32(w, kCanvasW);
  weblox::guest::put_u32(h, kCanvasH);
  return 1;
}
u32 w2c_env_emscripten_set_canvas_element_size(struct w2c_env*, u32, u32,
                                               u32) {
  return 1;
}
f64 w2c_env_get_effective_device_pixel_ratio(struct w2c_env*) { return 1.0; }
f64 w2c_env_emscripten_get_device_pixel_ratio(struct w2c_env*) { return 1.0; }

u32 w2c_env_has_keyboard(struct w2c_env*) { return 1; }
u32 w2c_env_has_mouse(struct w2c_env*) { return 1; }
u32 w2c_env_is_mobile_browser(struct w2c_env*) { return 0; }
u32 w2c_env_is_touch_mobile(struct w2c_env*) { return 0; }
u32 w2c_env_is_touch_screen_device(struct w2c_env*) { return 0; }
u32 w2c_env_js_web_client_page_visible(struct w2c_env*) { return 1; }

}  // extern "C"
