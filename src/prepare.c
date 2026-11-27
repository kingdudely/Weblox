/* What WasmPlayer.js does before createWebClient(), ported from
 * v2-wasmtime/src/main.rs::prepare and v3-wasm2c/impl/prepare.c.
 *
 * Order follows the proven Rust host: embind configuration first (platform,
 * base URL, channel, tracker id, locales, preloaded settings and place id),
 * then the VFS bootstrap (asset metadata, InExperience preload, appStorage
 * files and the asset base URL).
 */
#include "prepare.h"
#include "embind.h"
#include "fs.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WebClientApp settings as the page fetches them, with the two keys the page
 * always overrides.  WC_RAKNET selects the variant that disables the native
 * transport, matching v2-wasmtime/src/main.rs::settings. */
#define WEBLOX_SETTINGS_MIN                                                   \
  "{\"applicationSettings\":{\"BaseUrl\":\"https://www.roblox.com/\","        \
  "\"FIntWasmMemoryPages\":\"32768\"},"                                       \
  "\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}"
#define WEBLOX_SETTINGS_MIN_RAK                                               \
  "{\"applicationSettings\":{\"BaseUrl\":\"https://www.roblox.com/\","        \
  "\"FIntWasmMemoryPages\":\"32768\",\"FFlagUseRbxTransportClient3\":"         \
  "\"False\",\"FFlagUseRbxTransportServer\":\"False\"},"                      \
  "\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}"

static const char* default_settings(void) {
  const char* e = getenv("WC_RAKNET");
  return (e && *e) ? WEBLOX_SETTINGS_MIN_RAK : WEBLOX_SETTINGS_MIN;
}

// WC_SETTINGS may hold the JSON itself or the path to a settings file.
static const char* settings_json(char** owned) {
  *owned = NULL;
  const char* value = getenv("WC_SETTINGS");
  if (!value || !*value) return default_settings();
  const char* p = value;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  if (*p == '{') return value;

  enum { CAP = 1024 * 1024 };
  char* buf = (char*)malloc(CAP);
  if (!buf) return default_settings();
  long n = weblox_fs_read_file(value, buf, CAP - 1);
  if (n <= 0) {
    fprintf(stderr, "[prep] cannot read WC_SETTINGS file %s\n", value);
    free(buf);
    return default_settings();
  }
  buf[n] = 0;
  *owned = buf;
  return buf;
}

static int64_t place_id(void) {
  const char* e = getenv("WC_PLACE");
  if (!e || !*e) return 189707;
  char* end = NULL;
  long long v = strtoll(e, &end, 10);
  if (end == e || v <= 0) return 189707;
  return (int64_t)v;
}

static int call(w2c_WebClient* inst, const char* name, weblox_arg arg) {
  int ok = weblox_embind_call(inst, name, arg, WEBLOX_ARG_END);
  if (!ok) fprintf(stderr, "[prep] embind call %s failed\n", name);
  return ok;
}

int weblox_prepare(w2c_WebClient* inst) {
  if (!inst) {
    fprintf(stderr, "[prep] no instance\n");
    return -1;
  }

  char* owned_settings = NULL;
  const char* settings = settings_json(&owned_settings);
  const int64_t place = place_id();
  int failures = 0;
  const char* no_bind = getenv("WC_NOBIND");

  if (!no_bind || !*no_bind) {
    failures += !call(inst, "set_pretend_platform", weblox_arg_string("desktop"));
    failures += !call(inst, "set_roblox_base_url",
                      weblox_arg_string("https://www.roblox.com/"));
    failures += !call(inst, "set_channel", weblox_arg_string(""));
    failures += !call(inst, "set_browser_tracker_id", weblox_arg_string(""));
    if (!weblox_embind_call(inst, "set_account_locales",
                            weblox_arg_string("en-us"),
                            weblox_arg_string("en-us"), WEBLOX_ARG_END)) {
      fprintf(stderr, "[prep] embind call set_account_locales failed\n");
      ++failures;
    }
    // The page passes the settings blob as a guest std::string struct.
    failures += !call(inst, "set_preloaded_flags", weblox_arg_string(settings));
    failures += !call(inst, "set_place_id", weblox_arg_i64(place));
  } else {
    fprintf(stderr, "[prep] WC_NOBIND: skipping embind configuration\n");
  }

  failures += weblox_vfs_bootstrap(inst) != 0;

  fprintf(stderr, "[prep] place=%lld settings_bytes=%u bindings=%d failures=%d\n",
          (long long)place, (unsigned)strlen(settings), weblox_embind_count(),
          failures);
  free(owned_settings);
  return failures ? -1 : 0;
}
