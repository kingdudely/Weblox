/* embind registry and invoker, ported from v3-wasm2c/impl/embind.c.
 *
 * The engine's embind registration imports describe a host-callable function
 * as (name, argument count, signature, invoker index, method id).  The
 * invoker is a wasm function in the instance's indirect function table whose
 * first parameter is the method id, followed by one wasm value per argument.
 * Signature byte k+2 gives the k-th argument's wasm type ('p'/'s' string
 * pointer, 'i'/'c'/'b'/'h'/'t' i32, 'j'/'l'/'x' i64, 'f' f32, 'd' f64), which
 * is what lets us pick the right C function pointer type instead of guessing.
 */
#include "embind.h"
#include "guest.h"
#include "abort_debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEBLOX_EMBIND_MAX 512
#define WEBLOX_EMBIND_NAME 96
#define WEBLOX_EMBIND_SIG 32
#define WEBLOX_EMBIND_ARGS 8

struct weblox_binding {
  char name[WEBLOX_EMBIND_NAME];
  unsigned char sig[WEBLOX_EMBIND_SIG];
  u32 sig_len;
  u32 argc;     // arguments after the receiver placeholder
  u32 invoker;  // index into the instance's indirect function table
  u32 id;       // first (hidden) argument of the invoker
};

static struct weblox_binding g_bindings[WEBLOX_EMBIND_MAX];
static int g_count;

struct weblox_type {
  u32 id;
  char kind[16];
};
static struct weblox_type g_types[256];
static int g_type_count;

static int verbose(void) {
  static int cached = -1;
  if (cached < 0) {
    const char* e = getenv("WEBLOX_EMBIND_VERBOSE");
    cached = e && *e && strcmp(e, "0") != 0;
  }
  return cached;
}

static void add_type(u32 id, const char* kind) {
  for (int i = 0; i < g_type_count; ++i)
    if (g_types[i].id == id) {
      snprintf(g_types[i].kind, sizeof g_types[i].kind, "%s", kind);
      return;
    }
  if (g_type_count == (int)(sizeof g_types / sizeof g_types[0])) return;
  g_types[g_type_count].id = id;
  snprintf(g_types[g_type_count].kind, sizeof g_types[g_type_count].kind,
           "%s", kind);
  ++g_type_count;
}

int weblox_embind_count(void) { return g_count; }

const char* weblox_embind_name(int index) {
  return (index >= 0 && index < g_count) ? g_bindings[index].name : NULL;
}

void weblox_embind_dump(void) {
  for (int i = 0; i < g_count; ++i) {
    const struct weblox_binding* b = &g_bindings[i];
    fprintf(stderr, "[embind] %s argc=%u inv=%u id=%u sig=%.*s\n", b->name,
            b->argc, b->invoker, b->id, (int)b->sig_len,
            (const char*)b->sig);
  }
}

void weblox_embind_reset(void) {
  g_count = 0;
  g_type_count = 0;
  memset(g_bindings, 0, sizeof g_bindings);
  memset(g_types, 0, sizeof g_types);
}

static void add_binding(const char* name, const char* sig, u32 argc,
                        u32 invoker, u32 id) {
  if (!name || !*name || !invoker || g_count == WEBLOX_EMBIND_MAX) return;
  struct weblox_binding* b = &g_bindings[g_count];
  snprintf(b->name, sizeof b->name, "%s", name);
  if (sig) {
    size_t n = strlen(sig);
    if (n > WEBLOX_EMBIND_SIG - 1) n = WEBLOX_EMBIND_SIG - 1;
    memcpy(b->sig, sig, n);
    b->sig[n] = 0;
    b->sig_len = (u32)n;
  }
  b->argc = argc ? argc - 1 : 0;  // registration counts the 'this' slot
  b->invoker = invoker;
  b->id = id;
  ++g_count;
  if (verbose())
    fprintf(stderr, "[embind] register %s argc=%u inv=%u id=%u sig=%s\n",
            b->name, b->argc, invoker, id, (const char*)b->sig);
}

/* ---------------- registration imports (all signatures from WC.h) -------- */

void w2c_env_0x5Fembind_register_function(struct w2c_env* env, u32 name_ptr,
                                          u32 argc, u32 types_ptr,
                                          u32 sig_ptr, u32 invoker, u32 id,
                                          u32 a, u32 b) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_function", (u64)(uintptr_t)env, (u64)name_ptr, (u64)argc, (u64)types_ptr, (u64)sig_ptr, (u64)invoker, (u64)id, (u64)a, (u64)b);
  (void)env;
  (void)types_ptr;
  (void)a;
  (void)b;
  add_binding(guest_str(name_ptr), guest_str(sig_ptr), argc, invoker, id);
}

void w2c_env_0x5Fembind_register_void(struct w2c_env* env, u32 id, u32 name) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_void", (u64)(uintptr_t)env, (u64)id, (u64)name);
  (void)env;
  add_type(id, "void");
  if (verbose()) fprintf(stderr, "[embind] type %u=void (%s)\n", id, guest_str(name));
}

void w2c_env_0x5Fembind_register_bool(struct w2c_env* env, u32 id, u32 name,
                                      u32 size, u32 true_value) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_bool", (u64)(uintptr_t)env, (u64)id, (u64)name, (u64)size, (u64)true_value);
  (void)env;
  (void)size;
  (void)true_value;
  add_type(id, "bool");
  if (verbose()) fprintf(stderr, "[embind] type %u=bool (%s)\n", id, guest_str(name));
}

void w2c_env_0x5Fembind_register_integer(struct w2c_env* env, u32 id, u32 name,
                                         u32 size, u32 low, u32 high) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_integer", (u64)(uintptr_t)env, (u64)id, (u64)name, (u64)size, (u64)low, (u64)high);
  (void)env;
  (void)low;
  (void)high;
  add_type(id, size == 8 ? "i64" : "i32");
  if (verbose())
    fprintf(stderr, "[embind] type %u=%s (%s)\n", id,
            size == 8 ? "i64" : "i32", guest_str(name));
}

void w2c_env_0x5Fembind_register_bigint(struct w2c_env* env, u32 id, u32 name,
                                        u32 size, u64 low, u64 high) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_bigint", (u64)(uintptr_t)env, (u64)id, (u64)name, (u64)size, (u64)low, (u64)high);
  (void)env;
  (void)low;
  (void)high;
  add_type(id, size == 8 ? "i64" : "i32");
  if (verbose()) fprintf(stderr, "[embind] type %u=bigint (%s)\n", id, guest_str(name));
}

void w2c_env_0x5Fembind_register_float(struct w2c_env* env, u32 id, u32 name,
                                       u32 size) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_float", (u64)(uintptr_t)env, (u64)id, (u64)name, (u64)size);
  (void)env;
  add_type(id, size == 8 ? "f64" : "f32");
  if (verbose())
    fprintf(stderr, "[embind] type %u=%s (%s)\n", id,
            size == 8 ? "f64" : "f32", guest_str(name));
}

void w2c_env_0x5Fembind_register_std_string(struct w2c_env* env, u32 id,
                                            u32 name) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_std_string", (u64)(uintptr_t)env, (u64)id, (u64)name);
  (void)env;
  add_type(id, "str");
  if (verbose()) fprintf(stderr, "[embind] type %u=str (%s)\n", id, guest_str(name));
}

void w2c_env_0x5Fembind_register_std_wstring(struct w2c_env* env, u32 id,
                                             u32 name, u32 size) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_std_wstring", (u64)(uintptr_t)env, (u64)id, (u64)name, (u64)size);
  (void)env;
  (void)size;
  add_type(id, "wstr");
  if (verbose()) fprintf(stderr, "[embind] type %u=wstr (%s)\n", id, guest_str(name));
}

void w2c_env_0x5Fembind_register_emval(struct w2c_env* env, u32 id) {
  WEBLOX_IMPORT_TRACE_CALL("w2c_env_0x5Fembind_register_emval", (u64)(uintptr_t)env, (u64)id);
  (void)env;
  add_type(id, "emval");
  if (verbose()) fprintf(stderr, "[embind] type %u=emval\n", id);
}

void w2c_env_0x5Fembind_register_memory_view(struct w2c_env* env, u32 id,
                                             u32 kind, u32 name) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_0x5Fembind_register_memory_view", (u64)(uintptr_t)env, (u64)id, (u64)kind, (u64)name);
  (void)env;
  (void)kind;
  add_type(id, "memory_view");
  if (verbose()) fprintf(stderr, "[embind] type %u=view (%s)\n", id, guest_str(name));
}

/* ---------------- invocation -------------------------------------------- */

static const struct weblox_binding* find_binding(const char* name) {
  for (int i = 0; i < g_count; ++i)
    if (!strcmp(g_bindings[i].name, name)) return &g_bindings[i];
  return NULL;
}

// 0=i32/pointer, 1=i64, 2=f32, 3=f64.  Encodes the wasm signature as a base-4
// number so the typed invoker switch below can be generated with macros.
static unsigned sig_code(unsigned char c) {
  switch (c) {
    case 'j': case 'l': case 'x': return 1;
    case 'f': return 2;
    case 'd': return 3;
    default: return 0;  // p, s, i, c, b, h, t
  }
}

#define INV_T0 u32
#define INV_T1 u64
#define INV_T2 f32
#define INV_T3 f64

static u32 as_0(u64 v) { return (u32)v; }
static u64 as_1(u64 v) { return v; }
static f32 as_2(u64 v) {
  f32 f;
  u32 bits = (u32)v;
  memcpy(&f, &bits, 4);
  return f;
}
static f64 as_3(u64 v) {
  f64 f;
  memcpy(&f, &v, 8);
  return f;
}

#define INV_CASE1(N)                                                      \
  case (N):                                                               \
    ((void (*)(void*, u32, INV_T##N))fn)(module, id, as_##N(raw[0]));      \
    break;
#define INV_CASE2(A, B)                                                   \
  case ((A)*4 + (B)):                                                     \
    ((void (*)(void*, u32, INV_T##A, INV_T##B))fn)(                       \
        module, id, as_##A(raw[0]), as_##B(raw[1]));                      \
    break;
#define INV_CASE3(A, B, C)                                                \
  case ((A)*16 + (B)*4 + (C)):                                            \
    ((void (*)(void*, u32, INV_T##A, INV_T##B, INV_T##C))fn)(             \
        module, id, as_##A(raw[0]), as_##B(raw[1]), as_##C(raw[2]));      \
    break;
#define INV_GROUP2(A) \
  INV_CASE2(A, 0) INV_CASE2(A, 1) INV_CASE2(A, 2) INV_CASE2(A, 3)
#define INV_GROUP3(A, B) \
  INV_CASE3(A, B, 0) INV_CASE3(A, B, 1) INV_CASE3(A, B, 2) INV_CASE3(A, B, 3)

// Copy a C string into guest memory in embind's std::string wire form.
static u32 put_string(w2c_WebClient* inst, const char* s) {
  size_t n = strlen(s);
  if (n > UINT32_MAX - 8) return 0;
  u32 p = w2c_WebClient_malloc(inst, (u32)n + 8);
  if (!p) return 0;
  unsigned char* dst = guest_ptr(p, (u32)n + 8);
  if (!dst) {
    w2c_WebClient_free(inst, p);
    return 0;
  }
  u32 len = (u32)n;
  memcpy(dst, &len, 4);
  memcpy(dst + 4, s, n);
  dst[n + 4] = 0;
  return p;
}

int weblox_embind_call(w2c_WebClient* inst, const char* name, ...) {
  const struct weblox_binding* b = find_binding(name ? name : "");
  if (!inst || !b) {
    fprintf(stderr, "[embind] no binding for %s\n", name ? name : "(null)");
    return 0;
  }

  weblox_arg args[WEBLOX_EMBIND_ARGS];
  int argc = 0;
  va_list ap;
  va_start(ap, name);
  for (;;) {
    weblox_arg a = va_arg(ap, weblox_arg);
    if (a.kind == WEBLOX_KIND_END) break;
    if (argc == WEBLOX_EMBIND_ARGS) {
      va_end(ap);
      fprintf(stderr, "[embind] %s: too many arguments\n", name);
      return 0;
    }
    args[argc++] = a;
  }
  va_end(ap);

  if ((u32)argc != b->argc) {
    fprintf(stderr, "[embind] %s wants %u args (%s), got %d\n", name, b->argc,
            (const char*)b->sig, argc);
    return 0;
  }

  wasm_rt_funcref_table_t* table = &inst->w2c_0x5F_indirect_function_table;
  if (!table->data || b->invoker >= table->size ||
      !table->data[b->invoker].func) {
    fprintf(stderr, "[embind] %s: invoker %u out of range (table %u)\n", name,
            b->invoker, table->size);
    return 0;
  }

  u64 raw[WEBLOX_EMBIND_ARGS] = {0};
  u32 allocated[WEBLOX_EMBIND_ARGS] = {0};
  int nalloc = 0;
  unsigned code = 0;
  for (int k = 0; k < argc; ++k) {
    unsigned c = sig_code(b->sig_len > (u32)(k + 2) ? b->sig[k + 2] : 0);
    code |= c << (2 * k);
    switch (args[k].kind) {
      case WEBLOX_KIND_STRING:
        allocated[nalloc] = put_string(inst, args[k].as.str);
        if (!allocated[nalloc]) {
          fprintf(stderr, "[embind] %s: guest string allocation failed\n", name);
          for (int i = 0; i < nalloc; ++i) w2c_WebClient_free(inst, allocated[i]);
          return 0;
        }
        raw[k] = allocated[nalloc++];
        break;
      case WEBLOX_KIND_I64:
        raw[k] = (u64)args[k].as.i64;
        break;
      case WEBLOX_KIND_F64:
        memcpy(&raw[k], &args[k].as.f64, 8);
        break;
      case WEBLOX_KIND_I32:
      default:
        raw[k] = (u32)args[k].as.i32;
        break;
    }
  }

  wasm_rt_funcref_t* ref = &table->data[b->invoker];
  void* fn = (void*)ref->func;
  void* module = ref->module_instance;
  u32 id = b->id;
  int ok = 1;
  switch (argc) {
    case 0:
      ((void (*)(void*, u32))fn)(module, id);
      break;
    case 1:
      switch (code) {
        INV_CASE1(0)
        INV_CASE1(1)
        INV_CASE1(2)
        INV_CASE1(3)
        default: ok = 0; break;
      }
      break;
    case 2:
      switch (code) {
        INV_GROUP2(0)
        INV_GROUP2(1)
        INV_GROUP2(2)
        INV_GROUP2(3)
        default: ok = 0; break;
      }
      break;
    case 3:
      switch (code) {
        INV_GROUP3(0, 0) INV_GROUP3(0, 1) INV_GROUP3(0, 2) INV_GROUP3(0, 3)
        INV_GROUP3(1, 0) INV_GROUP3(1, 1) INV_GROUP3(1, 2) INV_GROUP3(1, 3)
        INV_GROUP3(2, 0) INV_GROUP3(2, 1) INV_GROUP3(2, 2) INV_GROUP3(2, 3)
        INV_GROUP3(3, 0) INV_GROUP3(3, 1) INV_GROUP3(3, 2) INV_GROUP3(3, 3)
        default: ok = 0; break;
      }
      break;
    default:
      // Bindings seen in this engine take at most two arguments; keep a
      // conservative integer-only fallback for unusual three-plus-argument
      // exports (float args are not representable through integer registers).
      switch (argc) {
        case 4:
          ((void (*)(void*, u32, u64, u64, u64, u64))fn)(
              module, id, raw[0], raw[1], raw[2], raw[3]);
          break;
        default:
          ok = 0;
          break;
      }
      break;
  }

  for (int i = 0; i < nalloc; ++i) w2c_WebClient_free(inst, allocated[i]);
  fprintf(stderr, "[embind] call %s(%d) -> %s\n", name, argc,
          ok ? "ok" : "unsupported signature");
  return ok;
}
