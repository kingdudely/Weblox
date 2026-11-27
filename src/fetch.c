// Port of v3-wasm2c/fetch.c: js_fetch_file serves rbxasset content from a
// local tree and mirrors CDN misses into it.  libcurl does the download via
// weblox_http_get(); stdio does the local reads/writes.
#include "http.h"
#include "guest.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FETCH (256u << 20)

static const char* content_root(void) {
  const char* e = getenv("WC_CONTENT");
  return (e && *e) ? e : "data/content";
}

static int resolve(const char* url, char* out, size_t cap) {
  if (!strncmp(url, "rbxasset://", 11)) url += 11;
  while (*url == '/') ++url;
  if (!*url || strstr(url, "..")) return 0;
  int n = snprintf(out, cap, "%s/%s", content_root(), url);
  return n > 0 && (size_t)n < cap;
}

static int read_local(const char* path, u32 dst, u32 want) {
  FILE* f = fopen(path, "rb");
  if (!f) return -2;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
  long size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -2; }
  if (want && (u32)size != want) { fclose(f); return -1; }
  if ((uint64_t)size > MAX_FETCH) { fclose(f); return -1; }
  unsigned char* out = guest_ptr(dst, (size_t)size);
  if (!out) { fclose(f); return -1; }
  size_t got = fread(out, 1, (size_t)size, f);
  fclose(f);
  return got == (size_t)size ? 0 : -1;
}

static int fetch_remote(const char* url, const char* cache, u32 dst, u32 want) {
  char* bytes = NULL;
  size_t len = 0;
  if (weblox_http_get(url, MAX_FETCH, &bytes, &len) != 0) return -1;
  if (want && len != want) { free(bytes); return -1; }
  unsigned char* out = guest_ptr(dst, len);
  if (!out) { free(bytes); return -1; }
  memcpy(out, bytes, len);
  if (cache && *cache && weblox_fs_write_file(cache, bytes, len))
    fprintf(stderr, "[fetch] cached %zu bytes -> %s\n", len, cache);
  free(bytes);
  return 0;
}

u32 w2c_env_js_fetch_file(struct w2c_env* env, u32 urlp, u32 dst, u32 want) {
  (void)env;
  const char* url = guest_str(urlp);
  fprintf(stderr, "[fetch] js_fetch_file %s want=%u\n",
          (url && *url) ? url : "(null)", want);
  if (!url || !*url) return (u32)-1;
  if (!strncmp(url, "data:", 5) || !strncmp(url, "rbxthumb://", 11))
    return (u32)-1;

  char path[1024];
  int is_http = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
  if (!is_http) {
    if (!resolve(url, path, sizeof path)) {
      fprintf(stderr, "[fetch] %s -> no local resolve\n", url);
      return (u32)-2;
    }
    int result = read_local(path, dst, want);
    if (result != -2) return (u32)result;
    // Local copy missing: try the optional mirrored CDN base.
    const char* base = getenv("WC_BASE_URL");
    if (!base || !*base) return (u32)result;
    const char* rel = url;
    while (*rel == '/') ++rel;
    if (!strncmp(rel, "rbxasset://", 11)) rel += 11;
    while (*rel == '/') ++rel;
    char remote[2048];
    int n = snprintf(remote, sizeof remote, "%s%s", base, rel);
    if (n < 0 || (size_t)n >= sizeof remote) return (u32)-1;
    return (u32)fetch_remote(remote, path, dst, want);
  }

  // For absolute URLs use the path portion as the cache key.
  path[0] = 0;
  const char* after_scheme = strstr(url, "://");
  const char* rel = after_scheme ? after_scheme + 3 : url;
  rel = strchr(rel, '/');
  if (rel && *++rel) (void)resolve(rel, path, sizeof path);
  return (u32)fetch_remote(url, path[0] ? path : NULL, dst, want);
}
