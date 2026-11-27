// Portable stdio file helpers.  The reference implementation used pl_file and
// POSIX mkdir/mkstemp; this host deliberately uses nothing outside C11 stdio.
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* fs_root(void) {
  const char* e = getenv("WC_ROOT");
  return (e && *e) ? e : ".";
}

long weblox_fs_read_file(const char* path, void* dst, size_t cap) {
  if (!path || !*path || !dst) return -1;
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long size = ftell(f);
  if (size < 0 || (unsigned long)size > cap || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  size_t got = fread(dst, 1, (size_t)size, f);
  fclose(f);
  return got == (size_t)size ? (long)got : -1;
}

int weblox_fs_write_file(const char* path, const void* src, size_t len) {
  if (!path || !*path) return 0;
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  size_t put = fwrite(src, 1, len, f);
  int ok = put == len && fclose(f) == 0;
  return ok ? 1 : 0;
}

void weblox_fs_remove_file(const char* path) {
  if (path && *path) (void)remove(path);
}

int weblox_fs_path(const char* bucket, const char* guest, char* out, size_t cap) {
  if (!bucket || !guest || !out || cap == 0) return 0;
  if (!*guest || strstr(guest, "..")) return 0;
  const char* root = fs_root();
  int n = snprintf(out, cap, "%s/%s/%s", root, bucket, guest);
  return n > 0 && (size_t)n < cap;
}

int weblox_fs_read(const char* bucket, const char* guest, void** out, size_t* len) {
  char path[1024];
  if (!weblox_fs_path(bucket, guest, path, sizeof path)) return 0;
  int64_t size = weblox_fs_size(bucket, guest);
  if (size <= 0 || (uint64_t)size > (256u << 20)) return 0;
  void* buf = malloc((size_t)size);
  if (!buf) return 0;
  long got = weblox_fs_read_file(path, buf, (size_t)size);
  if (got != (long)size) { free(buf); return 0; }
  *out = buf;
  *len = (size_t)got;
  return 1;
}

int weblox_fs_write_all(const char* bucket, const char* guest, const void* src,
                        size_t len) {
  char path[1024];
  if (!weblox_fs_path(bucket, guest, path, sizeof path)) return 0;
  return weblox_fs_write_file(path, src, len);
}

int64_t weblox_fs_size(const char* bucket, const char* guest) {
  char path[1024];
  if (!weblox_fs_path(bucket, guest, path, sizeof path)) return -1;
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long size = ftell(f);
  fclose(f);
  return size < 0 ? -1 : (int64_t)size;
}

int weblox_fs_exists(const char* bucket, const char* guest) {
  return weblox_fs_size(bucket, guest) >= 0;
}
