/* fs.h - sandboxed file helpers shared by opfs and fetch. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* all paths are guest paths, resolved under <root>/<bucket> */
int wc_fs_read(const char* bucket, const char* guest, void** out, size_t* len);
int wc_fs_read_at(const char* bucket, const char* guest, void* dst, int64_t off, size_t len);
int wc_fs_write_all(const char* bucket, const char* guest, const void* src, size_t len);
int64_t wc_fs_size(const char* bucket, const char* guest);
int wc_fs_exists(const char* bucket, const char* guest);
int wc_fs_remove(const char* bucket, const char* guest);
int wc_fs_clear(const char* bucket);
int wc_fs_list(const char* bucket, char** joined, int64_t* total, int* count, int sized);
int wc_fs_mkparents(const char* bucket, const char* guest);
