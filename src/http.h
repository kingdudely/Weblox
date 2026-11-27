// HTTP/fetch support for the clean C11 host.  Import implementations live in
// http.c and fetch.c; the pump is called by the host event loop so completions
// are delivered asynchronously, like browser XHR callbacks.
#ifndef WEBLOX_HTTP_H
#define WEBLOX_HTTP_H

#include "host.h"

#include <stddef.h>

// Run at most one queued emscripten_fetch completion callback.
// Returns 1 when a callback was invoked, 0 when the queue was empty.
int weblox_http_pump(w2c_WebClient* inst);

// Drop queued completions for a fetch object that is being freed.
void weblox_http_drop(u32 fetch);

// Blocking GET used by the asset fetcher.  On success returns 0 and transfers
// a NUL-terminated malloc'd buffer to *out (length in *out_len). Returns -1 on
// transport, HTTP >= 400, or size-limit failure.
int weblox_http_get(const char* url, size_t max_bytes, char** out, size_t* out_len);

#endif
