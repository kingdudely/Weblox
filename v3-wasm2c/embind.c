/* impl/embind.c - embind registry.
 *
 * The engine exposes its host-facing API through embind.  The glue stores, per
 * registered name, an "invoker" (a thunk that unpacks the JS argument vector)
 * plus a numeric method id.  The thunk is called as
 *
 *     invoker(rv, argc, args)
 *
 * where `args` points at `[id, arg0, arg1, ...]` - `args[0]` is the method id
 * the thunk dispatches on.  String arguments are `[len][bytes]` structs; the
 * thunk copies them out and hands the C++ side a std::string, so a "string"
 * here means allocating that struct, not a NUL-terminated C string.
 */
#include "../host/host.h"
#include <stdarg.h>

enum kind { K_VOID, K_STR, K_I32, K_I64, K_F32, K_F64 };

struct binding {
  char name[96];
  char sig[32];
  u32 argc;
  u32 invoker;
  u32 id;
};
static struct binding g_b[512];
static int g_nb;

int wc_embind_count(void) { return g_nb; }
const char* wc_embind_name(int i) { return (i >= 0 && i < g_nb) ? g_b[i].name : NULL; }

static void add(const char* name, const char* sig, u32 argc, u32 invoker, u32 id) {
  if (g_nb == 512 || !name || !invoker) return;
  snprintf(g_b[g_nb].name, sizeof g_b[0].name, "%s", name);
  snprintf(g_b[g_nb].sig, sizeof g_b[0].sig, "%s", sig ? sig : "");
  g_b[g_nb].argc = argc ? argc - 1 : 0;
  g_b[g_nb].invoker = invoker;
  g_b[g_nb].id = id;
  if (!strncmp(name, "set_", 4))
    fprintf(stderr, "[bindsig] %s argc=%u sig=", name, g_b[g_nb].argc);
  if (!strncmp(name, "set_", 4)) {
    for (const unsigned char* q = (const unsigned char*)(sig ? sig : ""); *q; q++)
      fprintf(stderr, "%02x(%c) ", *q, *q >= 32 && *q < 127 ? *q : '.');
    fprintf(stderr, "\n");
  }
  g_nb++;
}

void wc_impl_0x5Fembind_register_function(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4,
                                          u32 a5, u32 a6, u32 a7, u32 a8) {
  (void)a0; (void)a3; (void)a7; (void)a8;
  add(wc_str(a1), wc_str(a4), a2, a5, a6);
  if (g_wc_verbose && g_nb <= 6) {
    wasm_rt_funcref_t* fi = &g_inst->w2c_0x5F_indirect_function_table.data[a5];
    wasm_rt_funcref_t* f2 = &g_inst->w2c_0x5F_indirect_function_table.data[a6];
    fprintf(stderr, "[bind] %s inv=%u(type %p) id=%u(type %p)\n",
            wc_str(a1), a5, (void*)fi->func_type, a6, (void*)f2->func_type);
  }
}

void wc_impl_0x5Fembind_register_std_string(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void wc_impl_0x5Fembind_register_std_wstring(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void wc_impl_0x5Fembind_register_integer(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void wc_impl_0x5Fembind_register_float(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void wc_impl_0x5Fembind_register_bool(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void wc_impl_0x5Fembind_register_void(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void wc_impl_0x5Fembind_register_bigint(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u64 a4, u64 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void wc_impl_0x5Fembind_register_emval(struct w2c_env* a0, u32 a1) { (void)a0; (void)a1; }
void wc_impl_0x5Fembind_register_memory_view(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }

static struct binding* find(const char* name) {
  for (int i = 0; i < g_nb; i++)
    if (!strcmp(g_b[i].name, name)) return &g_b[i];
  return NULL;
}

static int sig_kind(char c) {
  switch (c) {
    case 'p': case 's': return K_STR;
    case 'j': case 'l': case 'x': return K_I64;
    case 'i': case 'c': case 'b': case 'h': case 't': return K_I32;
    case 'f': return K_F32;
    case 'd': return K_F64;
    default: return K_I32;
  }
}

static u32 put_str(const char* s) {
  size_t n = strlen(s);
  u32 p = w2c_WebClient_malloc(g_inst, (u32)n + 8);
  if (!p) return 0;
  *(u32*)wc_ptr(p) = (u32)n;
  memcpy((char*)wc_ptr(p) + 4, s, n + 1);
  return p;
}

static u32 put_i32(int v) {
  u32 p = w2c_WebClient_malloc(g_inst, 8);
  if (!p) return 0;
  memset(wc_ptr(p), 0, 8);
  *(int*)wc_ptr(p) = v;
  return p;
}

static u32 put_i64(long long v) {
  u32 p = w2c_WebClient_malloc(g_inst, 8);
  if (!p) return 0;
  *(long long*)wc_ptr(p) = v;
  return p;
}

static u32 put_f64(double v, int is_f32) {
  u32 p = w2c_WebClient_malloc(g_inst, 8);
  if (!p) return 0;
  memset(wc_ptr(p), 0, 8);
  if (is_f32) *(float*)wc_ptr(p) = (float)v;
  else *(double*)wc_ptr(p) = v;
  return p;
}

/* call a binding: the thunk takes (id, arg0, arg1, ...) as separate wasm
 * arguments - there is no argc, the marshalling is encoded in the signature.
 * The signature is `v`, the receiver placeholder, then one char per argument,
 * so argument k is at sig[k + 2]. */
static int call_va(struct binding* b, int argc, va_list ap) {
  u64 w[4] = {0, 0, 0, 0};
  w[0] = b->id;
  for (int k = 0; k < argc; k++) {
    switch (sig_kind(b->sig[k + 2])) {
      case K_STR: {
        const char* s = va_arg(ap, const char*);
        w[k + 1] = put_str(s ? s : "");
        break;
      }
      case K_I64: w[k + 1] = (u64)va_arg(ap, long long); break;
      case K_F32: { float f = (float)va_arg(ap, double); u32 t; memcpy(&t, &f, 4); w[k + 1] = t; break; }
      case K_F64: { double d = va_arg(ap, double); memcpy(&w[k + 1], &d, 8); break; }
      default: w[k + 1] = (u32)va_arg(ap, int); break;
    }
  }

  wasm_rt_funcref_t* e = &g_inst->w2c_0x5F_indirect_function_table.data[b->invoker];
  if (!e->func) return 0;
  void* in = e->module_instance;
  switch (argc) {
    case 0: ((void (*)(void*, u32))e->func)(in, (u32)w[0]); break;
    case 1: ((void (*)(void*, u32, u64))e->func)(in, (u32)w[0], w[1]); break;
    case 2: ((void (*)(void*, u32, u64, u64))e->func)(in, (u32)w[0], w[1], w[2]); break;
    default: ((void (*)(void*, u32, u64, u64, u64))e->func)(in, (u32)w[0], w[1], w[2], w[3]); break;
  }
  return 1;
}

int wc_embind_invoke(const char* name, int argc, ...) {
  struct binding* b = find(name);
  fprintf(stderr, "[bindcall] %s argc=%d have=%d sig='%s'\n", name, argc,
          b ? (int)b->argc : -1, b ? b->sig : "");
  if (!b) {
    if (g_wc_verbose) fprintf(stderr, "[bind] no binding %s\n", name);
    return 0;
  }
  if ((u32)argc != b->argc) {
    fprintf(stderr, "[bind] %s wants %u args (%s), got %d\n", name, b->argc, b->sig, argc);
    return 0;
  }
  va_list ap;
  va_start(ap, argc);
  int r = call_va(b, argc, ap);
  va_end(ap);
  return r;
}

void wc_embind_dump(void) {
  for (int i = 0; i < g_nb; i++)
    fprintf(stderr, "[bind] %s/%u inv=%u id=%u sig=%s\n",
            g_b[i].name, g_b[i].argc, g_b[i].invoker, g_b[i].id, g_b[i].sig);
}
