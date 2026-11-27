// Browser-side setup before createWebClient().  Mirrors
// v2-wasmtime/src/main.rs::prepare(), using the embind invoke API instead of
// Wasmtime's typed calls.  Everything goes through names exported by the
// module; no guest index or address is hardcoded.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "embind.h"
#include "guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace weblox::prepare {
namespace eb = weblox::embind;

namespace {

// Live settings normally come from clientsettingscdn.  This is the documented
// minimum blob with RbxTransport enabled, used when WC_SETTINGS is not set.
constexpr const char* kMinimalSettings =
    R"({"applicationSettings":{"BaseUrl":"https://www.roblox.com/",)"
    R"("FIntWasmMemoryPages":"65536","FFlagUseRbxTransportClient3":"True",)"
    R"("FFlagUseRbxTransportServer":"True"},)"
    R"("RobloxLocaleId":"en-us","GameLocaleId":"en-us"})";

int call_string(w2c_WebClient* inst, const char* name, const char* value) {
  const int rc = eb::call(inst, name, {eb::Arg::str(value)});
  std::fprintf(stderr, "[prep] %s(%s) -> %d\n", name, value, rc);
  return rc;
}

// set_preloaded_flags takes a raw byte pointer, not an embind string struct.
int call_bytes(w2c_WebClient* inst, const char* name, const char* bytes) {
  const std::size_t length = std::strlen(bytes);
  const u32 address = w2c_WebClient_malloc(inst, static_cast<u32>(length + 1));
  if (!address) return 0;
  if (auto* target = weblox::guest::ptr(address, length + 1))
    std::memcpy(target, bytes, length + 1);
  const int rc = eb::call(inst, name, {eb::Arg::i32(address)});
  w2c_WebClient_free(inst, address);
  std::fprintf(stderr, "[prep] %s(%zu bytes) -> %d\n", name, length, rc);
  return rc;
}

}  // namespace

bool run(w2c_WebClient* inst) {
  if (!eb::ready(inst)) {
    std::fprintf(stderr, "[prep] embind not ready\n");
    eb::dump(inst);
    return false;
  }
  call_string(inst, "set_pretend_platform", "desktop");
  call_string(inst, "set_roblox_base_url", "https://www.roblox.com/");
  call_string(inst, "set_channel", "");
  call_string(inst, "set_browser_tracker_id", "");
  {
    const int rc = eb::call(inst, "set_account_locales",
                            {eb::Arg::str("en-us"), eb::Arg::str("en-us")});
    std::fprintf(stderr, "[prep] set_account_locales -> %d\n", rc);
  }

  const char* settings = std::getenv("WC_SETTINGS");
  if (!settings || !*settings) settings = kMinimalSettings;
  call_bytes(inst, "set_preloaded_flags", settings);

  const char* place_text = std::getenv("WC_PLACE");
  const unsigned long long place =
      place_text && *place_text ? std::strtoull(place_text, nullptr, 10) : 189707;
  const int rc = eb::call(inst, "set_place_id", {eb::Arg::i64(place)});
  std::fprintf(stderr, "[prep] set_place_id(%llu) -> %d\n", place, rc);
  return true;
}

}  // namespace weblox::prepare
