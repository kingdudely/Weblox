// Compact ring-buffer tracer for host imports, used to diagnose engine aborts.
//
// Disabled by default.  Define WEBLOX_IMPORT_TRACE when compiling the host to
// record the last 64 import calls; weblox_import_trace_dump() is printed from
// the _abort_js stub (or from a debugger) when the engine aborts.
#ifndef WEBLOX_ABORT_DEBUG_H
#define WEBLOX_ABORT_DEBUG_H

#include "host.h"

#ifdef WEBLOX_IMPORT_TRACE

#define WEBLOX_TRACE_SLOTS 64
#define WEBLOX_TRACE_ARGS 6

typedef struct {
  const char* name;
  int nargs;
  u64 args[WEBLOX_TRACE_ARGS];
} weblox_trace_entry;

void weblox_import_trace(const char* name, const u64* args, int nargs);
void weblox_import_trace_dump(void);

// Compound literal keeps every call site to one line; trailing zeros are not
// needed because nargs is computed from the literal's size.
#define WEBLOX_IMPORT_TRACE_CALL(name, ...)                                   \
  weblox_import_trace(name, (const u64[]){__VA_ARGS__},                       \
                      (int)(sizeof((const u64[]){__VA_ARGS__}) / sizeof(u64)))

#else

#define WEBLOX_IMPORT_TRACE_CALL(name, ...) ((void)0)

#endif

#endif  // WEBLOX_ABORT_DEBUG_H
