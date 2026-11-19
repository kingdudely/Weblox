/* impl/fetch.c - js_fetch_file and the emscripten_fetch shims.
 *
 * Everything the engine considers rbxasset content is served out of the local
 * asset tree (WC_CONTENT, default data/content).  Misses are pulled from the
 * CDN and written back into that tree, so the second run is fully local.
 */
#include "../host/host.h"
#include "../host/plat.h"
#include "fs.h"

#define MAX_FETCH (256u << 20)
#define BUCKET "content"

static const char* content_root(void) {
  const char* e = pl_getenv("WC_CONTENT");
  return (e && *e) ? e : "data/content";
}

/* map an engine URL to a path under the asset root */
static int resolve(const char* url, char* out, size_t cap) {
  if (!strncmp(url, "rbxasset://", 11)) url += 11;
  while (*url == '/') url++;
  if (!*url || strstr(url, "..")) return 0;
  int n = snprintf(out, cap, "%s/%s", content_root(), url);
  return n > 0 && (size_t)n < cap;
}

static int read_local(const char* path, u32 dst, u32 want) {
  pl_file* f = pl_open_read(path);
  if (!f) return -2;
  int64_t sz = pl_size(f);
  if (sz < 0) { pl_close(f); return -2; }
  if (want && (u32)sz != want) { pl_close(f); return -1; }
  size_t n = (size_t)sz;
  if (n > MAX_FETCH) { pl_close(f); return -1; }
  size_t got = pl_read(f, wc_ptr(dst), n);
  pl_close(f);
  return got == n ? 0 : -1;
}

static int fetch_remote(const char* url, const char* cache, u32 dst, u32 want) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s/fetch-XXXXXX", pl_getenv("TMPDIR") ? pl_getenv("TMPDIR") : "/tmp");
  char* slash = strrchr(cache, '/');
  if (slash) { char d[512]; snprintf(d, sizeof d, "%.*s", (int)(slash - cache), cache); pl_mkdirs(d); }
  int fd = mkstemp(tmp);
  if (fd < 0) return -1;
  close(fd);
  const char* argv[] = { "curl", "-fsSL", "--max-time", "120", "-o", tmp, url, NULL };
  int rc = pl_spawn_capture(argv, "/dev/null");
  pl_file* f = rc ? NULL : pl_open_read(tmp);
  int r = -1;
  if (f) {
    int64_t sz = pl_size(f);
    if (sz > 0 && (uint64_t)sz <= MAX_FETCH && (!want || (u32)sz == want)) {
      size_t n = (size_t)sz;
      if (pl_read(f, wc_ptr(dst), n) == n) {
        r = 0;
        pl_file* out = pl_open_write(cache);
        if (out) {
          pl_write(out, wc_ptr(dst), n);
          pl_close(out);
        }
      }
    }
    pl_close(f);
  }
  pl_remove(tmp);
  return r;
}

u32 wc_impl_js_fetch_file(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  const char* url = wc_str(a1);
  fprintf(stderr, "[jff0] %s want=%u\n", url ? url : "(null)", a3);
  if (!url || !*url) return (u32)-1;
  if (!strncmp(url, "data:", 5) || !strncmp(url, "rbxthumb://", 11)) return (u32)-1;

  int is_http = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
  int dbg = strstr(url, "InExperience") != NULL;
  int pr = 1;
  if (g_wc_verbose) fprintf(stderr, "[fetch] %s want=%u\n", url, a3);
  char path[1024];
  if (!is_http) {
    if (!resolve(url, path, sizeof path)) { if (pr) fprintf(stderr, "[jff] %s -> noresolve\n", url); return (u32)-2; }
    int r = read_local(path, a2, a3);
    if (pr) fprintf(stderr, "[jff] %s want=%u -> %d\n", url, a3, r);
    if (dbg) fprintf(stderr, "[inexp] %s want=%u -> %d (%s)\n", url, a3, r, path);
    if (r != -2 || !(g_wc_base && *g_wc_base)) return (u32)r;
    /* not mirrored yet: fall back to the CDN copy */
    while (*url == '/') url++;
    if (!strncmp(url, "rbxasset://", 11)) url += 11;
    while (*url == '/') url++;
    char remote[1024];
    snprintf(remote, sizeof remote, "%s%s", g_wc_base, url);
    return (u32)fetch_remote(remote, path, a2, a3);
  }
  if (!resolve(url, path, sizeof path)) return (u32)-1;
  return (u32)fetch_remote(url, path, a2, a3);
}


u32 wc_impl_0x5F_syscall_getegid32(struct w2c_env* a0) { (void)a0; return 0; }
u32 wc_impl_0x5F_syscall_geteuid32(struct w2c_env* a0) { (void)a0; return 0; }
u32 wc_impl_0x5F_syscall_getgid32(struct w2c_env* a0) { (void)a0; return 0; }
u32 wc_impl_0x5F_syscall_getuid32(struct w2c_env* a0) { (void)a0; return 0; }
