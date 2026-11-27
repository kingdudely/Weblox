// Portable stdio file helpers shared by the HTTP and asset-fetch imports.
// No POSIX or Win32 calls live here: only C11 <stdio.h> and string functions.
#ifndef WEBLOX_FS_H
#define WEBLOX_FS_H

#include <stddef.h>
#include <stdint.h>

// Read at most cap bytes from a host path. Returns the byte count, or -1 on
// failure (including a file larger than cap). NUL is not appended.
long weblox_fs_read_file(const char* path, void* dst, size_t cap);

// Write a buffer to a host path, replacing any existing file. Returns 1 on
// success, 0 on failure. Parent directories are not created.
int weblox_fs_write_file(const char* path, const void* src, size_t len);

// Delete a host path (files only, via stdio remove()).
void weblox_fs_remove_file(const char* path);

// Resolve a guest path under <root>/<bucket>/, rejecting ".." traversal.
// WC_ROOT defaults to ".". Returns 1 on success.
int weblox_fs_path(const char* bucket, const char* guest, char* out, size_t cap);

// Bucket-relative helpers used by the asset fetcher.
int weblox_fs_read(const char* bucket, const char* guest, void** out, size_t* len);
int weblox_fs_write_all(const char* bucket, const char* guest, const void* src,
                        size_t len);
int64_t weblox_fs_size(const char* bucket, const char* guest);
int weblox_fs_exists(const char* bucket, const char* guest);

#endif
