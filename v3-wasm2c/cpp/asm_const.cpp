// Emscripten EM_ASM dispatch.  The module passes a numeric key, a pointer to
// a signature string and a packed argument buffer.  These keys are page-side
// closure IDs from WebClient.js (not wasm indices); we implement the ones with
// an observable effect on control flow and log the rest.
//
// Ported from v3-wasm2c/glue.c.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "platform.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace {

struct Arg {
  char kind;
  double number;
  u32 pointer;
};

// The signature advances 4 bytes per i32/pointer and 8 per i64/f64, aligned.
int decode(u32 sigp, u32 buf, Arg* out, int max = 8) {
  const char* sig = weblox::guest::str(sigp);
  int n = 0;
  u32 off = 0;
  for (const char* c = sig; *c && n < max; ++c) {
    bool wide = (*c == 'j' || *c == 'd');
    if (wide && (off & 7)) off += 4;
    Arg a{*c, 0.0, 0};
    if (auto* p = weblox::guest::ptr(buf + off, wide ? 8 : 4)) {
      if (*c == 'i') {
        u32 v;
        std::memcpy(&v, p, 4);
        a.number = static_cast<double>(static_cast<s32>(v));
      } else if (*c == 'f') {
        float v;
        std::memcpy(&v, p, 4);
        a.number = v;
      } else if (*c == 'd' || *c == 'j') {
        if (*c == 'd') {
          double v;
          std::memcpy(&v, p, 8);
          a.number = v;
        } else {
          u64 v;
          std::memcpy(&v, p, 8);
          a.number = static_cast<double>(v);
        }
      } else {
        u32 v;
        std::memcpy(&v, p, 4);
        a.pointer = v;
      }
    }
    out[n++] = a;
    off += wide ? 8 : 4;
  }
  return n;
}

double eval(u32 key, u32 sigp, u32 buf) {
  Arg a[8];
  int n = buf ? decode(sigp, buf, a) : 0;
  switch (key) {
    case 8667121:  // performance.now()
      return static_cast<double>(weblox::platform::now_ms());
    case 8667344:  // wasmPreInitTime
    case 8667510:  // navigator.webdriver
      return 0.0;
    case 8667550:  // main() entered
      std::fprintf(stderr, "[asm] main entered\n");
      return 0.0;
    case 8667715: {  // loading status
      const char* msg = n > 2 ? weblox::guest::str(a[2].pointer) : "";
      std::fprintf(stderr, "[asm] loading %s status=%d msg=\"%s\"\n",
                   n > 0 && a[0].number ? "FAILURE" : "status",
                   n > 1 ? static_cast<int>(a[1].number) : -1, msg);
      return 0.0;
    }
    case 8667386:  // join failed
      std::fprintf(stderr, "[asm] JOIN FAILED statusCode=%d\n",
                   n ? static_cast<int>(a[0].number) : -1);
      return 0.0;
    case 8667151:
      std::fprintf(stderr, "[asm] EXPERIENCE JOINED\n");
      return 0.0;
    case 8667221:
      std::fprintf(stderr, "[asm] EXPERIENCE LEFT\n");
      return 0.0;
    case 8679911:  // crash report hook (type, message)
      std::fprintf(stderr, "[asm] CRASH type=\"%s\" msg=\"%s\"\n",
                   n > 0 ? weblox::guest::str(a[0].pointer) : "",
                   n > 1 ? weblox::guest::str(a[1].pointer) : "");
      return 0.0;
    case 8686787:  // CORS header warning
      std::fprintf(stderr, "[asm] cors: %s\n",
                   n ? weblox::guest::str(a[0].pointer) : "");
      return 0.0;
    case 8683155:
    case 8686439:
    case 8680027:
      return 48000.0;  // FMOD sample rate
    case 8687350: {  // getRandomValue
      std::random_device rd;
      return static_cast<double>(rd());
    }
    default:
      std::fprintf(stderr, "[asmc] key=%u argc=%d\n", key, n);
      return 0.0;
  }
}

}  // namespace

extern "C" {

u32 w2c_env_emscripten_asm_const_int(struct w2c_env*, u32 key, u32 sig,
                                     u32 buf) {
  return static_cast<u32>(eval(key, sig, buf));
}

f64 w2c_env_emscripten_asm_const_double(struct w2c_env*, u32 key, u32 sig,
                                        u32 buf) {
  return eval(key, sig, buf);
}

u32 w2c_env_emscripten_asm_const_int_sync_on_main_thread(struct w2c_env* e,
                                                         u32 key, u32 sig,
                                                         u32 buf) {
  return w2c_env_emscripten_asm_const_int(e, key, sig, buf);
}

void w2c_env_emscripten_asm_const_async_on_main_thread(struct w2c_env* e,
                                                       u32 key, u32 sig,
                                                       u32 buf) {
  (void)w2c_env_emscripten_asm_const_int(e, key, sig, buf);
}

}  // extern "C"
