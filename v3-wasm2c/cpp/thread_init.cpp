// Main-thread setup.  WasmPlayer.js installs this import before the module's
// static constructors run; without it TLS-backed globals are invalid and the
// engine aborts inside initialize().

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

// Owned by host.cpp, set right after instantiation.
extern "C" w2c_WebClient* g_wc_instance;

extern "C" void w2c_env_0x5Femscripten_init_main_thread_js(struct w2c_env*,
                                                           u32 pthread_ptr) {
  if (!g_wc_instance) return;
  w2c_WebClient_0x5Femscripten_thread_init(g_wc_instance, pthread_ptr,
                                           /*is_main=*/1, /*is_worker=*/1,
                                           /*initialized=*/1,
                                           /*stack_size=*/262144,
                                           /*proc_param=*/0);
  w2c_WebClient_0x5Femscripten_tls_init(g_wc_instance);
}
