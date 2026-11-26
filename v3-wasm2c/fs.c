/* impl/fs.c - sandboxed filesystem used by opfs and fetch.
 *
 * Guest paths are resolved under <WC_ROOT>/<bucket>/; ".." is rejected.  The
 * opfs bucket is "rbx-storage", the local asset bucket is "content".
 */
#include "../host/host.h"
#include "../host/plat.h"
#include "fs.h"

#define MAX_READ (256u << 20)

int wc_fs_mkparents(const char* bucket, const char* guest) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  char* slash = strrchr(path, '/');
  if (!slash) return 1;
  *slash = 0;
  return pl_mkdirs(path);
}

int wc_fs_read(const char* bucket, const char* guest, void** out, size_t* len) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  int64_t sz = pl_size_path(path);
  if (sz <= 0 || (uint64_t)sz > MAX_READ) return 0;
  pl_file* f = pl_open_read(path);
  if (!f) return 0;
  void* buf = malloc((size_t)sz);
  if (!buf) { pl_close(f); return 0; }
  size_t got = pl_read(f, buf, (size_t)sz);
  pl_close(f);
  if (got != (size_t)sz) { free(buf); return 0; }
  *out = buf;
  *len = got;
  return 1;
}

int wc_fs_read_at(const char* bucket, const char* guest, void* dst, int64_t off, size_t len) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  pl_file* f = pl_open_read(path);
  if (!f) return 0;
  pl_seek(f, off, SEEK_SET);
  size_t got = pl_read(f, dst, len);
  pl_close(f);
  return (int)got;
}

int wc_fs_write_all(const char* bucket, const char* guest, const void* src, size_t len) {
  wc_fs_mkparents(bucket, guest);
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  pl_file* f = pl_open_write(path);
  if (!f) return 0;
  size_t put = pl_write(f, src, len);
  pl_close(f);
  return put == len;
}

int64_t wc_fs_size(const char* bucket, const char* guest) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return -1;
  return pl_size_path(path);
}

int wc_fs_exists(const char* bucket, const char* guest) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  return pl_exists(path);
}

int wc_fs_remove(const char* bucket, const char* guest) {
  char path[1024];
  if (!pl_path(bucket, guest, path, sizeof path)) return 0;
  return pl_remove_tree(path);
}

int wc_fs_clear(const char* bucket) {
  char path[1024];
  if (!pl_path(bucket, "", path, sizeof path)) return 0;
  pl_remove_tree(path);
  return pl_mkdirs(path);
}

struct ls_ctx {
  char* buf;
  size_t len, cap;
  int64_t total;
  int count;
  int sized;
};

static int ls_cb(const char* name, int64_t size, void* ud) {
  struct ls_ctx* c = ud;
  if (size <= 0) return 0;
  char tmp[1200];
  int n = c->sized ? snprintf(tmp, sizeof tmp, "%lld\t%s", (long long)size, name)
                   : snprintf(tmp, sizeof tmp, "%s", name);
  if (n <= 0) return 0;
  size_t need = c->len + (size_t)n + 2;
  if (need > c->cap) {
    c->cap = need * 2;
    char* nb = realloc(c->buf, c->cap);
    if (!nb) return 1;
    c->buf = nb;
  }
  if (c->len) c->buf[c->len++] = ' ';
  memcpy(c->buf + c->len, tmp, (size_t)n);
  c->len += (size_t)n;
  c->buf[c->len] = 0;
  c->total += size;
  c->count++;
  return 0;
}

int wc_fs_list(const char* bucket, char** joined, int64_t* total, int* count, int sized) {
  char path[1024];
  if (!pl_path(bucket, "", path, sizeof path)) return 0;
  if (!pl_exists(path)) pl_mkdirs(path);
  struct ls_ctx c = {0};
  c.sized = sized;
  c.cap = 256;
  c.buf = malloc(c.cap);
  if (!c.buf) return 0;
  c.buf[0] = 0;
  pl_list(path, ls_cb, &c);
  *joined = c.buf;
  *total = c.total;
  *count = c.count;
  return c.count > 0;
}
