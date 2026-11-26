/* impl/prepare.c - what WasmPlayer.js does before createWebClient().
 *
 * onRuntimeInitialized() in the glue is, in order:
 *   rbx_fs_set_base_url(base)
 *   rbx_fs_register_asset(path, size)   for every entry of the asset metadata
 *   rbx_fs_preload_asset(path, ptr, len) for prefetched bodies
 *   rbx_fs_add_file("/appData/LocalStorage/*.json", ...)
 *   set_roblox_base_url / set_channel / set_pretend_platform /
 *   set_preloaded_flags / set_account_locales / set_place_id /
 *   set_browser_tracker_id
 *
 * The metadata pass is what makes the engine's VFS contain the content tree;
 * bodies stay on disk and are pulled by js_fetch_file when the engine reads.
 */
#include "../host/host.h"
#include "../host/plat.h"

static u32 gstr(const char* s) {
  size_t n = strlen(s) + 1;
  u32 p = w2c_WebClient_malloc(g_inst, (u32)n);
  if (p) memcpy(wc_ptr(p), s, n);
  return p;
}

static void add_file(const char* path, const char* data) {
  u32 p = gstr(path), d = gstr(data);
  if (!p || !d) return;
  u32 rc = w2c_WebClient_rbx_fs_add_file(g_inst, p, d, (u32)strlen(data));
  if (g_wc_verbose) fprintf(stderr, "[prep] add_file %s -> %u\n", path, rc);
  w2c_WebClient_free(g_inst, p);
  w2c_WebClient_free(g_inst, d);
}

/* ---- asset metadata ---- */

static const char* meta_path(void) {
  const char* e = pl_getenv("WC_META");
  return (e && *e) ? e : "data/meta.json";
}

/* pull "<path>" and <size> out of one {"path": ..., "size": ...} object */
static int meta_next(const char** pp, char* path, size_t cap, u64* size) {
  const char* s = *pp;
  while (*s && *s != '{') s++;
  if (!*s) return 0;
  const char* p = strstr(s, "\"path\"");
  if (!p) return 0;
  p = strchr(p, ':');
  if (!p) return 0;
  p = strchr(p, '"');
  if (!p) return 0;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) path[n++] = *p++;
  path[n] = 0;
  const char* z = strstr(p, "\"size\"");
  if (!z) return 0;
  z = strchr(z, ':');
  if (!z) return 0;
  *size = strtoull(z + 1, NULL, 10);
  *pp = p;
  return 1;
}

static int register_meta(int* count) {
  void* buf = NULL;
  size_t len = 0;
  char full[1024];
  snprintf(full, sizeof full, "%s", meta_path());
  pl_file* f = pl_open_read(full);
  if (!f) {
    fprintf(stderr, "[prep] no metadata at %s\n", full);
    return 0;
  }
  int64_t sz = pl_size(f);
  buf = malloc((size_t)sz + 1);
  if (!buf) { pl_close(f); return 0; }
  pl_read(f, buf, (size_t)sz);
  pl_close(f);
  ((char*)buf)[sz] = 0;

  const char* p = buf;
  char path[512];
  u64 size = 0;
  int n = 0;
  while (meta_next(&p, path, sizeof path, &size)) {
    u32 gp = gstr(path);
    if (!gp) continue;
    w2c_WebClient_rbx_fs_register_asset(g_inst, gp, (u32)size);
    w2c_WebClient_free(g_inst, gp);
    n++;
  }
  free(buf);
  *count = n;
  return n > 0;
}

void wc_prepare(void) {
  extern int wc_embind_invoke(const char*, int, ...);

  if (g_wc_base && *g_wc_base) {
    u32 b = gstr(g_wc_base);
    if (b) {
      w2c_WebClient_rbx_fs_set_base_url(g_inst, b);
      w2c_WebClient_free(g_inst, b);
    }
  }

  int n = 0;
  if (register_meta(&n)) fprintf(stderr, "[prep] registered %d asset paths\n", n);
  else { /* keep the boot alive without a metadata file */
    const char* seed[] = { "/content/.keep", "/ExtraContent/.keep" };
    for (unsigned i = 0; i < 2; i++) {
      u32 p = gstr(seed[i]);
      if (p) { w2c_WebClient_rbx_fs_register_asset(g_inst, p, 1u); w2c_WebClient_free(g_inst, p); }
    }
  }

  /* Preload the in-experience patch model: its loader verifies the body and
   * does not fetch it on demand. */
  {
    const char* rel[] = { "/models/InExperience/InExperience.rbxm",
                          "/models/InExperience/InExperience_checksum" };
    for (int i = 0; i < 2; i++) {
      char p[512];
      snprintf(p, sizeof p, "data/content%s", rel[i]);
      int64_t sz = pl_size_path(p);
      if (sz <= 0) continue;
      pl_file* f = pl_open_read(p);
      if (!f) continue;
      u32 gp = gstr(rel[i]);
      u32 mem = w2c_WebClient_malloc(g_inst, (u32)sz);
      if (gp && mem && pl_read(f, wc_ptr(mem), (size_t)sz) == (size_t)sz) {
        u32 rc = w2c_WebClient_rbx_fs_preload_asset(g_inst, gp, mem, (u32)sz);
        fprintf(stderr, "[prep] preload %s -> %u\n", rel[i], rc);
      }
      pl_close(f);
      if (mem) w2c_WebClient_free(g_inst, mem);
      if (gp) w2c_WebClient_free(g_inst, gp);
    }
  }

  static const char* json =
      "{\"WebLogin\":\"2\",\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}";
  static const char* flags_json =
      "{\"applicationSettings\":{\"BaseUrl\":\"https://www.roblox.com/\","
      "\"FIntWasmMemoryPages\":\"32768\"},"
      "\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}";
  add_file("/appData/LocalStorage/appStorage.json", json);
  add_file("/appData/LocalStorage/sitetest1.robloxlabs.com_appStorage.json", json);
  add_file("/appData/LocalStorage/sitetest2.robloxlabs.com_appStorage.json", json);
  add_file("/appData/LocalStorage/sitetest3.robloxlabs.com_appStorage.json", json);

  if (!getenv("WC_NOBIND")) {
    wc_embind_invoke("set_pretend_platform", 1, "desktop");
    wc_embind_invoke("set_roblox_base_url", 1, "https://www.roblox.com/");
    wc_embind_invoke("set_channel", 1, "");
    wc_embind_invoke("set_preloaded_flags", 1, flags_json);
    wc_embind_invoke("set_account_locales", 2, "en-us", "en-us");
    wc_embind_invoke("set_place_id", 1, (long long)g_wc_place);
    wc_embind_invoke("set_browser_tracker_id", 1, "");
  }
}
