/* VFS bootstrap ported from v2-wasmtime/src/main.rs::prepare and
 * v3-wasm2c/impl/prepare.c.  JSON is parsed with yyjson (no hand-rolled
 * parser), file IO uses the portable stdio helpers in fs.h, and everything is
 * handed to the engine through the generated exports in WC.h.
 *
 * The metadata file is the official `data/meta.json` asset list:
 *   [{"path":"/ExtraContent/...","size":1234}, ...]
 * A small embedded manifest is used when the file is absent so the engine's
 * VFS still contains the directories it probes during boot.
 */
#include "vfs.h"
#include "guest.h"
#include "fs.h"

#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(WEBLOX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#define WEBLOX_ASSET_BASE                                                     \
  "https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/"

#define WEBLOX_EMBEDDED_META                                                  \
  "[{\"path\":\"/content/.keep\",\"size\":1},"                               \
  "{\"path\":\"/ExtraContent/.keep\",\"size\":1},"                           \
  "{\"path\":\"/models/InExperience/InExperience.rbxm\",\"size\":1},"         \
  "{\"path\":\"/models/InExperience/InExperience_checksum\",\"size\":1}]"

#define WEBLOX_APP_STORAGE                                                    \
  "{\"WebLogin\":\"2\",\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}"

#define WEBLOX_MAX_ASSET (256u << 20)

static const char* meta_path(void) {
  const char* e = getenv("WC_META");
  return (e && *e) ? e : "data/meta.json";
}

static const char* content_root(void) {
  const char* e = getenv("WC_CONTENT");
  return (e && *e) ? e : "data/content";
}

static u32 guest_bytes(w2c_WebClient* inst, const void* src, size_t len) {
  if (len > UINT32_MAX - 1) return 0;
  u32 p = w2c_WebClient_malloc(inst, (u32)len + 1);
  if (!p) return 0;
  unsigned char* dst = guest_ptr(p, (u32)len + 1);
  if (!dst) {
    w2c_WebClient_free(inst, p);
    return 0;
  }
  memcpy(dst, src, len);
  dst[len] = 0;
  return p;
}

static int register_asset(w2c_WebClient* inst, const char* path, u64 size) {
  if (!path || !*path) return 0;
  u32 p = guest_bytes(inst, path, strlen(path));
  if (!p) {
    fprintf(stderr, "[vfs] cannot allocate path %s\n", path);
    return 0;
  }
  u32 rc = w2c_WebClient_rbx_fs_register_asset(inst, p, (u32)size);
  w2c_WebClient_free(inst, p);
  fprintf(stderr, "[vfs] register %s size=%llu -> %u\n", path,
          (unsigned long long)size, rc);
  return rc == 0;
}

/* Read and parse an asset list.  Both the official top-level array and an
 * object with an "assets"/"files" array are accepted. */
static int parse_manifest(w2c_WebClient* inst, const char* json, size_t len,
                          int* count) {
  yyjson_doc* doc = yyjson_read(json, len, 0);
  if (!doc) {
    fprintf(stderr, "[vfs] yyjson could not parse asset metadata\n");
    return 0;
  }
  yyjson_val* root = yyjson_doc_get_root(doc);
  yyjson_val* array = NULL;
  if (yyjson_is_arr(root)) {
    array = root;
  } else if (yyjson_is_obj(root)) {
    array = yyjson_obj_get(root, "assets");
    if (!array) array = yyjson_obj_get(root, "files");
  }
  if (!yyjson_is_arr(array)) {
    fprintf(stderr, "[vfs] metadata has no asset array\n");
    yyjson_doc_free(doc);
    return 0;
  }

  yyjson_arr_iter iter;
  yyjson_val* entry;
  yyjson_arr_iter_init(array, &iter);
  while ((entry = yyjson_arr_iter_next(&iter))) {
    if (!yyjson_is_obj(entry)) continue;
    const char* path = yyjson_get_str(yyjson_obj_get(entry, "path"));
    if (!path || !*path) continue;
    u64 size = yyjson_get_uint(yyjson_obj_get(entry, "size"));
    if (register_asset(inst, path, size)) ++*count;
  }
  yyjson_doc_free(doc);
  return 1;
}

static int register_metadata(w2c_WebClient* inst) {
  const char* no_reg = getenv("WC_NOREG");
  int count = 0;
  if (!no_reg || !*no_reg) {
    enum { CAP = 32 * 1024 * 1024 };
    char* buf = (char*)malloc(CAP);
    if (buf) {
      const char* path = meta_path();
      long n = weblox_fs_read_file(path, buf, CAP - 1);
      if (n > 0) {
        buf[n] = 0;
        parse_manifest(inst, buf, (size_t)n, &count);
      } else {
        fprintf(stderr, "[vfs] no asset metadata at %s; using manifest\n", path);
      }
      free(buf);
    }
  }
  if (!count) {
    parse_manifest(inst, WEBLOX_EMBEDDED_META, strlen(WEBLOX_EMBEDDED_META),
                   &count);
  }
  fprintf(stderr, "[vfs] registered %d asset paths\n", count);
  return count > 0;
}

/* ---------------- InExperience patch model ------------------------------- */

static int host_file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static long host_file_size(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long n = ftell(f);
  fclose(f);
  return n;
}

#if defined(WEBLOX_HAVE_SDL3)
static int ends_with(const char* path, const char* tail) {
  size_t n = strlen(path), m = strlen(tail);
  return n >= m && !strcmp(path + n - m, tail);
}


typedef struct {
  const char* tail;
  char found[1024];
  int depth;
} find_ctx;

static SDL_EnumerationResult find_entry(void* userdata, const char* dirname,
                                        const char* fname) {
  find_ctx* ctx = (find_ctx*)userdata;
  char path[1024];
  int n = snprintf(path, sizeof path, "%s%s", dirname, fname);
  if (n <= 0 || (size_t)n >= sizeof path) return SDL_ENUM_CONTINUE;

  SDL_PathInfo info;
  if (ends_with(path, ctx->tail) && SDL_GetPathInfo(path, &info) &&
      info.type == SDL_PATHTYPE_FILE) {
    snprintf(ctx->found, sizeof ctx->found, "%s", path);
    return SDL_ENUM_SUCCESS;
  }
  if (ctx->depth < 12 && SDL_GetPathInfo(path, &info) &&
      info.type == SDL_PATHTYPE_DIRECTORY) {
    find_ctx nested = *ctx;
    ++nested.depth;
    if (SDL_EnumerateDirectory(path, find_entry, &nested) && *nested.found) {
      snprintf(ctx->found, sizeof ctx->found, "%s", nested.found);
      return SDL_ENUM_SUCCESS;
    }
  }
  return SDL_ENUM_CONTINUE;
}
#endif

/* The mirror keeps the engine's own directory layout, so look for the file by
 * its tail instead of assuming exactly where it sits. */
static int find_content_file(const char* base, const char* rel, char* out,
                             size_t cap) {
  const char* tail = rel;
  while (*tail == '/' || *tail == '\\') ++tail;

  char candidate[1024];
  int n = snprintf(candidate, sizeof candidate, "%s/%s", base, tail);
  if (n > 0 && (size_t)n < sizeof candidate && host_file_exists(candidate)) {
    snprintf(out, cap, "%s", candidate);
    return 1;
  }
  if (weblox_fs_path("content", tail, candidate, sizeof candidate) &&
      host_file_exists(candidate)) {
    snprintf(out, cap, "%s", candidate);
    return 1;
  }
#if defined(WEBLOX_HAVE_SDL3)
  find_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.tail = tail;
  ctx.depth = 0;
  SDL_EnumerateDirectory(base, find_entry, &ctx);
  if (*ctx.found) {
    snprintf(out, cap, "%s", ctx.found);
    return 1;
  }
#endif
  return 0;
}

static int preload_asset(w2c_WebClient* inst, const char* rel) {
  char disk[1024];
  if (!find_content_file(content_root(), rel, disk, sizeof disk)) {
    fprintf(stderr, "[vfs] %s not found under %s\n", rel, content_root());
    return 0;
  }
  long size = host_file_size(disk);
  if (size <= 0 || (unsigned long)size > WEBLOX_MAX_ASSET) return 0;

  void* body = malloc((size_t)size);
  if (!body) return 0;
  long got = weblox_fs_read_file(disk, body, (size_t)size);
  if (got != size) {
    free(body);
    return 0;
  }

  u32 path = guest_bytes(inst, rel, strlen(rel));
  u32 data = w2c_WebClient_malloc(inst, (u32)size);
  unsigned char* dst = guest_ptr(data, (size_t)size);
  int ok = 0;
  if (path && data && dst) {
    memcpy(dst, body, (size_t)size);
    w2c_WebClient_rbx_fs_register_asset(inst, path, (u32)size);
    u32 rc = w2c_WebClient_rbx_fs_preload_asset(inst, path, data, (u32)size);
    fprintf(stderr, "[vfs] preload %s (%ld bytes) -> %u\n", rel, size, rc);
    ok = rc == 0;
  } else {
    fprintf(stderr, "[vfs] cannot stage %s in guest memory\n", rel);
  }
  if (data) w2c_WebClient_free(inst, data);
  if (path) w2c_WebClient_free(inst, path);
  free(body);
  return ok;
}

static void preload_models(w2c_WebClient* inst) {
  static const char* const rels[] = {
      "/models/InExperience/InExperience.rbxm",
      "/models/InExperience/InExperience_checksum"};
  for (size_t i = 0; i < sizeof rels / sizeof rels[0]; ++i)
    (void)preload_asset(inst, rels[i]);
}

static int add_file(w2c_WebClient* inst, const char* path, const char* data) {
  u32 p = guest_bytes(inst, path, strlen(path));
  u32 d = guest_bytes(inst, data, strlen(data));
  int ok = 0;
  if (p && d) {
    u32 rc = w2c_WebClient_rbx_fs_add_file(inst, p, d, (u32)strlen(data));
    fprintf(stderr, "[vfs] add_file %s -> %u\n", path, rc);
    ok = rc == 0;
  }
  if (d) w2c_WebClient_free(inst, d);
  if (p) w2c_WebClient_free(inst, p);
  return ok;
}

static int add_app_storage(w2c_WebClient* inst) {
  static const char* const paths[] = {
      "/appData/LocalStorage/appStorage.json",
      "/appData/LocalStorage/sitetest1.robloxlabs.com_appStorage.json",
      "/appData/LocalStorage/sitetest2.robloxlabs.com_appStorage.json",
      "/appData/LocalStorage/sitetest3.robloxlabs.com_appStorage.json"};
  int ok = 1;
  for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i)
    if (!add_file(inst, paths[i], WEBLOX_APP_STORAGE)) ok = 0;
  return ok;
}

int weblox_vfs_set_base_url(w2c_WebClient* inst, const char* url) {
  if (!inst || !url || !*url) return -1;
  u32 p = guest_bytes(inst, url, strlen(url));
  if (!p) return -1;
  w2c_WebClient_rbx_fs_set_base_url(inst, p);
  w2c_WebClient_free(inst, p);
  fprintf(stderr, "[vfs] base url %s\n", url);
  return 0;
}

int weblox_vfs_bootstrap(w2c_WebClient* inst) {
  if (!inst) {
    fprintf(stderr, "[vfs] no instance\n");
    return -1;
  }
  const char* base = getenv("WC_ASSET_BASE");
  int failures = 0;
  failures += weblox_vfs_set_base_url(inst, (base && *base) ? base
                                                          : WEBLOX_ASSET_BASE) != 0;
  if (!register_metadata(inst)) ++failures;
  preload_models(inst);  // optional patch files are fetched on demand if absent
  if (!add_app_storage(inst)) ++failures;
  fprintf(stderr, "[vfs] bootstrap %s\n", failures ? "with errors" : "ok");
  return failures ? -1 : 0;
}
