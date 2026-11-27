// embind registry + invoke path for the wasm2c host.
//
// The engine registers its bindings through the `_embind_register_*` imports
// during its constructor pass.  Every signature below is taken verbatim from
// the generated WC.h; the generated module calls these as global C symbols.
// `call()` packs a host-side `[id, arg0, ...]` slot array exactly like
// v3-wasm2c/embind.c and dispatches through the instance's indirect function
// table.  See embind.h for the public API and a prepare-step example.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "embind.h"
#include "guest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace weblox::embind {
namespace {

constexpr std::size_t kMaxBindings = 1024;
constexpr std::size_t kMaxTypes = 512;
constexpr std::size_t kMaxArgs = 8;

struct Binding {
  std::string name;
  std::string sig;
  u32 argc = 0;     // number of real args (registration argc minus return type)
  u32 invoker = 0;  // indirect function table index
  u32 id = 0;       // method id passed in slots[0]
};

struct TypeEntry {
  std::string name;
  char kind[16] = {};
  u32 id = 0;
};

Binding g_bindings[kMaxBindings];
std::size_t g_binding_count = 0;
TypeEntry g_types[kMaxTypes];
std::size_t g_type_count = 0;
std::mutex g_mutex;

void add_binding(const char* name, const char* sig, u32 raw_argc, u32 invoker,
                 u32 id) {
  if (!name || !invoker) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_binding_count == kMaxBindings) return;
  Binding& b = g_bindings[g_binding_count++];
  b.name = name;
  b.sig = sig ? sig : "";
  b.argc = raw_argc ? raw_argc - 1 : 0;
  b.invoker = invoker;
  b.id = id;
}

void set_type(u32 id, const char* name, const char* kind) {
  if (!kind) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (std::size_t i = 0; i < g_type_count; ++i) {
    if (g_types[i].id != id) continue;
    g_types[i].name = name ? name : "";
    std::memset(g_types[i].kind, 0, sizeof g_types[i].kind);
    std::memcpy(g_types[i].kind, kind,
                std::min(sizeof g_types[i].kind - 1, std::strlen(kind)));
    return;
  }
  if (g_type_count == kMaxTypes) return;
  TypeEntry& t = g_types[g_type_count++];
  t.id = id;
  t.name = name ? name : "";
  std::memset(t.kind, 0, sizeof t.kind);
  std::memcpy(t.kind, kind, std::min(sizeof t.kind - 1, std::strlen(kind)));
}

// embind signature bytes: return type, hidden id/receiver slot, then args.
// Mirrors sig_kind() in v3-wasm2c/embind.c.
enum class Kind { I32, I64, F32, F64, Str };

Kind sig_kind(unsigned char c) {
  switch (c) {
    case 'p':
    case 's':
      return Kind::Str;
    case 'j':
    case 'l':
    case 'x':
      return Kind::I64;
    case 'f':
      return Kind::F32;
    case 'd':
      return Kind::F64;
    default:  // i, c, b, h, t
      return Kind::I32;
  }
}

// std::string wire form: [u32 length][bytes][NUL], in guest linear memory.
u32 put_string(w2c_WebClient* inst, const std::string& s) {
  const std::size_t n = s.size();
  if (n > UINT32_MAX - 8) return 0;
  const u32 p = w2c_WebClient_malloc(inst, static_cast<u32>(n + 8));
  if (!p) return 0;
  guest::put_u32(p, static_cast<u32>(n));
  unsigned char* dst = guest::ptr(p + 4, n + 1);
  if (!dst) {
    w2c_WebClient_free(inst, p);
    return 0;
  }
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
  return p;
}

// Call the funcref with a wasm exception target installed.  Only trivial
// locals live across wasm_rt_try (setjmp), so a wasm throw never unwinds a
// C++ object.  The slot array is packed by call(), outside this function.
bool invoke(const wasm_rt_funcref_t& e, u32 argc, const u64* slots,
            const std::string& sig) {
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET target;
  if (wasm_rt_try(target)) {
    wasm_rt_set_unwind_target(outer);
    std::fprintf(stderr, "[bind] wasm exception tag=%p bytes=%u\n",
                 const_cast<void*>(wasm_rt_exception_tag()),
                 wasm_rt_exception_size());
    return false;
  }
  wasm_rt_set_unwind_target(&target);
  void* ctx = e.module_instance;
  // A single f32/f64 argument must travel in a floating-point register on
  // common ABIs; passing its bits in an integer slot (as embind.c does for
  // every argument) would be read as garbage by the generated function.
  if (argc == 1 && sig.size() > 2) {
    const Kind want = sig_kind(static_cast<unsigned char>(sig[2]));
    if (want == Kind::F32) {
      f32 v;
      std::memcpy(&v, &slots[1], sizeof v);
      reinterpret_cast<void (*)(void*, u32, f32)>(e.func)(
          ctx, static_cast<u32>(slots[0]), v);
      wasm_rt_set_unwind_target(outer);
      return true;
    }
    if (want == Kind::F64) {
      f64 v;
      std::memcpy(&v, &slots[1], sizeof v);
      reinterpret_cast<void (*)(void*, u32, f64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), v);
      wasm_rt_set_unwind_target(outer);
      return true;
    }
  }
  switch (argc) {
    case 0:
      reinterpret_cast<void (*)(void*, u32)>(e.func)(ctx, static_cast<u32>(slots[0]));
      break;
    case 1:
      reinterpret_cast<void (*)(void*, u32, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1]);
      break;
    case 2:
      reinterpret_cast<void (*)(void*, u32, u64, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2]);
      break;
    case 3:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3]);
      break;
    case 4:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3], slots[4]);
      break;
    case 5:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64, u64, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3], slots[4],
          slots[5]);
      break;
    case 6:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64, u64, u64, u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3], slots[4],
          slots[5], slots[6]);
      break;
    case 7:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64, u64, u64, u64, u64)>(
          e.func)(ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3],
                  slots[4], slots[5], slots[6], slots[7]);
      break;
    case 8:
      reinterpret_cast<void (*)(void*, u32, u64, u64, u64, u64, u64, u64, u64,
                                u64)>(e.func)(
          ctx, static_cast<u32>(slots[0]), slots[1], slots[2], slots[3], slots[4],
          slots[5], slots[6], slots[7], slots[8]);
      break;
    default:
      wasm_rt_set_unwind_target(outer);
      return false;
  }
  wasm_rt_set_unwind_target(outer);
  return true;
}

}  // namespace

bool ready(w2c_WebClient* inst) {
  if (!inst || !inst->w2c_0x5F_indirect_function_table.data) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_binding_count != 0;
}

int call(w2c_WebClient* inst, const char* name, std::initializer_list<Arg> args) {
  if (!inst || !name) return 0;

  Binding b;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool found = false;
    for (std::size_t i = 0; i < g_binding_count; ++i) {
      if (g_bindings[i].name == name) {
        b = g_bindings[i];
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "[bind] no binding %s\n", name);
      return 0;
    }
  }

  if (args.size() > kMaxArgs || args.size() != b.argc) {
    std::fprintf(stderr, "[bind] %s wants %u args (sig=%s), got %zu\n", name,
                 b.argc, b.sig.c_str(), args.size());
    return 0;
  }

  u64 slots[kMaxArgs + 1] = {};
  u32 buffers[kMaxArgs] = {};
  std::size_t nbuf = 0;
  slots[0] = b.id;
  std::size_t k = 0;
  for (const Arg& a : args) {
    const unsigned char code = (k + 2 < b.sig.size())
                                   ? static_cast<unsigned char>(b.sig[k + 2])
                                   : 0;
    if (sig_kind(code) == Kind::Str) {
      const u32 p = put_string(inst, a.string_value());
      if (!p) {
        for (std::size_t i = 0; i < nbuf; ++i)
          w2c_WebClient_free(inst, buffers[i]);
        return 0;
      }
      buffers[nbuf++] = p;
      slots[k + 1] = p;  // guest pointer, never a host address
    } else {
      // i32/i64/f32/f64 all travel as raw wire bits, as in embind.c.
      slots[k + 1] = a.bits();
    }
    ++k;
  }

  bool ok = false;
  const auto& table = inst->w2c_0x5F_indirect_function_table;
  if (table.data && b.invoker < table.size) {
    const wasm_rt_funcref_t& e = table.data[b.invoker];
    if (e.func)
      ok = invoke(e, static_cast<u32>(args.size()), slots, b.sig);
  } else {
    std::fprintf(stderr, "[bind] %s: invoker %u outside table (%u)\n", name,
                 b.invoker, table.size);
  }

  for (std::size_t i = 0; i < nbuf; ++i) w2c_WebClient_free(inst, buffers[i]);
  return ok ? 1 : 0;
}

void dump(w2c_WebClient* inst) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fprintf(stderr, "[embind] instance=%p bindings=%zu types=%zu\n",
               static_cast<void*>(inst), g_binding_count, g_type_count);
  for (std::size_t i = 0; i < g_binding_count; ++i) {
    const Binding& b = g_bindings[i];
    std::fprintf(stderr, "[embind] %s argc=%u inv=%u id=%u sig=%s\n",
                 b.name.c_str(), b.argc, b.invoker, b.id, b.sig.c_str());
  }
  for (std::size_t i = 0; i < g_type_count; ++i) {
    const TypeEntry& t = g_types[i];
    std::fprintf(stderr, "[embind] type %u name=%s kind=%s\n", t.id,
                 t.name.c_str(), t.kind);
  }
}

}  // namespace weblox::embind

// ---------------------------------------------------------------------------
// Registration imports.  Signatures are copied verbatim from WC.h.  C linkage
// ignores the enclosing namespace, so the generated C library still resolves
// these as plain `w2c_env_*` symbols.  The gen_stubs.py skip list must contain
// the prefix-stripped short names so no no-op stub duplicates a definition.
// ---------------------------------------------------------------------------

namespace weblox::embind {

extern "C" void w2c_env_0x5Fembind_register_function(
    struct w2c_env*, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7,
    u32 a8) {
  (void)a3;  // raw type-id array; the signature string carries wire kinds
  (void)a7;
  (void)a8;
  add_binding(guest::string(a1).c_str(), guest::string(a4).c_str(), a2, a5,
              a6);
}

extern "C" void w2c_env_0x5Fembind_register_void(struct w2c_env*, u32 a1,
                                                u32 a2) {
  set_type(a1, guest::string(a2).c_str(), "v");
}

extern "C" void w2c_env_0x5Fembind_register_bool(struct w2c_env*, u32 a1,
                                                u32 a2, u32 a3, u32 a4) {
  (void)a3;  // size
  (void)a4;  // true value
  set_type(a1, guest::string(a2).c_str(), "b");
}

extern "C" void w2c_env_0x5Fembind_register_integer(struct w2c_env*, u32 a1,
                                                   u32 a2, u32 a3, u32 a4,
                                                   u32 a5) {
  (void)a4;  // min
  (void)a5;  // max
  set_type(a1, guest::string(a2).c_str(), a3 == 8 ? "j" : "i");
}

extern "C" void w2c_env_0x5Fembind_register_bigint(struct w2c_env*, u32 a1,
                                                  u32 a2, u32 a3, u64 a4,
                                                  u64 a5) {
  (void)a3;  // size
  (void)a4;  // min
  (void)a5;  // max
  set_type(a1, guest::string(a2).c_str(), "j");
}

extern "C" void w2c_env_0x5Fembind_register_float(struct w2c_env*, u32 a1,
                                                 u32 a2, u32 a3) {
  set_type(a1, guest::string(a2).c_str(), a3 == 8 ? "d" : "f");
}

extern "C" void w2c_env_0x5Fembind_register_std_string(struct w2c_env*, u32 a1,
                                                      u32 a2) {
  set_type(a1, guest::string(a2).c_str(), "s");
}

extern "C" void w2c_env_0x5Fembind_register_std_wstring(struct w2c_env*, u32 a1,
                                                       u32 a2, u32 a3) {
  (void)a3;  // element size
  set_type(a1, guest::string(a2).c_str(), "s");
}

extern "C" void w2c_env_0x5Fembind_register_emval(struct w2c_env*, u32 a1) {
  set_type(a1, "", "e");
}

extern "C" void w2c_env_0x5Fembind_register_memory_view(struct w2c_env*, u32 a1,
                                                       u32 a2, u32 a3) {
  (void)a2;  // typed-array kind
  set_type(a1, guest::string(a3).c_str(), "p");
}

}  // namespace weblox::embind
