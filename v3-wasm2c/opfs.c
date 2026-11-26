/* impl/opfs.c - the rbx_opfs_* surface over a sandboxed directory.
 *
 * The browser build is async (JSPI); natively every call completes inline.
 */
#include "../host/host.h"
#include "../host/plat.h"
#include "fs.h"

#define BUCKET "rbx-storage"

static void ensure_root(void) {
  char p[1024];
  if (pl_path(BUCKET, "", p, sizeof p)) pl_mkdirs(p);
}

/* open handles are tracked only for reference counting parity */
struct handle { char path[512]; int ref; };
static struct handle g_handles[128];
static int g_nhandles;

static int touch(const char* p) {
  for (int i = 0; i < g_nhandles; i++)
    if (!strcmp(g_handles[i].path, p)) return i;
  if (g_nhandles == 128) return -1;
  snprintf(g_handles[g_nhandles].path, sizeof g_handles[0].path, "%s", p);
  g_handles[g_nhandles].ref = 1;
  return g_nhandles++;
}

void wc_impl_0x5F_asyncjs_0x5Frbx_opfs_open(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  const char* p = wc_str(a1);
  int h = touch(p);
  if (h >= 0) g_handles[h].ref++;
  if (a2) {
    wc_fs_mkparents(BUCKET, p);
    if (!wc_fs_exists(BUCKET, p)) wc_fs_write_all(BUCKET, p, "", 0);
  }
}

void wc_impl_rbx_opfs_close(struct w2c_env* a0, u32 a1) {
  (void)a0;
  const char* p = wc_str(a1);
  for (int i = 0; i < g_nhandles; i++)
    if (!strcmp(g_handles[i].path, p) && --g_handles[i].ref <= 0) {
      g_handles[i] = g_handles[--g_nhandles];
      return;
    }
}

void wc_impl_rbx_opfs_free(struct w2c_env* a0, u32 a1) {
  (void)a0;
  w2c_WebClient_free(g_inst, a1);
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_contains(struct w2c_env* a0, u32 a1) {
  (void)a0;
  ensure_root();
  const char* g = wc_str(a1);
  char path[1024];
  if (!pl_path(BUCKET, g, path, sizeof path)) return 0;
  if (pl_isdir(path)) return 1;
  return pl_exists(path) != 0;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_file_size(struct w2c_env* a0, u32 a1) {
  (void)a0;
  int64_t n = wc_fs_size(BUCKET, wc_str(a1));
  return (u32)(n > 0 ? n : 0);
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_get(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  if (!a2) return 0;
  *(u32*)wc_ptr(a2) = 0;
  void* buf = NULL; size_t len = 0;
  if (!wc_fs_read(BUCKET, wc_str(a1), &buf, &len)) return 0;
  u32 dst = w2c_WebClient_malloc(g_inst, (u32)len);
  if (!dst) { free(buf); return 0; }
  memcpy(wc_ptr(dst), buf, len);
  free(buf);
  *(u32*)wc_ptr(a2) = (u32)len;
  return dst;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_get_buffer(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  if (!a3) return 0;
  int64_t sz = wc_fs_size(BUCKET, wc_str(a1));
  if (sz <= 0) return 0;
  size_t n = (size_t)((int64_t)a3 < sz ? a3 : sz);
  return (u32)wc_fs_read_at(BUCKET, wc_str(a1), wc_ptr(a2), 0, n);
}

/* (path, dst, n, srcOffset, max) */
u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_get_buffer_offset(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)a0;
  int64_t sz = wc_fs_size(BUCKET, wc_str(a1));
  if (sz <= 0 || (int64_t)a4 >= sz || !a5) return 0;
  size_t n = (size_t)((int64_t)a3 < a5 ? a3 : a5);
  if ((int64_t)(a4 + n) > sz) n = (size_t)(sz - a4);
  return (u32)wc_fs_read_at(BUCKET, wc_str(a1), wc_ptr(a2), a4, n);
}

/* (path, outSize, srcOffset, len) */
u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_get_offset(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  if (a2) *(u32*)wc_ptr(a2) = 0;
  int64_t sz = wc_fs_size(BUCKET, wc_str(a1));
  if (sz <= 0 || (int64_t)a3 >= sz || !a4) return 0;
  size_t n = (size_t)a4;
  if ((int64_t)(a3 + n) > sz) n = (size_t)(sz - a3);
  u32 dst = w2c_WebClient_malloc(g_inst, (u32)n);
  if (!dst) return 0;
  size_t got = (size_t)wc_fs_read_at(BUCKET, wc_str(a1), wc_ptr(dst), a3, n);
  if (a2) *(u32*)wc_ptr(a2) = (u32)got;
  return dst;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_set(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  if (!a3) return 0;
  return wc_fs_write_all(BUCKET, wc_str(a1), wc_ptr(a2), a3) ? 1 : 0;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_remove(struct w2c_env* a0, u32 a1) {
  (void)a0;
  return wc_fs_remove(BUCKET, wc_str(a1)) ? 1 : 0;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_remove_batch(struct w2c_env* a0, u32 a1) {
  (void)a0;
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", wc_str(a1));
  int n = 0;
  for (char* s = tmp; s && *s;) {
    char* sp = strchr(s, ' ');
    if (sp) *sp = 0;
    if (*s && wc_fs_remove(BUCKET, s)) n++;
    s = sp ? sp + 1 : NULL;
  }
  return (u32)n;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_clear(struct w2c_env* a0) {
  (void)a0;
  g_nhandles = 0;
  return wc_fs_clear(BUCKET) ? 1 : 0;
}

static u32 do_list(u32 a1, u32 a2, int sized) {
  char* joined = NULL;
  int64_t total = 0;
  int count = 0;
  if (!wc_fs_list(BUCKET, &joined, &total, &count, sized)) { free(joined); return 0; }
  size_t n = strlen(joined) + 1;
  u32 dst = w2c_WebClient_malloc(g_inst, (u32)n);
  if (!dst) { free(joined); return 0; }
  memcpy(wc_ptr(dst), joined, n);
  free(joined);
  if (a1) *(u64*)wc_ptr(a1) = (u64)total;
  if (a2) *(u32*)wc_ptr(a2) = (u32)count;
  return dst;
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_list(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  return do_list(a1, a2, 0);
}

u32 wc_impl_0x5F_asyncjs_0x5Frbx_opfs_list_sized(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  return do_list(a1, a2, 1);
}

u32 wc_impl_rbx_opfs_read_sync_if_cached(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  int64_t sz = wc_fs_size(BUCKET, wc_str(a1));
  if (sz < 0) return (u32)-1;
  if ((int64_t)a3 >= sz) return 0;
  size_t n = a4;
  if ((int64_t)(a3 + n) > sz) n = (size_t)(sz - a3);
  return (u32)wc_fs_read_at(BUCKET, wc_str(a1), wc_ptr(a2), a3, n);
}

void wc_impl_rbx_opfs_install_helpers(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0; (void)a1; (void)a2;
}

u32 wc_impl_0x5Fmsync_js(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u64 a6) {
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  return 0;
}
