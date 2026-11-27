// Emscripten worker threads for the portable C++ host.
//
// Ported from v3-wasm2c/thread.c.  Emscripten gives every worker its own
// module instance over the same shared linear memory: globals, TLS and the
// funcref table are per worker, only memory is common.  wasm2c's instance
// struct gives exactly that, so a worker is a fresh instance + std::thread.
//
// The guest pthread struct offsets below are relative to the guest pointer
// passed to pthread_create_js (never absolute guest addresses), and the entry
// function comes from that worker instance's indirect function table.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "platform.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" {
extern struct w2c_env g_env;
extern struct w2c_wasi__snapshot__preview1 g_wasi;
void weblox_mailbox_set_worker(void);
}

namespace {

// Relative offsets into the guest pthread struct (see v3-wasm2c/thread.c).
constexpr u32 kStackTopOffset = 48;
constexpr u32 kStackSizeOffset = 52;
constexpr u32 kPthreadStartedOffset = 112;

struct Worker {
  w2c_WebClient inst{};
  u32 pthread_ptr = 0;
  u32 start = 0;
  u32 arg = 0;
};

std::mutex g_workers_mtx;
std::unordered_map<u32, Worker*> g_workers;

u32 read_u32(u32 addr) {
  u32 value = 0;
  if (auto* p = weblox::guest::ptr(addr, 4)) std::memcpy(&value, p, 4);
  return value;
}

void worker_main(Worker* w) {
  // worker threads are never the thread that owns the host event loop.
  weblox_mailbox_set_worker();
  wasm_rt_init_thread();
  wasm2c_WebClient_instantiate(&w->inst, &g_env, &g_wasi);

  const u32 top = read_u32(w->pthread_ptr + kStackTopOffset);
  const u32 bottom = read_u32(w->pthread_ptr + kStackSizeOffset);
  if (top && top > bottom) {
    w2c_WebClient_set_stack_limits(&w->inst, top, top - bottom);
    w2c_WebClient_set_stack_ptr(&w->inst, top);
  }
  weblox::guest::put_u32(w->pthread_ptr + kPthreadStartedOffset, 1);

  w2c_WebClient_0x5Femscripten_thread_init(&w->inst, w->pthread_ptr,
                                           /*is_main=*/0u, /*is_worker=*/1u,
                                           /*initialized=*/1u,
                                           /*stack_size=*/0u,
                                           /*proc_param=*/0u);
  w2c_WebClient_0x5Femscripten_tls_init(&w->inst);

  auto& table = w->inst.w2c_0x5F_indirect_function_table;
  if (w->start < table.size && table.data && table.data[w->start].func) {
    wasm_rt_funcref_t& e = table.data[w->start];
    WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
    WASM_RT_UNWIND_TARGET target;
    if (!wasm_rt_try(target)) {
      wasm_rt_set_unwind_target(&target);
      reinterpret_cast<void (*)(void*, u32)>(e.func)(e.module_instance, w->arg);
      wasm_rt_set_unwind_target(outer);
    } else {
      wasm_rt_set_unwind_target(outer);
      std::fprintf(stderr, "[thread] worker=%u start=%u threw tag=%p bytes=%u\n",
                   w->pthread_ptr, w->start, wasm_rt_exception_tag(),
                   wasm_rt_exception_size());
    }
  } else {
    std::fprintf(stderr, "[thread] worker=%u start=%u out of table (%u)\n",
                 w->pthread_ptr, w->start, table.size);
  }

  std::fprintf(stderr, "[thread] worker=%u finished\n", w->pthread_ptr);
  {
    std::lock_guard<std::mutex> lock(g_workers_mtx);
    g_workers.erase(w->pthread_ptr);
  }
  wasm2c_WebClient_free(&w->inst);
  wasm_rt_free_thread();
  delete w;
}

}  // namespace

extern "C" {

u32 w2c_env_0x5F_pthread_create_js(struct w2c_env*, u32 pthread_ptr, u32,
                                   u32 start, u32 arg) {
  auto* w = new Worker();
  w->pthread_ptr = pthread_ptr;
  w->start = start;
  w->arg = arg;
  {
    std::lock_guard<std::mutex> lock(g_workers_mtx);
    g_workers[pthread_ptr] = w;
  }
  weblox::platform::spawn_thread([w] { worker_main(w); });
  std::fprintf(stderr, "[thread] spawn pthread=%x start=%u arg=%u\n",
               pthread_ptr, start, arg);
  return 0;
}

void w2c_env_0x5Femscripten_thread_cleanup(struct w2c_env*, u32 pthread_ptr) {
  // The worker frees its own instance when its entry returns.  Do not race it
  // by freeing here; Emscripten only uses this import to release TLS data.
  std::fprintf(stderr, "[thread] cleanup pthread=%x\n", pthread_ptr);
}

}  // extern "C"
