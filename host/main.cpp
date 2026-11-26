// Smoke test: prove the Wasmtime VMContext ABI works by calling one guest
// export from a plain C++ process linked against wasm2obj's out.full.o.
#include "vmctx.h"
#include "imports.h"
#include "rna_bridge.h"
#include "route.h"
#include "prepare.h"
#include "table.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <csignal>
#include <fstream>
#include <functional>
#include <setjmp.h>
#include <stdexcept>
#include <thread>
#include <dlfcn.h>

// wasm function indices from the module export section (first defined
// function index is 341, so _wasm_function_N == wasm index N + 341):
//   _emscripten_thread_init = 151421 -> _wasm_function_151080
//   _emscripten_tls_init    = 149940 -> _wasm_function_149599
//   pthread_self           = 150209 -> _wasm_function_149868
// These are global symbols in out.full.o but the linked executable makes
// them local, so dlsym(RTLD_DEFAULT) cannot see them: call them directly.
extern "C" {
uint32_t _wasm_function_149599(void*, void*);
uint32_t _wasm_function_149868(void*, void*);
void wb_call_thread_init(void*, uint32_t);
// Guest exports: __main_argc_argv (wasm index 488) and main_loop_iteration
// (wasm index 517); defined function index = wasm index - 341.
int32_t _wasm_function_147(void*, void*, int32_t, int32_t);
void _wasm_function_176(void*, void*);
}

// Wasmtime's wasm-call ABI is callee-pops for stack arguments (`ret $16`),
// unlike the SysV C ABI.  Calling a 6-arg wasm function through a C prototype
// corrupts rsp, so use a tiny asm shim that matches the wasm convention.
asm(
    ".text\n"
    ".globl wb_call_thread_init\n"
    "wb_call_thread_init:\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  mov %rdi, %rax\n"          // callee vmctx
    "  mov %esi, %edx\n"          // wasm arg 0: pthread_ptr
    "  mov %rax, %rsi\n"          // caller vmctx
    "  mov %rax, %rdi\n"          // callee vmctx
    "  mov $1, %ecx\n"            // wasm arg 1: is_main
    "  mov $1, %r8d\n"            // wasm arg 2
    "  mov $1, %r9d\n"            // wasm arg 3: atomics
    "  pushq $0\n"                // wasm arg 5
    "  pushq $262144\n"           // wasm arg 4: stack size
    "  call _wasm_function_151080\n"  // callee pops the 16 bytes above
    "  mov %rbp, %rsp\n"
    "  pop %rbp\n"
    "  ret\n");

static void main_thread_init(void* vmctx, uint32_t ptr) {
  std::fprintf(stderr, "[pthr] guest thread_init(ptr=%#x) + tls_init\n", ptr);
  wb_call_thread_init(vmctx, ptr);
  _wasm_function_149599(vmctx, vmctx);
  const uint32_t me = _wasm_function_149868(vmctx, vmctx);
  if (me) wb::imports_set_main_thread(me);
}

// Emscripten pthread worker entry: re-enter the wasm function named by the
// table index `start` with a per-thread VMContext.  This is the hook the
// ported __pthread_create_js import requires.
static void* g_main_vmctx = nullptr;

static void thread_entry(uint32_t start, uint32_t arg, uint32_t pthread_ptr) {
  char sym[64];
  std::snprintf(sym, sizeof(sym), "_wasm_function_%u", start - 341u);
  using WasmFn = void (*)(void*, void*, uint32_t);
  auto* fn = reinterpret_cast<WasmFn>(dlsym(RTLD_DEFAULT, sym));
  if (!fn) {
    std::fprintf(stderr, "[pthr] no wasm symbol for start=%u (%s)\n", start, sym);
    return;
  }
  void* vm = std::malloc(VMCTX_SIZE_OF_VMCTX);
  if (!vm) return;
  std::memcpy(vm, g_main_vmctx, VMCTX_SIZE_OF_VMCTX);
  // Give this fibre a fresh stack guard.  vmctx+8 is VMStoreContext; its
  // stack_limit lives at +24 (offsets from vmctx_offsets.h).
  uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  uint64_t limit = static_cast<uint64_t>(sp - (64u << 20));
  auto** store = reinterpret_cast<void**>(static_cast<uint8_t*>(vm) + VMCTX_OFF_STORE_CONTEXT);
  if (*store) {
    std::memcpy(static_cast<uint8_t*>(*store) + 24, &limit, sizeof(limit));
  }
  std::fprintf(stderr, "[pthr] re-enter start=%u (%s) arg=%u ptr=%#x\n", start, sym, arg, pthread_ptr);
  fn(vm, vm, arg);
  std::free(vm);
}

// The join path calls guest exports that may trap. vmctx.cpp's handler only
// longjmps when its (internal) g_in_wasm flag is set, so install a temporary
// handler here; a trapped export becomes a printed signal instead of a crash.
namespace {
sigjmp_buf g_export_jmp;
volatile sig_atomic_t g_export_active = 0;
volatile sig_atomic_t g_export_signal = 0;
void export_trap(int sig, siginfo_t*, void*) {
  if (!g_export_active) return;
  g_export_signal = sig;
  g_export_active = 0;
  siglongjmp(g_export_jmp, sig);
}

class ExportTrapGuard {
 public:
  ExportTrapGuard() {
    struct sigaction sa {};
    sa.sa_sigaction = &export_trap;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int kSignals[] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS};
    for (int i = 0; i < 4; ++i) {
      sigaction(kSignals[i], &sa, &old_[i]);
    }
  }
  ~ExportTrapGuard() {
    static const int kSignals[] = {SIGSEGV, SIGFPE, SIGILL, SIGBUS};
    for (int i = 0; i < 4; ++i) sigaction(kSignals[i], &old_[i], nullptr);
  }
  // Returns 0 on normal return, or the trapping signal number.
  int call(const std::function<void()>& fn) {
    g_export_signal = 0;
    if (sigsetjmp(g_export_jmp, 1) != 0) return g_export_signal;
    g_export_active = 1;
    fn();
    g_export_active = 0;
    return 0;
  }

 private:
  struct sigaction old_[4];
};

std::string read_cookie(const char* path) {
  if (!path || !*path) return {};
  std::ifstream f(path);
  std::string line;
  if (!std::getline(f, line)) return {};
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                           line.back() == ' ' || line.back() == '\t')) {
    line.pop_back();
  }
  return line;
}
}  // namespace

int main(int argc, char** argv) {
  // --skip-ctors is useful because __wasm_call_ctors runs Emscripten's full
  // startup with hundreds of host imports; the stub imports cannot satisfy it.
  bool skip_ctors = false, test_trap = false, rna_selftest = false,
       rna_probe = false, list_bindings = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--skip-ctors") skip_ctors = true;
    if (a == "--test-trap") test_trap = true;
    if (a == "--rna-selftest") rna_selftest = true;
    if (a == "--rna-probe") rna_probe = true;
    if (a == "--bindings") list_bindings = true;
  }
  host::HostState state;
  if (!host::setup(state)) {
    std::fprintf(stderr, "[wasm-host] RESULT=FAIL setup failed\n");
    return 1;
  }
  g_main_vmctx = state.vmctx;
  wb::imports_set_thread_entry(&thread_entry);
  wb::imports_set_main_thread_init(&main_thread_init);
  std::printf("[wasm-host] vmctx=%p size=%u memory=%p length=%zu imports=%u builtins=%u\n",
              static_cast<void*>(state.vmctx), VMCTX_SIZE_OF_VMCTX,
              static_cast<void*>(state.memory_base), state.memory_def_storage.current_length,
              VMCTX_NUM_IMPORTED_FUNCTIONS, BUILTIN_FUNCTIONS_ARRAY_LEN);
  std::fflush(stdout);

  // Primary ABI proof: real wasm exports write and read VMContext globals.
  uint32_t stack_base = 0, stack_end = 0;
  int roundtrip = host::call_stack_roundtrip(state, &stack_base, &stack_end);
  if (roundtrip == 0) {
    std::printf("[wasm-host] ABI ROUNDTRIP OK: emscripten_stack_set_limits -> "
                "get_stack_base=%u get_stack_end=%u\n", stack_base, stack_end);
  } else {
    std::printf("[wasm-host] ABI ROUNDTRIP FAILED: %d (base=%u end=%u)\n",
                roundtrip, stack_base, stack_end);
  }
  std::fflush(stdout);

  if (test_trap) {
    int sig = host::call_trap_test(state);
    std::printf("[wasm-host] exported __trap: %s\n",
                sig ? "converted to controlled error" : "unexpectedly returned");
    std::fflush(stdout);
  }

  if (skip_ctors || test_trap) {
    std::printf("[wasm-host] __wasm_call_ctors: skipped (--skip-ctors/--test-trap)\n");
  } else {
    int ctor_trap = host::call_ctors(state);
    if (ctor_trap == 0) {
      std::printf("[wasm-host] __wasm_call_ctors (_wasm_function_0): returned normally\n");
    } else {
      std::printf("[wasm-host] __wasm_call_ctors (_wasm_function_0): trapped via signal %d "
                  "(converted to controlled error)\n", ctor_trap);
    }
  }
  if (list_bindings) {
    // Diagnostic only: show what ctors actually registered, without invoking
    // any binding. Used to verify the prepare() port's ABI assumptions.
    static const char* kNames[] = {
        "set_pretend_platform", "set_roblox_base_url", "set_channel",
        "set_browser_tracker_id", "set_account_locales",
        "set_preloaded_flags", "set_place_id",
    };
    for (const char* name : kNames) {
      const wb::Binding* b = wb::embind_find(name);
      if (!b) {
        std::printf("[bindings] %-26s MISSING\n", name);
        continue;
      }
      const bool live = b->invoker < wb_table_size && state.table_funcrefs &&
                        state.table_funcrefs[b->invoker].wasm_call;
      std::printf("[bindings] %-26s sig=%-8s argc=%u invoker=%u id=%u table=%d\n",
                  name, b->sig, b->argc, b->invoker, b->id, live ? 1 : 0);
    }
    std::printf("[wasm-host] import calls=%llu controlled errors=%u\n",
                static_cast<unsigned long long>(state.import_call_count),
                static_cast<unsigned>(state.error_count));
    std::fflush(stdout);
    return 0;
  }
  // Optional RNA integration. WC_PLACE + WC_COOKIE_FILE switch on a bounded
  // join attempt; --rna-selftest only checks the native CONNECT path.
  const char* place_env = std::getenv("WC_PLACE");
  const char* cookie_path = std::getenv("WC_COOKIE_FILE");
  const bool join_mode = place_env && *place_env && cookie_path && *cookie_path;
  if (rna_selftest || rna_probe || join_mode) {
    try {
      const std::string cookie = read_cookie(cookie_path);
      const uint64_t place = (place_env && *place_env)
                                 ? std::strtoull(place_env, nullptr, 10)
                                 : 189707;
      weblox::FetchOptions fo;
      fo.max_attempts = 3;
      fo.timeout_ms = 20000;
      fo.connect_timeout_ms = 10000;
      const weblox::JoinRoute route = weblox::fetch(place, cookie, fo);
      std::printf("[rna-main] route: fqdn=%s host=%s port=%u caps=%s "
                  "cert=%s place=%llu status=%ld\n",
                  route.fqdn.c_str(), route.host.c_str(), route.port,
                  route.caps.c_str(), route.cert_hash.c_str(),
                  static_cast<unsigned long long>(route.place_id), route.status);
      std::fflush(stdout);

      weblox::RnaBridge bridge;
      bridge.attach(state);
      bridge.set_config(route);
      if (rna_probe) {
        // Bounded bridge-level probe: a synthetic connect_js with a real guest
        // receive ring, then drive the guest-tick callback hook. No engine
        // startup / game session is attempted.
        constexpr uint32_t kSlotSize = 65536, kSlotCount = 4;
        int mtrap = 0;
        const uint32_t base = host::call_malloc(
            state, kSlotSize * kSlotCount + 16, mtrap);
        if (!base || mtrap) throw std::runtime_error("probe ring malloc failed");
        char url[1024];
        std::snprintf(url, sizeof(url),
                      "https://%s:%u/wt?X%%2DRbx%%2DCapabilities=%s"
                      "&X-Rbx-Close-Reason=utf8-v1",
                      route.fqdn.c_str(), route.port, route.caps.c_str());
        bridge.set_config([&] {
          auto c = weblox::RnaBridge::Config{};
          c.host = route.host;
          c.port = route.port;
          c.fqdn = route.fqdn;
          c.caps = route.caps;
          c.cert_hash = route.cert_hash;
          c.connect_timeout_ms = 8000;
          return c;
        }());
        weblox::RnaConnectArgs args;
        args.id = 1;
        args.url = url;
        args.ring.data = base;
        args.ring.head = base + kSlotSize * kSlotCount;
        args.ring.tail = args.ring.head + 4;
        args.ring.slot = kSlotSize;
        args.ring.count = kSlotCount;
        std::printf("[rna-main] probe url=%s ring=%u/%u/%u\n", url,
                    args.ring.data, args.ring.head, args.ring.tail);
        std::fflush(stdout);
        bridge.connect(args);
        ExportTrapGuard guard;
        uint64_t ticks = 0;
        unsigned probe_ms = 3000;
        if (const char* s = std::getenv("WC_LOOP_MS")) {
          probe_ms = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
          if (probe_ms > 10000) probe_ms = 10000;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(probe_ms);
        int tick_sig = 0;
        while (std::chrono::steady_clock::now() < deadline && tick_sig == 0) {
          tick_sig = guard.call([&] { bridge.guest_tick(); });
          ++ticks;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto s = bridge.stats();
        std::printf("[rna-main] probe ticks=%llu trap=%d connects=%llu "
                    "status=%lld callbacks=%llu/%llu ring=%llu/%llu\n",
                    static_cast<unsigned long long>(ticks), tick_sig,
                    static_cast<unsigned long long>(s.connects),
                    static_cast<long long>(s.connect_status),
                    static_cast<unsigned long long>(s.callbacks_delivered),
                    static_cast<unsigned long long>(s.callbacks_failed),
                    static_cast<unsigned long long>(s.ring_slots_written),
                    static_cast<unsigned long long>(s.ring_drops));
        std::fflush(stdout);
        bridge.stop_all();
        return tick_sig ? 4 : 0;
      }
      if (rna_selftest) {
        unsigned secs = 5;
        if (const char* s = std::getenv("WC_SELFTEST_SECS")) {
          secs = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
          if (secs == 0 || secs > 30) secs = 5;
        }
        std::string report;
        const bool connected = bridge.selftest(secs, &report);
        std::printf("[rna-main] selftest connected=%d %s\n",
                    connected ? 1 : 0, report.c_str());
        std::fflush(stdout);
        return connected ? 0 : 3;
      }

      ExportTrapGuard guard;
      // Engine bootstrap: the browser host calls prepare() before entering
      // __main_argc_argv. Without it initialize() throws before any RNA import
      // can run. --skip-ctors has no embind table, so there is nothing to set.
      bool prep_ok = false;
      int prep_sig = 0;
      if (skip_ctors) {
        std::printf("[rna-main] prepare: skipped (--skip-ctors)\n");
      } else {
        std::string meta_path = "data/meta.json";
        if (const char* e = std::getenv("WC_META"); e && *e) meta_path = e;
        std::string content_base = "data/content";
        if (const char* e = std::getenv("WC_CONTENT"); e && *e) content_base = e;
        prep_sig = guard.call([&] {
          prep_ok = weblox::prepare(state, place, cookie, meta_path, content_base);
        });
        std::printf("[rna-main] prepare ok=%d trap=%d meta=%s content=%s\n",
                    prep_ok ? 1 : 0, prep_sig, meta_path.c_str(),
                    content_base.c_str());
        std::fflush(stdout);
      }

      int argv_sig = 0;
      uint32_t argv = 0;
      if (prep_sig == 0 || skip_ctors) {
        argv_sig = guard.call([&] { argv = weblox::guest_argv(state); });
      }
      std::printf("[rna-main] __main_argc_argv argc=1 argv=%u "
                  "(argv_trap=%d)\n", argv, argv_sig);
      std::fflush(stdout);
      const int main_sig = guard.call([&] {
        _wasm_function_147(state.vmctx, state.vmctx, 1,
                           static_cast<int32_t>(argv));
      });
      std::printf("[rna-main] __main_argc_argv: %s\n",
                  main_sig ? ("trapped via signal " + std::to_string(main_sig)).c_str()
                           : "returned normally");
      std::fflush(stdout);
      if (main_sig == 0) {
        unsigned loop_ms = 3000;
        if (const char* s = std::getenv("WC_LOOP_MS")) {
          loop_ms = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
          if (loop_ms > 60000) loop_ms = 60000;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(loop_ms);
        uint64_t turns = 0;
        int iter_sig = 0;
        while (std::chrono::steady_clock::now() < deadline && iter_sig == 0) {
          iter_sig = guard.call([&] { _wasm_function_176(state.vmctx, state.vmctx); });
          ++turns;
          if (turns % 100 == 0) {
            const auto s = bridge.stats();
            std::printf("[rna-main] turns=%llu connects=%llu dgrams=%llu/%llu "
                        "ring=%llu drop=%llu cb=%llu/%llu status=%lld\n",
                        static_cast<unsigned long long>(turns),
                        static_cast<unsigned long long>(s.connects),
                        static_cast<unsigned long long>(s.datagrams_sent),
                        static_cast<unsigned long long>(s.datagrams_recv),
                        static_cast<unsigned long long>(s.ring_slots_written),
                        static_cast<unsigned long long>(s.ring_drops),
                        static_cast<unsigned long long>(s.callbacks_delivered),
                        static_cast<unsigned long long>(s.callbacks_failed),
                        static_cast<long long>(s.connect_status));
            std::fflush(stdout);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::printf("[rna-main] loop turns=%llu iter_trap=%d\n",
                    static_cast<unsigned long long>(turns), iter_sig);
      }
      const auto s = bridge.stats();
      std::printf("[rna-main] final: connects=%llu connect_errors=%llu "
                  "callbacks=%llu/%llu status=%lld error=%s\n",
                  static_cast<unsigned long long>(s.connects),
                  static_cast<unsigned long long>(s.connect_errors),
                  static_cast<unsigned long long>(s.callbacks_delivered),
                  static_cast<unsigned long long>(s.callbacks_failed),
                  static_cast<long long>(s.connect_status),
                  s.last_error.c_str());
      std::fflush(stdout);
      bridge.stop_all();
    } catch (const std::exception& e) {
      std::printf("[rna-main] join attempt failed: %s\n", e.what());
      std::fflush(stdout);
      return 3;
    }
  }

  std::printf("[wasm-host] import calls=%llu controlled errors=%u\n",
              static_cast<unsigned long long>(state.import_call_count),
              static_cast<unsigned>(state.error_count));
  std::fflush(stdout);

  int malloc_trap = 0;
  uint32_t ptr = host::call_malloc(state, 16, malloc_trap);
  if (malloc_trap == 0) {
    std::printf("[wasm-host] malloc(16) (_wasm_function_149984): returned %u (0x%x)\n", ptr, ptr);
  } else {
    std::printf("[wasm-host] malloc(16): trapped via signal %d (controlled)\n", malloc_trap);
  }
  bool ok = (roundtrip == 0 && malloc_trap == 0);
  std::printf("[wasm-host] RESULT=%s\n", (ok ? "OK" : "FAIL"));
  return ok ? 0 : 2;
}
