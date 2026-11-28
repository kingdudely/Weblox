// Implementation of the optional import tracer.  Included once, from
// imports.c, when WEBLOX_IMPORT_TRACE is defined.
#ifdef WEBLOX_IMPORT_TRACE

#include "abort_debug.h"

#include <stdio.h>

static weblox_trace_entry g_trace[WEBLOX_TRACE_SLOTS];
static unsigned g_trace_head;

void weblox_import_trace(const char* name, const u64* args, int nargs) {
  weblox_trace_entry* e = &g_trace[g_trace_head++ % WEBLOX_TRACE_SLOTS];
  e->name = name;
  e->nargs = nargs > WEBLOX_TRACE_ARGS ? WEBLOX_TRACE_ARGS : nargs;
  for (int i = 0; i < e->nargs; ++i) e->args[i] = args[i];
}

void weblox_import_trace_dump(void) {
  fprintf(stderr, "[trace] last imports:");
  for (unsigned i = 0; i < WEBLOX_TRACE_SLOTS; ++i) {
    unsigned slot = (g_trace_head + i) % WEBLOX_TRACE_SLOTS;
    const weblox_trace_entry* e = &g_trace[slot];
    if (!e->name) continue;
    fprintf(stderr, "\n  %s(", e->name);
    for (int j = 0; j < e->nargs; ++j) {
      if (j) fprintf(stderr, ", ");
      fprintf(stderr, "0x%llx", (unsigned long long)e->args[j]);
    }
    fputs(")", stderr);
  }
  fputc('\n', stderr);
}

#endif  // WEBLOX_IMPORT_TRACE
