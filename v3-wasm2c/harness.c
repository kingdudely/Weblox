#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "WC.h"
#include "host/plat.h"
#include "host/host.h"
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

struct w2c_env;
struct w2c_wasi__snapshot__preview1;
/* probe-mode: the module does 32-bit atomics on odd addresses; x86 lock ops tolerate it */
void wasm_rt_trap_impl(wasm_rt_trap_t);
extern wasm_rt_shared_memory_t g_mem;
extern void wc_bt_dump(void);
static void abrt(int sig, siginfo_t* si, void* uc) {
  (void)sig; (void)si;
  write(2, "\n[SIGABRT]\n", 11);
  { ucontext_t* u = uc; void** fp = (void**)u->uc_mcontext.gregs[REG_RBP];
    for (int i = 0; i < 24 && fp; i++) {
      char b[64];
      int n = snprintf(b, sizeof b, "[bt] %2d %p\n", i, fp[1]);
      write(2, b, n);
      void** next = (void**)fp[0];
      if (!next || next <= fp) break;
      fp = next;
    } }
  _exit(71);
}
static void segv(int sig, siginfo_t* si, void* uc) {
  (void)sig; (void)uc;
  char b[256];
  ucontext_t* uc_ = uc;
  int n = snprintf(b, sizeof b,
    "\n[segv] addr=%p rip=%p rax=%llx rcx=%llx g0=%x base=%p end=%p sp=%x size=%llx\n",
    si->si_addr, (void*)uc_->uc_mcontext.gregs[REG_RIP],
    (unsigned long long)uc_->uc_mcontext.gregs[REG_RAX],
    (unsigned long long)uc_->uc_mcontext.gregs[REG_RCX],
    g_inst ? g_inst->w2c_g0 : 0,
    (void*)g_mem.data, (void*)(g_mem.data + g_mem.size),
    g_inst ? w2c_WebClient_get_stack_ptr(g_inst) : 0,
    (unsigned long long)g_mem.size);
  write(2, b, n);
  { ucontext_t* u = uc; void** fp = (void**)u->uc_mcontext.gregs[REG_RBP];
    for (int i = 0; i < 20 && fp; i++) {
      char c[64];
      int m = snprintf(c, sizeof c, "[sbt] %2d %p\n", i, fp[1]);
      write(2, c, m);
      void** nx = (void**)fp[0];
      if (!nx || nx <= fp) break;
      fp = nx;
    } }
  _exit(70);
}
static int g_ua = 0;
void wasm_rt_trap(wasm_rt_trap_t code) {
  if (code == WASM_RT_TRAP_UNALIGNED) {
    if (g_ua++ < 12) fprintf(stderr, "[unaligned-atomic ignored #%d]\n", g_ua);
    return;
  }
  wasm_rt_trap_impl(code);
}
struct w2c_env {
  u32 (*w2c_env_HaveOffsetConverter)(struct w2c_env*);
  void (*w2c_env_RBX_restoreWasmStackTraceLimit)(struct w2c_env*, f64);
  f64 (*w2c_env_RBX_setFullWasmStackTrace)(struct w2c_env*);
  void (*w2c_env_0x5F_assert_fail)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi)(struct w2c_env*, u32, u32, f64);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_clear)(struct w2c_env*);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_contains)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_file_size)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer_offset)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_offset)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list_sized)(struct w2c_env*, u32, u32);
  void (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_open)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove_batch)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5F_asyncjs_0x5Frbx_opfs_set)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_0x5F_call_sighandler)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5F_pthread_create_js)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_0x5F_syscall_getegid32)(struct w2c_env*);
  u32 (*w2c_env_0x5F_syscall_geteuid32)(struct w2c_env*);
  u32 (*w2c_env_0x5F_syscall_getgid32)(struct w2c_env*);
  u32 (*w2c_env_0x5F_syscall_getuid32)(struct w2c_env*);
  void (*w2c_env_0x5Fabort_js)(struct w2c_env*);
  void (*w2c_env_0x5Fembind_register_bigint)(struct w2c_env*, u32, u32, u32, u64, u64);
  void (*w2c_env_0x5Fembind_register_bool)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_emval)(struct w2c_env*, u32);
  void (*w2c_env_0x5Fembind_register_float)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_function)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_integer)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_memory_view)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_std_string)(struct w2c_env*, u32, u32);
  void (*w2c_env_0x5Fembind_register_std_wstring)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_0x5Fembind_register_void)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_0x5Femscripten_fetch_get_response_headers)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_0x5Femscripten_fetch_get_response_headers_length)(struct w2c_env*, u32);
  void (*w2c_env_0x5Femscripten_init_main_thread_js)(struct w2c_env*, u32);
  void (*w2c_env_0x5Femscripten_log_formatted)(struct w2c_env*, u32, u32);
  void (*w2c_env_0x5Femscripten_notify_mailbox_postmessage)(struct w2c_env*, u32, u32);
  f64 (*w2c_env_0x5Femscripten_receive_on_main_thread_js)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_0x5Femscripten_runtime_keepalive_clear)(struct w2c_env*);
  void (*w2c_env_0x5Femscripten_thread_cleanup)(struct w2c_env*, u32);
  void (*w2c_env_0x5Femscripten_thread_mailbox_await)(struct w2c_env*, u32);
  void (*w2c_env_0x5Femscripten_thread_set_strongref)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5Fgmtime_js)(struct w2c_env*, u64, u32);
  u32 (*w2c_env_0x5Flocaltime_js)(struct w2c_env*, u64, u32);
  u64 (*w2c_env_0x5Fmktime_js)(struct w2c_env*, u32);
  u32 (*w2c_env_0x5Fmsync_js)(struct w2c_env*, u32, u32, u32, u32, u32, u64);
  void (*w2c_env_0x5Ftzset_js)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_applyPendingCanvasResize)(struct w2c_env*, u32, u32);
  void (*w2c_env_blur_text_input)(struct w2c_env*);
  void (*w2c_env_choose_current_text_input)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_copy_pending_text)(struct w2c_env*, u32);
  void (*w2c_env_emscripten_asm_const_async_on_main_thread)(struct w2c_env*, u32, u32, u32);
  f64 (*w2c_env_emscripten_asm_const_double)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_asm_const_int)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_asm_const_int_sync_on_main_thread)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_emscripten_async_call)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_emscripten_check_blocking_allowed)(struct w2c_env*);
  void (*w2c_env_emscripten_console_error)(struct w2c_env*, u32);
  void (*w2c_env_emscripten_console_log)(struct w2c_env*, u32);
  void (*w2c_env_emscripten_console_warn)(struct w2c_env*, u32);
  f64 (*w2c_env_emscripten_date_now)(struct w2c_env*);
  void (*w2c_env_emscripten_errn)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_exit_pointerlock)(struct w2c_env*);
  void (*w2c_env_emscripten_exit_with_live_runtime)(struct w2c_env*);
  void (*w2c_env_emscripten_fetch_free)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_get_canvas_element_size)(struct w2c_env*, u32, u32, u32);
  f64 (*w2c_env_emscripten_get_device_pixel_ratio)(struct w2c_env*);
  u32 (*w2c_env_emscripten_get_element_css_size)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_get_gamepad_status)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_get_heap_max)(struct w2c_env*);
  f64 (*w2c_env_emscripten_get_now)(struct w2c_env*);
  u32 (*w2c_env_emscripten_get_num_gamepads)(struct w2c_env*);
  u32 (*w2c_env_emscripten_num_logical_cores)(struct w2c_env*);
  void (*w2c_env_emscripten_outn)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_pc_get_function)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_request_pointerlock)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_resize_heap)(struct w2c_env*, u32);
  void (*w2c_env_emscripten_runtime_keepalive_push)(struct w2c_env*);
  u32 (*w2c_env_emscripten_sample_gamepad_data)(struct w2c_env*);
  u32 (*w2c_env_emscripten_set_blur_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_canvas_element_size)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_focus_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_gamepadconnected_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_gamepaddisconnected_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_keydown_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_keyup_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_mousedown_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_mouseenter_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_mouseleave_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_mousemove_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_mouseup_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_pointerlockerror_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_touchcancel_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_touchend_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_touchmove_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_touchstart_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_set_wheel_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_stack_snapshot)(struct w2c_env*);
  u32 (*w2c_env_emscripten_stack_unwind_buffer)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_emscripten_start_fetch)(struct w2c_env*, u32);
  void (*w2c_env_emscripten_unwind_to_js_event_loop)(struct w2c_env*);
  u32 (*w2c_env_emscripten_webgl_create_context)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_webgl_destroy_context)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_webgl_get_context_attributes)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_webgl_make_context_current)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_websocket_close)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_websocket_delete)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_websocket_new)(struct w2c_env*, u32);
  u32 (*w2c_env_emscripten_websocket_send_binary)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_emscripten_websocket_send_utf8_text)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_emscripten_websocket_set_onclose_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_websocket_set_onerror_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_websocket_set_onmessage_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_emscripten_websocket_set_onopen_callback_on_thread)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_exit)(struct w2c_env*, u32);
  void (*w2c_env_fiber_destroy_js)(struct w2c_env*, u32);
  void (*w2c_env_focus_text_input)(struct w2c_env*);
  f64 (*w2c_env_get_effective_device_pixel_ratio)(struct w2c_env*);
  u32 (*w2c_env_get_hardware_concurrency)(struct w2c_env*);
  void (*w2c_env_glActiveTexture)(struct w2c_env*, u32);
  void (*w2c_env_glAttachShader)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBindAttribLocation)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glBindBuffer)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBindBufferBase)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glBindBufferRange)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glBindFramebuffer)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBindRenderbuffer)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBindTexture)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBindVertexArray)(struct w2c_env*, u32);
  void (*w2c_env_glBlendFunc)(struct w2c_env*, u32, u32);
  void (*w2c_env_glBlendFuncSeparate)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glBlitFramebuffer)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glBufferData)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glBufferSubData)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_glCheckFramebufferStatus)(struct w2c_env*, u32);
  void (*w2c_env_glClear)(struct w2c_env*, u32);
  void (*w2c_env_glClearBufferfi)(struct w2c_env*, u32, u32, f32, u32);
  void (*w2c_env_glClearBufferfv)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glClearBufferiv)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glClearColor)(struct w2c_env*, f32, f32, f32, f32);
  void (*w2c_env_glClearDepthf)(struct w2c_env*, f32);
  void (*w2c_env_glClearStencil)(struct w2c_env*, u32);
  u32 (*w2c_env_glClientWaitSync)(struct w2c_env*, u32, u32, u64);
  void (*w2c_env_glClipControlEXT)(struct w2c_env*, u32, u32);
  void (*w2c_env_glColorMask)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glCompileShader)(struct w2c_env*, u32);
  void (*w2c_env_glCompressedTexImage2D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glCompressedTexImage3D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glCompressedTexSubImage2D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glCompressedTexSubImage3D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glCopyTexSubImage2D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32);
  u32 (*w2c_env_glCreateProgram)(struct w2c_env*);
  u32 (*w2c_env_glCreateShader)(struct w2c_env*, u32);
  void (*w2c_env_glCullFace)(struct w2c_env*, u32);
  void (*w2c_env_glDeleteBuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDeleteFramebuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDeleteProgram)(struct w2c_env*, u32);
  void (*w2c_env_glDeleteRenderbuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDeleteShader)(struct w2c_env*, u32);
  void (*w2c_env_glDeleteSync)(struct w2c_env*, u32);
  void (*w2c_env_glDeleteTextures)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDeleteVertexArrays)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDepthFunc)(struct w2c_env*, u32);
  void (*w2c_env_glDepthMask)(struct w2c_env*, u32);
  void (*w2c_env_glDisable)(struct w2c_env*, u32);
  void (*w2c_env_glDisableVertexAttribArray)(struct w2c_env*, u32);
  void (*w2c_env_glDrawArrays)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glDrawArraysInstanced)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glDrawBuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glDrawElements)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glDrawElementsInstanced)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glEnable)(struct w2c_env*, u32);
  void (*w2c_env_glEnableVertexAttribArray)(struct w2c_env*, u32);
  u32 (*w2c_env_glFenceSync)(struct w2c_env*, u32, u32);
  void (*w2c_env_glFramebufferRenderbuffer)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glFramebufferTexture2D)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glFramebufferTextureLayer)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glGenBuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGenFramebuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGenRenderbuffers)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGenTextures)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGenVertexArrays)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGenerateMipmap)(struct w2c_env*, u32);
  void (*w2c_env_glGetActiveUniform)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glGetActiveUniformBlockiv)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_glGetError)(struct w2c_env*);
  void (*w2c_env_glGetIntegerv)(struct w2c_env*, u32, u32);
  void (*w2c_env_glGetProgramInfoLog)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glGetProgramiv)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glGetShaderInfoLog)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glGetShaderiv)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_glGetString)(struct w2c_env*, u32);
  u32 (*w2c_env_glGetUniformBlockIndex)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_glGetUniformLocation)(struct w2c_env*, u32, u32);
  void (*w2c_env_glInvalidateFramebuffer)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glLinkProgram)(struct w2c_env*, u32);
  u32 (*w2c_env_glMapBufferRange)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glPixelStorei)(struct w2c_env*, u32, u32);
  void (*w2c_env_glPolygonOffset)(struct w2c_env*, f32, f32);
  void (*w2c_env_glReadPixels)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glRenderbufferStorage)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glRenderbufferStorageMultisample)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glScissor)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glShaderSource)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_glStencilFunc)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glStencilMask)(struct w2c_env*, u32);
  void (*w2c_env_glStencilOp)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glTexImage2D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glTexImage3D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glTexParameterf)(struct w2c_env*, u32, u32, f32);
  void (*w2c_env_glTexParameterfv)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glTexParameteri)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_glTexStorage2D)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_glTexStorage3D)(struct w2c_env*, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glTexSubImage2D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glTexSubImage3D)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glUniform1i)(struct w2c_env*, u32, u32);
  void (*w2c_env_glUniformBlockBinding)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_glUnmapBuffer)(struct w2c_env*, u32);
  void (*w2c_env_glUseProgram)(struct w2c_env*, u32);
  void (*w2c_env_glVertexAttribPointer)(struct w2c_env*, u32, u32, u32, u32, u32, u32);
  void (*w2c_env_glViewport)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_has_keyboard)(struct w2c_env*);
  u32 (*w2c_env_has_mouse)(struct w2c_env*);
  void (*w2c_env_install_pointer_source_tracking)(struct w2c_env*);
  u32 (*w2c_env_is_mobile_browser)(struct w2c_env*);
  u32 (*w2c_env_is_mouse_sourced_event)(struct w2c_env*);
  u32 (*w2c_env_is_touch_mobile)(struct w2c_env*);
  u32 (*w2c_env_is_touch_screen_device)(struct w2c_env*);
  void (*w2c_env_js_copy_keyboard_layout_key)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_js_external_content_sharing_deliver)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_js_fetch_file)(struct w2c_env*, u32, u32, u32);
  u32 (*w2c_env_js_keyboard_layout_status)(struct w2c_env*);
  u32 (*w2c_env_js_linking_open_url)(struct w2c_env*, u32, u32);
  void (*w2c_env_js_request_keyboard_layout)(struct w2c_env*);
  u32 (*w2c_env_js_web_client_page_visible)(struct w2c_env*);
  void (*w2c_env_preallocate_emscripten_key_event_buffer)(struct w2c_env*, u32);
  void (*w2c_env_rbx_opfs_close)(struct w2c_env*, u32);
  void (*w2c_env_rbx_opfs_free)(struct w2c_env*, u32);
  void (*w2c_env_rbx_opfs_install_helpers)(struct w2c_env*, u32, u32);
  u32 (*w2c_env_rbx_opfs_read_sync_if_cached)(struct w2c_env*, u32, u32, u32, u32);
  void (*w2c_env_rbx_wasm_rna_close_js)(struct w2c_env*, u32);
  void (*w2c_env_rbx_wasm_rna_connect_js)(struct w2c_env*, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
  u32 (*w2c_env_rbx_wasm_rna_is_supported_js)(struct w2c_env*);
  void (*w2c_env_rbx_wasm_rna_open_stream_js)(struct w2c_env*, u32, u32);
  void (*w2c_env_rbx_wasm_rna_pause_stream_read_js)(struct w2c_env*, u32, u32);
  void (*w2c_env_rbx_wasm_rna_receive_ring_consumed_js)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_rbx_wasm_rna_resume_stream_read_js)(struct w2c_env*, u32, u32);
  void (*w2c_env_rbx_wasm_rna_send_connection_close_js)(struct w2c_env*, u32);
  void (*w2c_env_rbx_wasm_rna_send_datagram_js)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_rbx_wasm_rna_shutdown_stream_read_js)(struct w2c_env*, u32, u32);
  void (*w2c_env_rbx_wasm_rna_shutdown_stream_write_js)(struct w2c_env*, u32, u32);
  void (*w2c_env_rbx_wasm_rna_write_stream_js)(struct w2c_env*, u32, u32, u32, u32);
  u32 (*w2c_env_read_host_info_int)(struct w2c_env*, u32);
  void (*w2c_env_read_host_info_string)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_report_crash)(struct w2c_env*, u32, u32, u32, u32, u32);
  void (*w2c_env_reset_current_text_input)(struct w2c_env*);
  void (*w2c_env_schedule_main_loop)(struct w2c_env*);
  void (*w2c_env_set_crash_attribute)(struct w2c_env*, u32, u32);
  void (*w2c_env_set_text_box_horiz_alignment)(struct w2c_env*, u32);
  void (*w2c_env_set_text_box_overlay)(struct w2c_env*, u32);
  void (*w2c_env_set_text_box_vert_alignment)(struct w2c_env*, u32);
  void (*w2c_env_set_text_color)(struct w2c_env*, f32, f32, f32, f32);
  void (*w2c_env_setup_font)(struct w2c_env*, u32, u32, u32, f32, u32);
  void (*w2c_env_shift_canvas)(struct w2c_env*, f32);
  void (*w2c_env_sync_text_and_pos)(struct w2c_env*, u32, u32, u32);
  void (*w2c_env_update_text_box_rect)(struct w2c_env*, f32, f32, f32, f32);
  u32 (*w2c_wasi__snapshot__preview1_clock_time_get)(struct w2c_wasi__snapshot__preview1*, u32, u64, u32);
  u32 (*w2c_wasi__snapshot__preview1_environ_get)(struct w2c_wasi__snapshot__preview1*, u32, u32);
  u32 (*w2c_wasi__snapshot__preview1_environ_sizes_get)(struct w2c_wasi__snapshot__preview1*, u32, u32);
  void (*w2c_wasi__snapshot__preview1_proc_exit)(struct w2c_wasi__snapshot__preview1*, u32);
  u32 (*w2c_wasi__snapshot__preview1_random_get)(struct w2c_wasi__snapshot__preview1*, u32, u32);
};
struct w2c_env g_env;
struct w2c_wasi__snapshot__preview1 { int x; };
struct w2c_wasi__snapshot__preview1 g_wasi;
extern wasm_rt_shared_memory_t g_mem;
extern __thread w2c_WebClient* g_inst;
extern WASM_RT_THREAD_LOCAL uint32_t wasm_rt_call_stack_depth;
int g_fx;
int g_ci_trace;
static void my_init_main_thread_js(struct w2c_env* e, u32 pthread_ptr) {
  (void)e;
  /* mirrors WasmPlayer.js: _emscripten_thread_init(ptr,is_main,1,atomics,256K,0) + threadInitTLS() */
  w2c_WebClient_0x5Femscripten_thread_init(g_inst, pthread_ptr, 1u, 1u, 1u, 262144u, 0u);
  w2c_WebClient_0x5Femscripten_tls_init(g_inst);
}
static void fill_env(void) {
  g_env.w2c_env_HaveOffsetConverter = w2c_env_HaveOffsetConverter;
  g_env.w2c_env_RBX_restoreWasmStackTraceLimit = w2c_env_RBX_restoreWasmStackTraceLimit;
  g_env.w2c_env_RBX_setFullWasmStackTrace = w2c_env_RBX_setFullWasmStackTrace;
  g_env.w2c_env_0x5F_assert_fail = w2c_env_0x5F_assert_fail;
  g_env.w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js = w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js;
  g_env.w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js = w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js;
  g_env.w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi = w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_clear = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_clear;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_contains = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_contains;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_file_size = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_file_size;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer_offset = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer_offset;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_offset = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_offset;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list_sized = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list_sized;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_open = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_open;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove_batch = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove_batch;
  g_env.w2c_env_0x5F_asyncjs_0x5Frbx_opfs_set = w2c_env_0x5F_asyncjs_0x5Frbx_opfs_set;
  g_env.w2c_env_0x5F_call_sighandler = w2c_env_0x5F_call_sighandler;
  g_env.w2c_env_0x5F_pthread_create_js = w2c_env_0x5F_pthread_create_js;
  g_env.w2c_env_0x5F_syscall_getegid32 = w2c_env_0x5F_syscall_getegid32;
  g_env.w2c_env_0x5F_syscall_geteuid32 = w2c_env_0x5F_syscall_geteuid32;
  g_env.w2c_env_0x5F_syscall_getgid32 = w2c_env_0x5F_syscall_getgid32;
  g_env.w2c_env_0x5F_syscall_getuid32 = w2c_env_0x5F_syscall_getuid32;
  g_env.w2c_env_0x5Fabort_js = w2c_env_0x5Fabort_js;
  g_env.w2c_env_0x5Fembind_register_bigint = w2c_env_0x5Fembind_register_bigint;
  g_env.w2c_env_0x5Fembind_register_bool = w2c_env_0x5Fembind_register_bool;
  g_env.w2c_env_0x5Fembind_register_emval = w2c_env_0x5Fembind_register_emval;
  g_env.w2c_env_0x5Fembind_register_float = w2c_env_0x5Fembind_register_float;
  g_env.w2c_env_0x5Fembind_register_function = w2c_env_0x5Fembind_register_function;
  g_env.w2c_env_0x5Fembind_register_integer = w2c_env_0x5Fembind_register_integer;
  g_env.w2c_env_0x5Fembind_register_memory_view = w2c_env_0x5Fembind_register_memory_view;
  g_env.w2c_env_0x5Fembind_register_std_string = w2c_env_0x5Fembind_register_std_string;
  g_env.w2c_env_0x5Fembind_register_std_wstring = w2c_env_0x5Fembind_register_std_wstring;
  g_env.w2c_env_0x5Fembind_register_void = w2c_env_0x5Fembind_register_void;
  g_env.w2c_env_0x5Femscripten_fetch_get_response_headers = w2c_env_0x5Femscripten_fetch_get_response_headers;
  g_env.w2c_env_0x5Femscripten_fetch_get_response_headers_length = w2c_env_0x5Femscripten_fetch_get_response_headers_length;
  g_env.w2c_env_0x5Femscripten_init_main_thread_js = my_init_main_thread_js;
  g_env.w2c_env_0x5Femscripten_log_formatted = w2c_env_0x5Femscripten_log_formatted;
  g_env.w2c_env_0x5Femscripten_notify_mailbox_postmessage = w2c_env_0x5Femscripten_notify_mailbox_postmessage;
  g_env.w2c_env_0x5Femscripten_receive_on_main_thread_js = w2c_env_0x5Femscripten_receive_on_main_thread_js;
  g_env.w2c_env_0x5Femscripten_runtime_keepalive_clear = w2c_env_0x5Femscripten_runtime_keepalive_clear;
  g_env.w2c_env_0x5Femscripten_thread_cleanup = w2c_env_0x5Femscripten_thread_cleanup;
  g_env.w2c_env_0x5Femscripten_thread_mailbox_await = w2c_env_0x5Femscripten_thread_mailbox_await;
  g_env.w2c_env_0x5Femscripten_thread_set_strongref = w2c_env_0x5Femscripten_thread_set_strongref;
  g_env.w2c_env_0x5Fgmtime_js = w2c_env_0x5Fgmtime_js;
  g_env.w2c_env_0x5Flocaltime_js = w2c_env_0x5Flocaltime_js;
  g_env.w2c_env_0x5Fmktime_js = w2c_env_0x5Fmktime_js;
  g_env.w2c_env_0x5Fmsync_js = w2c_env_0x5Fmsync_js;
  g_env.w2c_env_0x5Ftzset_js = w2c_env_0x5Ftzset_js;
  g_env.w2c_env_applyPendingCanvasResize = w2c_env_applyPendingCanvasResize;
  g_env.w2c_env_blur_text_input = w2c_env_blur_text_input;
  g_env.w2c_env_choose_current_text_input = w2c_env_choose_current_text_input;
  g_env.w2c_env_copy_pending_text = w2c_env_copy_pending_text;
  g_env.w2c_env_emscripten_asm_const_async_on_main_thread = w2c_env_emscripten_asm_const_async_on_main_thread;
  g_env.w2c_env_emscripten_asm_const_double = w2c_env_emscripten_asm_const_double;
  g_env.w2c_env_emscripten_asm_const_int = w2c_env_emscripten_asm_const_int;
  g_env.w2c_env_emscripten_asm_const_int_sync_on_main_thread = w2c_env_emscripten_asm_const_int_sync_on_main_thread;
  g_env.w2c_env_emscripten_async_call = w2c_env_emscripten_async_call;
  g_env.w2c_env_emscripten_check_blocking_allowed = w2c_env_emscripten_check_blocking_allowed;
  g_env.w2c_env_emscripten_console_error = w2c_env_emscripten_console_error;
  g_env.w2c_env_emscripten_console_log = w2c_env_emscripten_console_log;
  g_env.w2c_env_emscripten_console_warn = w2c_env_emscripten_console_warn;
  g_env.w2c_env_emscripten_date_now = w2c_env_emscripten_date_now;
  g_env.w2c_env_emscripten_errn = w2c_env_emscripten_errn;
  g_env.w2c_env_emscripten_exit_pointerlock = w2c_env_emscripten_exit_pointerlock;
  g_env.w2c_env_emscripten_exit_with_live_runtime = w2c_env_emscripten_exit_with_live_runtime;
  g_env.w2c_env_emscripten_fetch_free = w2c_env_emscripten_fetch_free;
  g_env.w2c_env_emscripten_get_canvas_element_size = w2c_env_emscripten_get_canvas_element_size;
  g_env.w2c_env_emscripten_get_device_pixel_ratio = w2c_env_emscripten_get_device_pixel_ratio;
  g_env.w2c_env_emscripten_get_element_css_size = w2c_env_emscripten_get_element_css_size;
  g_env.w2c_env_emscripten_get_gamepad_status = w2c_env_emscripten_get_gamepad_status;
  g_env.w2c_env_emscripten_get_heap_max = w2c_env_emscripten_get_heap_max;
  g_env.w2c_env_emscripten_get_now = w2c_env_emscripten_get_now;
  g_env.w2c_env_emscripten_get_num_gamepads = w2c_env_emscripten_get_num_gamepads;
  g_env.w2c_env_emscripten_num_logical_cores = w2c_env_emscripten_num_logical_cores;
  g_env.w2c_env_emscripten_outn = w2c_env_emscripten_outn;
  g_env.w2c_env_emscripten_pc_get_function = w2c_env_emscripten_pc_get_function;
  g_env.w2c_env_emscripten_request_pointerlock = w2c_env_emscripten_request_pointerlock;
  g_env.w2c_env_emscripten_resize_heap = w2c_env_emscripten_resize_heap;
  g_env.w2c_env_emscripten_runtime_keepalive_push = w2c_env_emscripten_runtime_keepalive_push;
  g_env.w2c_env_emscripten_sample_gamepad_data = w2c_env_emscripten_sample_gamepad_data;
  g_env.w2c_env_emscripten_set_blur_callback_on_thread = w2c_env_emscripten_set_blur_callback_on_thread;
  g_env.w2c_env_emscripten_set_canvas_element_size = w2c_env_emscripten_set_canvas_element_size;
  g_env.w2c_env_emscripten_set_focus_callback_on_thread = w2c_env_emscripten_set_focus_callback_on_thread;
  g_env.w2c_env_emscripten_set_gamepadconnected_callback_on_thread = w2c_env_emscripten_set_gamepadconnected_callback_on_thread;
  g_env.w2c_env_emscripten_set_gamepaddisconnected_callback_on_thread = w2c_env_emscripten_set_gamepaddisconnected_callback_on_thread;
  g_env.w2c_env_emscripten_set_keydown_callback_on_thread = w2c_env_emscripten_set_keydown_callback_on_thread;
  g_env.w2c_env_emscripten_set_keyup_callback_on_thread = w2c_env_emscripten_set_keyup_callback_on_thread;
  g_env.w2c_env_emscripten_set_mousedown_callback_on_thread = w2c_env_emscripten_set_mousedown_callback_on_thread;
  g_env.w2c_env_emscripten_set_mouseenter_callback_on_thread = w2c_env_emscripten_set_mouseenter_callback_on_thread;
  g_env.w2c_env_emscripten_set_mouseleave_callback_on_thread = w2c_env_emscripten_set_mouseleave_callback_on_thread;
  g_env.w2c_env_emscripten_set_mousemove_callback_on_thread = w2c_env_emscripten_set_mousemove_callback_on_thread;
  g_env.w2c_env_emscripten_set_mouseup_callback_on_thread = w2c_env_emscripten_set_mouseup_callback_on_thread;
  g_env.w2c_env_emscripten_set_pointerlockerror_callback_on_thread = w2c_env_emscripten_set_pointerlockerror_callback_on_thread;
  g_env.w2c_env_emscripten_set_touchcancel_callback_on_thread = w2c_env_emscripten_set_touchcancel_callback_on_thread;
  g_env.w2c_env_emscripten_set_touchend_callback_on_thread = w2c_env_emscripten_set_touchend_callback_on_thread;
  g_env.w2c_env_emscripten_set_touchmove_callback_on_thread = w2c_env_emscripten_set_touchmove_callback_on_thread;
  g_env.w2c_env_emscripten_set_touchstart_callback_on_thread = w2c_env_emscripten_set_touchstart_callback_on_thread;
  g_env.w2c_env_emscripten_set_wheel_callback_on_thread = w2c_env_emscripten_set_wheel_callback_on_thread;
  g_env.w2c_env_emscripten_stack_snapshot = w2c_env_emscripten_stack_snapshot;
  g_env.w2c_env_emscripten_stack_unwind_buffer = w2c_env_emscripten_stack_unwind_buffer;
  g_env.w2c_env_emscripten_start_fetch = w2c_env_emscripten_start_fetch;
  g_env.w2c_env_emscripten_unwind_to_js_event_loop = w2c_env_emscripten_unwind_to_js_event_loop;
  g_env.w2c_env_emscripten_webgl_create_context = w2c_env_emscripten_webgl_create_context;
  g_env.w2c_env_emscripten_webgl_destroy_context = w2c_env_emscripten_webgl_destroy_context;
  g_env.w2c_env_emscripten_webgl_get_context_attributes = w2c_env_emscripten_webgl_get_context_attributes;
  g_env.w2c_env_emscripten_webgl_make_context_current = w2c_env_emscripten_webgl_make_context_current;
  g_env.w2c_env_emscripten_websocket_close = w2c_env_emscripten_websocket_close;
  g_env.w2c_env_emscripten_websocket_delete = w2c_env_emscripten_websocket_delete;
  g_env.w2c_env_emscripten_websocket_new = w2c_env_emscripten_websocket_new;
  g_env.w2c_env_emscripten_websocket_send_binary = w2c_env_emscripten_websocket_send_binary;
  g_env.w2c_env_emscripten_websocket_send_utf8_text = w2c_env_emscripten_websocket_send_utf8_text;
  g_env.w2c_env_emscripten_websocket_set_onclose_callback_on_thread = w2c_env_emscripten_websocket_set_onclose_callback_on_thread;
  g_env.w2c_env_emscripten_websocket_set_onerror_callback_on_thread = w2c_env_emscripten_websocket_set_onerror_callback_on_thread;
  g_env.w2c_env_emscripten_websocket_set_onmessage_callback_on_thread = w2c_env_emscripten_websocket_set_onmessage_callback_on_thread;
  g_env.w2c_env_emscripten_websocket_set_onopen_callback_on_thread = w2c_env_emscripten_websocket_set_onopen_callback_on_thread;
  g_env.w2c_env_exit = w2c_env_exit;
  g_env.w2c_env_fiber_destroy_js = w2c_env_fiber_destroy_js;
  g_env.w2c_env_focus_text_input = w2c_env_focus_text_input;
  g_env.w2c_env_get_effective_device_pixel_ratio = w2c_env_get_effective_device_pixel_ratio;
  g_env.w2c_env_get_hardware_concurrency = w2c_env_get_hardware_concurrency;
  g_env.w2c_env_glActiveTexture = w2c_env_glActiveTexture;
  g_env.w2c_env_glAttachShader = w2c_env_glAttachShader;
  g_env.w2c_env_glBindAttribLocation = w2c_env_glBindAttribLocation;
  g_env.w2c_env_glBindBuffer = w2c_env_glBindBuffer;
  g_env.w2c_env_glBindBufferBase = w2c_env_glBindBufferBase;
  g_env.w2c_env_glBindBufferRange = w2c_env_glBindBufferRange;
  g_env.w2c_env_glBindFramebuffer = w2c_env_glBindFramebuffer;
  g_env.w2c_env_glBindRenderbuffer = w2c_env_glBindRenderbuffer;
  g_env.w2c_env_glBindTexture = w2c_env_glBindTexture;
  g_env.w2c_env_glBindVertexArray = w2c_env_glBindVertexArray;
  g_env.w2c_env_glBlendFunc = w2c_env_glBlendFunc;
  g_env.w2c_env_glBlendFuncSeparate = w2c_env_glBlendFuncSeparate;
  g_env.w2c_env_glBlitFramebuffer = w2c_env_glBlitFramebuffer;
  g_env.w2c_env_glBufferData = w2c_env_glBufferData;
  g_env.w2c_env_glBufferSubData = w2c_env_glBufferSubData;
  g_env.w2c_env_glCheckFramebufferStatus = w2c_env_glCheckFramebufferStatus;
  g_env.w2c_env_glClear = w2c_env_glClear;
  g_env.w2c_env_glClearBufferfi = w2c_env_glClearBufferfi;
  g_env.w2c_env_glClearBufferfv = w2c_env_glClearBufferfv;
  g_env.w2c_env_glClearBufferiv = w2c_env_glClearBufferiv;
  g_env.w2c_env_glClearColor = w2c_env_glClearColor;
  g_env.w2c_env_glClearDepthf = w2c_env_glClearDepthf;
  g_env.w2c_env_glClearStencil = w2c_env_glClearStencil;
  g_env.w2c_env_glClientWaitSync = w2c_env_glClientWaitSync;
  g_env.w2c_env_glClipControlEXT = w2c_env_glClipControlEXT;
  g_env.w2c_env_glColorMask = w2c_env_glColorMask;
  g_env.w2c_env_glCompileShader = w2c_env_glCompileShader;
  g_env.w2c_env_glCompressedTexImage2D = w2c_env_glCompressedTexImage2D;
  g_env.w2c_env_glCompressedTexImage3D = w2c_env_glCompressedTexImage3D;
  g_env.w2c_env_glCompressedTexSubImage2D = w2c_env_glCompressedTexSubImage2D;
  g_env.w2c_env_glCompressedTexSubImage3D = w2c_env_glCompressedTexSubImage3D;
  g_env.w2c_env_glCopyTexSubImage2D = w2c_env_glCopyTexSubImage2D;
  g_env.w2c_env_glCreateProgram = w2c_env_glCreateProgram;
  g_env.w2c_env_glCreateShader = w2c_env_glCreateShader;
  g_env.w2c_env_glCullFace = w2c_env_glCullFace;
  g_env.w2c_env_glDeleteBuffers = w2c_env_glDeleteBuffers;
  g_env.w2c_env_glDeleteFramebuffers = w2c_env_glDeleteFramebuffers;
  g_env.w2c_env_glDeleteProgram = w2c_env_glDeleteProgram;
  g_env.w2c_env_glDeleteRenderbuffers = w2c_env_glDeleteRenderbuffers;
  g_env.w2c_env_glDeleteShader = w2c_env_glDeleteShader;
  g_env.w2c_env_glDeleteSync = w2c_env_glDeleteSync;
  g_env.w2c_env_glDeleteTextures = w2c_env_glDeleteTextures;
  g_env.w2c_env_glDeleteVertexArrays = w2c_env_glDeleteVertexArrays;
  g_env.w2c_env_glDepthFunc = w2c_env_glDepthFunc;
  g_env.w2c_env_glDepthMask = w2c_env_glDepthMask;
  g_env.w2c_env_glDisable = w2c_env_glDisable;
  g_env.w2c_env_glDisableVertexAttribArray = w2c_env_glDisableVertexAttribArray;
  g_env.w2c_env_glDrawArrays = w2c_env_glDrawArrays;
  g_env.w2c_env_glDrawArraysInstanced = w2c_env_glDrawArraysInstanced;
  g_env.w2c_env_glDrawBuffers = w2c_env_glDrawBuffers;
  g_env.w2c_env_glDrawElements = w2c_env_glDrawElements;
  g_env.w2c_env_glDrawElementsInstanced = w2c_env_glDrawElementsInstanced;
  g_env.w2c_env_glEnable = w2c_env_glEnable;
  g_env.w2c_env_glEnableVertexAttribArray = w2c_env_glEnableVertexAttribArray;
  g_env.w2c_env_glFenceSync = w2c_env_glFenceSync;
  g_env.w2c_env_glFramebufferRenderbuffer = w2c_env_glFramebufferRenderbuffer;
  g_env.w2c_env_glFramebufferTexture2D = w2c_env_glFramebufferTexture2D;
  g_env.w2c_env_glFramebufferTextureLayer = w2c_env_glFramebufferTextureLayer;
  g_env.w2c_env_glGenBuffers = w2c_env_glGenBuffers;
  g_env.w2c_env_glGenFramebuffers = w2c_env_glGenFramebuffers;
  g_env.w2c_env_glGenRenderbuffers = w2c_env_glGenRenderbuffers;
  g_env.w2c_env_glGenTextures = w2c_env_glGenTextures;
  g_env.w2c_env_glGenVertexArrays = w2c_env_glGenVertexArrays;
  g_env.w2c_env_glGenerateMipmap = w2c_env_glGenerateMipmap;
  g_env.w2c_env_glGetActiveUniform = w2c_env_glGetActiveUniform;
  g_env.w2c_env_glGetActiveUniformBlockiv = w2c_env_glGetActiveUniformBlockiv;
  g_env.w2c_env_glGetError = w2c_env_glGetError;
  g_env.w2c_env_glGetIntegerv = w2c_env_glGetIntegerv;
  g_env.w2c_env_glGetProgramInfoLog = w2c_env_glGetProgramInfoLog;
  g_env.w2c_env_glGetProgramiv = w2c_env_glGetProgramiv;
  g_env.w2c_env_glGetShaderInfoLog = w2c_env_glGetShaderInfoLog;
  g_env.w2c_env_glGetShaderiv = w2c_env_glGetShaderiv;
  g_env.w2c_env_glGetString = w2c_env_glGetString;
  g_env.w2c_env_glGetUniformBlockIndex = w2c_env_glGetUniformBlockIndex;
  g_env.w2c_env_glGetUniformLocation = w2c_env_glGetUniformLocation;
  g_env.w2c_env_glInvalidateFramebuffer = w2c_env_glInvalidateFramebuffer;
  g_env.w2c_env_glLinkProgram = w2c_env_glLinkProgram;
  g_env.w2c_env_glMapBufferRange = w2c_env_glMapBufferRange;
  g_env.w2c_env_glPixelStorei = w2c_env_glPixelStorei;
  g_env.w2c_env_glPolygonOffset = w2c_env_glPolygonOffset;
  g_env.w2c_env_glReadPixels = w2c_env_glReadPixels;
  g_env.w2c_env_glRenderbufferStorage = w2c_env_glRenderbufferStorage;
  g_env.w2c_env_glRenderbufferStorageMultisample = w2c_env_glRenderbufferStorageMultisample;
  g_env.w2c_env_glScissor = w2c_env_glScissor;
  g_env.w2c_env_glShaderSource = w2c_env_glShaderSource;
  g_env.w2c_env_glStencilFunc = w2c_env_glStencilFunc;
  g_env.w2c_env_glStencilMask = w2c_env_glStencilMask;
  g_env.w2c_env_glStencilOp = w2c_env_glStencilOp;
  g_env.w2c_env_glTexImage2D = w2c_env_glTexImage2D;
  g_env.w2c_env_glTexImage3D = w2c_env_glTexImage3D;
  g_env.w2c_env_glTexParameterf = w2c_env_glTexParameterf;
  g_env.w2c_env_glTexParameterfv = w2c_env_glTexParameterfv;
  g_env.w2c_env_glTexParameteri = w2c_env_glTexParameteri;
  g_env.w2c_env_glTexStorage2D = w2c_env_glTexStorage2D;
  g_env.w2c_env_glTexStorage3D = w2c_env_glTexStorage3D;
  g_env.w2c_env_glTexSubImage2D = w2c_env_glTexSubImage2D;
  g_env.w2c_env_glTexSubImage3D = w2c_env_glTexSubImage3D;
  g_env.w2c_env_glUniform1i = w2c_env_glUniform1i;
  g_env.w2c_env_glUniformBlockBinding = w2c_env_glUniformBlockBinding;
  g_env.w2c_env_glUnmapBuffer = w2c_env_glUnmapBuffer;
  g_env.w2c_env_glUseProgram = w2c_env_glUseProgram;
  g_env.w2c_env_glVertexAttribPointer = w2c_env_glVertexAttribPointer;
  g_env.w2c_env_glViewport = w2c_env_glViewport;
  g_env.w2c_env_has_keyboard = w2c_env_has_keyboard;
  g_env.w2c_env_has_mouse = w2c_env_has_mouse;
  g_env.w2c_env_install_pointer_source_tracking = w2c_env_install_pointer_source_tracking;
  g_env.w2c_env_is_mobile_browser = w2c_env_is_mobile_browser;
  g_env.w2c_env_is_mouse_sourced_event = w2c_env_is_mouse_sourced_event;
  g_env.w2c_env_is_touch_mobile = w2c_env_is_touch_mobile;
  g_env.w2c_env_is_touch_screen_device = w2c_env_is_touch_screen_device;
  g_env.w2c_env_js_copy_keyboard_layout_key = w2c_env_js_copy_keyboard_layout_key;
  g_env.w2c_env_js_external_content_sharing_deliver = w2c_env_js_external_content_sharing_deliver;
  g_env.w2c_env_js_fetch_file = w2c_env_js_fetch_file;
  g_env.w2c_env_js_keyboard_layout_status = w2c_env_js_keyboard_layout_status;
  g_env.w2c_env_js_linking_open_url = w2c_env_js_linking_open_url;
  g_env.w2c_env_js_request_keyboard_layout = w2c_env_js_request_keyboard_layout;
  g_env.w2c_env_js_web_client_page_visible = w2c_env_js_web_client_page_visible;
  g_env.w2c_env_preallocate_emscripten_key_event_buffer = w2c_env_preallocate_emscripten_key_event_buffer;
  g_env.w2c_env_rbx_opfs_close = w2c_env_rbx_opfs_close;
  g_env.w2c_env_rbx_opfs_free = w2c_env_rbx_opfs_free;
  g_env.w2c_env_rbx_opfs_install_helpers = w2c_env_rbx_opfs_install_helpers;
  g_env.w2c_env_rbx_opfs_read_sync_if_cached = w2c_env_rbx_opfs_read_sync_if_cached;
  g_env.w2c_env_rbx_wasm_rna_close_js = w2c_env_rbx_wasm_rna_close_js;
  g_env.w2c_env_rbx_wasm_rna_connect_js = w2c_env_rbx_wasm_rna_connect_js;
  g_env.w2c_env_rbx_wasm_rna_is_supported_js = w2c_env_rbx_wasm_rna_is_supported_js;
  g_env.w2c_env_rbx_wasm_rna_open_stream_js = w2c_env_rbx_wasm_rna_open_stream_js;
  g_env.w2c_env_rbx_wasm_rna_pause_stream_read_js = w2c_env_rbx_wasm_rna_pause_stream_read_js;
  g_env.w2c_env_rbx_wasm_rna_receive_ring_consumed_js = w2c_env_rbx_wasm_rna_receive_ring_consumed_js;
  g_env.w2c_env_rbx_wasm_rna_resume_stream_read_js = w2c_env_rbx_wasm_rna_resume_stream_read_js;
  g_env.w2c_env_rbx_wasm_rna_send_connection_close_js = w2c_env_rbx_wasm_rna_send_connection_close_js;
  g_env.w2c_env_rbx_wasm_rna_send_datagram_js = w2c_env_rbx_wasm_rna_send_datagram_js;
  g_env.w2c_env_rbx_wasm_rna_shutdown_stream_read_js = w2c_env_rbx_wasm_rna_shutdown_stream_read_js;
  g_env.w2c_env_rbx_wasm_rna_shutdown_stream_write_js = w2c_env_rbx_wasm_rna_shutdown_stream_write_js;
  g_env.w2c_env_rbx_wasm_rna_write_stream_js = w2c_env_rbx_wasm_rna_write_stream_js;
  g_env.w2c_env_read_host_info_int = w2c_env_read_host_info_int;
  g_env.w2c_env_read_host_info_string = w2c_env_read_host_info_string;
  g_env.w2c_env_report_crash = w2c_env_report_crash;
  g_env.w2c_env_reset_current_text_input = w2c_env_reset_current_text_input;
  g_env.w2c_env_schedule_main_loop = w2c_env_schedule_main_loop;
  g_env.w2c_env_set_crash_attribute = w2c_env_set_crash_attribute;
  g_env.w2c_env_set_text_box_horiz_alignment = w2c_env_set_text_box_horiz_alignment;
  g_env.w2c_env_set_text_box_overlay = w2c_env_set_text_box_overlay;
  g_env.w2c_env_set_text_box_vert_alignment = w2c_env_set_text_box_vert_alignment;
  g_env.w2c_env_set_text_color = w2c_env_set_text_color;
  g_env.w2c_env_setup_font = w2c_env_setup_font;
  g_env.w2c_env_shift_canvas = w2c_env_shift_canvas;
  g_env.w2c_env_sync_text_and_pos = w2c_env_sync_text_and_pos;
  g_env.w2c_env_update_text_box_rect = w2c_env_update_text_box_rect;
  g_env.w2c_wasi__snapshot__preview1_clock_time_get = w2c_wasi__snapshot__preview1_clock_time_get;
  g_env.w2c_wasi__snapshot__preview1_environ_get = w2c_wasi__snapshot__preview1_environ_get;
  g_env.w2c_wasi__snapshot__preview1_environ_sizes_get = w2c_wasi__snapshot__preview1_environ_sizes_get;
  g_env.w2c_wasi__snapshot__preview1_proc_exit = w2c_wasi__snapshot__preview1_proc_exit;
  g_env.w2c_wasi__snapshot__preview1_random_get = w2c_wasi__snapshot__preview1_random_get;
}


extern u32 w2c_WebClient_f151423(w2c_WebClient*, u32);
extern u32 w2c_WebClient_f151875(w2c_WebClient*, u32, u32);
extern u32 w2c_WebClient_f151424(w2c_WebClient*, u32, u32, u32);
extern u32 w2c_WebClient_f151425(w2c_WebClient*, u32, u32);

extern wasm_rt_shared_memory_t g_mem;

extern void* c5[];
extern void* c6[];
extern void* c7[];
static void* cands[] = {(void*)w2c_WebClient_f150029,(void*)w2c_WebClient_f150079,(void*)w2c_WebClient_f150558,(void*)w2c_WebClient_f150603,(void*)w2c_WebClient_f150604,(void*)w2c_WebClient_f150653,(void*)w2c_WebClient_f150677,(void*)w2c_WebClient_f150692,(void*)w2c_WebClient_f150694,(void*)w2c_WebClient_f150697,(void*)w2c_WebClient_f150698,(void*)w2c_WebClient_f150702,(void*)w2c_WebClient_f150707,(void*)w2c_WebClient_f150708,(void*)w2c_WebClient_f150709,(void*)w2c_WebClient_f150710,(void*)w2c_WebClient_f150711,(void*)w2c_WebClient_f150716,(void*)w2c_WebClient_f150717,(void*)w2c_WebClient_f150718,(void*)w2c_WebClient_f150719,(void*)w2c_WebClient_f150721,(void*)w2c_WebClient_f150722,(void*)w2c_WebClient_f150723,(void*)w2c_WebClient_f150725,(void*)w2c_WebClient_f150734,(void*)w2c_WebClient_f150736,(void*)w2c_WebClient_f150738,(void*)w2c_WebClient_f150739,(void*)w2c_WebClient_f150740,(void*)w2c_WebClient_f150742,(void*)w2c_WebClient_f150743,(void*)w2c_WebClient_f150744,(void*)w2c_WebClient_f150753,(void*)w2c_WebClient_f150754,(void*)w2c_WebClient_f150755,(void*)w2c_WebClient_f150757,(void*)w2c_WebClient_f150761,(void*)w2c_WebClient_f150763,(void*)w2c_WebClient_f150790,(void*)w2c_WebClient_f133758,};

static void* c4[] = {(void*)w2c_WebClient_f455,(void*)w2c_WebClient_f156211,(void*)w2c_WebClient_f159798,(void*)w2c_WebClient_f139564,(void*)w2c_WebClient_f139563,(void*)w2c_WebClient_f161847,(void*)w2c_WebClient_f124897,(void*)w2c_WebClient_f124852,};

extern u32 w2c_WebClient_malloc(w2c_WebClient*, u32);
extern u32 w2c_WebClient_realloc(w2c_WebClient*, u32, u32);
extern void w2c_WebClient_free(w2c_WebClient*, u32);
size_t g_last = 0;
static void* my_alloc(void* inst, void* ud, void* p, size_t osz, size_t nsz) {
  (void)ud; (void)osz;
  if (!nsz) { w2c_WebClient_free((w2c_WebClient*)inst, (u32)(uintptr_t)p); return 0; }
  void* r = (void*)(uintptr_t)w2c_WebClient_realloc((w2c_WebClient*)inst, (u32)(uintptr_t)p, (u32)nsz);
  if (nsz == 15520) g_last = (size_t)(uintptr_t)r;
  return r;
}
static u32 mkstate(void) {
  u64 slot = g_inst->w2c_0x5F_indirect_function_table.size - 2;
  wasm_rt_funcref_t* t = g_inst->w2c_0x5F_indirect_function_table.data;
  for (u64 i = 0; i + 1 < g_inst->w2c_0x5F_indirect_function_table.size; i++) {
    if (t[i].func_type && memcmp(t[i].func_type, w2c_WebClient_t9, 32) == 0) { t[slot] = t[i]; break; }
  }
  t[slot].func = (wasm_rt_function_ptr_t)my_alloc;
  t[slot].module_instance = g_inst;
  return w2c_WebClient_f151875(g_inst, (u32)slot, 0u);
}
extern int wc_root_spawn(void (*)(void*), void*);
extern void* wc_root_spawn_slot(void (*)(void*), void*);
extern int wc_root_slot_done(void*);
extern void wc_root_cleanup_slot(void*);
extern void wc_futex_pump(void);
extern void wc_root_mode(void);
static u32 g_main_rc;
static int g_main_st;
static void run_main(void* p) {
  w2c_WebClient* in = (w2c_WebClient*)p;
  const char* a0 = "/content/.keep";
  u32 s = w2c_WebClient_malloc(in, 32), arr = w2c_WebClient_malloc(in, 8);
  memcpy(wc_ptr(s), a0, 16);
  *(u32*)wc_ptr(arr) = s;
  *(u32*)wc_ptr(arr + 4) = 0;
  g_main_rc = w2c_WebClient_0x5F_main_argc_argv(in, 1u, arr);
}

/* the root fibre body for main(): records how it left */
/* The glue wraps every scheduled turn in try/catch
 * (`port1.onmessage = async () => { try { await ... } catch (e) { console.error } }`),
 * so a wasm exception escaping a task is logged and the loop keeps going
 * instead of aborting. */
static void guarded(void (*fn)(void*), void* arg) {
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET t;
  if (!wasm_rt_try(t)) {
    wasm_rt_set_unwind_target(&t);
    fn(arg);
    wasm_rt_set_unwind_target(outer);
  } else {
    wasm_rt_set_unwind_target(outer);
    extern void* wc_unwind_tag(void);
    if (wasm_rt_exception_tag() != wc_unwind_tag()) {
      size_t n = wasm_rt_exception_size();
      if (n && n < 512) fprintf(stderr, "[iter] threw: %.*s\n", (int)n,
                                (const char*)wasm_rt_exception());
      else fprintf(stderr, "[iter] threw (%zu bytes)\n", n);
    }
  }
}

static void root_main(void* p) { g_main_st = wc_protected(run_main, p); }

extern int wc_pump_deferred(void);
static void tick(void* p);
static void tick(void* p) {
  (void)p;
  wc_pump_deferred();
  guarded((void (*)(void*))w2c_WebClient_main_loop_iteration, g_inst);
}
int main(int argc, char** argv) {
  setbuf(stdout, NULL);
  { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sa.sa_sigaction = abrt; sigaction(SIGABRT, &sa, NULL); }
  g_fx = getenv("WC_FX") != NULL;
  g_ci_trace = getenv("WC_VT") != NULL;
  wc_host_init();
  { extern __thread int g_is_main; g_is_main = 1; }
  wasm_rt_init();
  /* the runtime installs its own SEGV handler (which turns every fault into a
   * trap and loses the address); take it back for the time being */
  { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); }
  /* Chrome allocates the module's maximum (4 GiB) up front - `ni()` in the
   * glue: `INITIAL_MEMORY || 4294967296`.  The module never emits memory.grow,
   * so anything above the allocation is simply out of bounds (a 2 GiB heap
   * made stores land at exactly 0x8000001c). */
  g_mem.data = pl_mem_alloc((size_t)65536 * 65536);
  g_mem.page_size = 65536; g_mem.pages = 65536; g_mem.max_pages = 65536;
  g_mem.size = (uint64_t)65536 * 65536; g_mem.is64 = false;
  fill_env();
  w2c_WebClient inst; memset(&inst, 0, sizeof inst);
  g_inst = &inst;
  wasm2c_WebClient_instantiate(&inst, &g_env, &g_wasi);
  {
    wasm_rt_funcref_t fill = inst.w2c_0x5F_indirect_function_table.data[1];
    for (u64 i = 0; i < inst.w2c_0x5F_indirect_function_table.size; i++)
      if (!inst.w2c_0x5F_indirect_function_table.data[i].func) inst.w2c_0x5F_indirect_function_table.data[i] = fill;
  }
  w2c_WebClient_0x5F_wasm_call_ctors(&inst);
  printf("tbl0=%p tsz=%llu\n", (void*)inst.w2c_0x5F_indirect_function_table.data[0].func, (unsigned long long)inst.w2c_0x5F_indirect_function_table.size);
  int k = argc > 1 ? atoi(argv[1]) : -1;
  if (k == 900) {
    extern volatile int g_wc_sched;
    extern int wc_pump_once(void);
    int wc_pump_deferred(void);
    printf("main(argc=0)...\n");
    extern void wc_prepare(void);
    wc_prepare();
    if (getenv("WC_DUMP_BIND")) { extern void wc_embind_dump(void); wc_embind_dump(); }
    /* main() runs as the root fibre: an import that has to wait parks it and
     * the loop keeps turning, the way JSPI unwinds to the event loop. */

    int st = 0;
    {
      /* main() may park; the page's turn loop keeps running while it does, and
       * an import suspends instead of blocking the thread. */
      wc_root_mode();
      void* main_root = wc_root_spawn_slot(root_main, &inst);
      void* tick_root = NULL;
      double dl = pl_now_ms() + 600000.0;
      while (main_root && !wc_root_slot_done(main_root) && pl_now_ms() < dl) {
        wc_futex_pump();
        if (wc_root_slot_done(main_root)) break;
        /* one turn at a time, like `port1.onmessage` awaiting the iteration */
        if (!tick_root || wc_root_slot_done(tick_root)) {
          if (tick_root) wc_root_cleanup_slot(tick_root);
          tick_root = wc_root_spawn_slot(tick, NULL);
        }
        pl_sleep_ms(1);
      }
      if (tick_root) wc_root_cleanup_slot(tick_root);
      wc_root_cleanup_slot(main_root);
      st = g_main_st;
    }
    if (st == 1) { printf("main threw\n"); return 0; }
    if (st == 2) printf("main exited to live runtime\n");
    else printf("main returned %u\n", g_main_rc);
    const char* ms = getenv("WC_LOOP_MS");
    double t0 = pl_now_ms();
    double deadline = t0 + (ms ? atof(ms) : 120000.0);
    /* Chrome's rAF keeps calling main_loop_iteration for the life of the tab,
     * whether or not the engine currently has work queued - the join and other
     * tasks are driven from it.  Never exit early. */
    {
      void* tr = NULL;
      for (long it = 0; it < 50000000L && pl_now_ms() < deadline; it++) {
        wc_futex_pump();
        if (!tr || wc_root_slot_done(tr)) {
          if (tr) wc_root_cleanup_slot(tr);
          tr = wc_root_spawn_slot(tick, NULL);
        }
        pl_sleep_ms(2);
        if ((it & 0xFFF) == 0) printf("iter %ld t=%.0f\n", it, pl_now_ms() - t0);
      }
      if (tr) wc_root_cleanup_slot(tr);
    }
    printf("loop done\n");
    return 0;
  }
  if (k >= 600) {
    u32 L = mkstate();
    u32 A = 0x4000000, B = 0x4000100, NAMEP = 0x4000200;
    static const char* s0 = "return 1+1"; u32 n0 = (u32)strlen(s0);
    memcpy(g_mem.data + A, s0, n0 + 1);
    memcpy(g_mem.data + B, "!@", 3);
    memcpy(g_mem.data + NAMEP, "=chunk", 7);
    int seven = (k >= 700);
    u32 c = (u32)(k - (seven ? 700 : 600));
    void** tab = seven ? c7 : c6;
    u32 before[8]; memcpy(before, g_mem.data+L, 32);
    u32 r1;
    if (seven) r1 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32,u32,u32))tab[c])(&inst, L, NAMEP, A, n0, 0u, 0u, 0u);
    else       r1 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32,u32))tab[c])(&inst, L, NAMEP, A, n0, 0u, 0u);
    u32 after[8]; memcpy(after, g_mem.data+L, 32);
    int ch = memcmp(before, after, 32) != 0;
    u32 r2;
    if (seven) r2 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32,u32,u32))tab[c])(&inst, L, NAMEP, B, 2u, 0u, 0u, 0u);
    else       r2 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32,u32))tab[c])(&inst, L, NAMEP, B, 2u, 0u, 0u);
    printf("e%d r1=%d r2=%d chg=%d\n", c, (int)r1, (int)r2, ch);
    return 0;
  }
  if (k == 500) {
    u32 L = mkstate();
    u32 A = 0x4000000, N = 0x4000200;
    static const char* s0 = "return 1+1"; u32 n0 = (u32)strlen(s0);
    memcpy(g_mem.data + A, s0, n0 + 1);
    memcpy(g_mem.data + N, "=chunk", 7);
    u32 before[64], after[64];
    memcpy(before, g_mem.data+L, 256);
    u32 r = w2c_WebClient_f160824(&inst, L, N, A, n0, 0u); u32 r2 = w2c_WebClient_f160824(&inst, L, N, (u32)(uintptr_t)"@", 2u, 0u); (void)r2;
    memcpy(after, g_mem.data+L, 256);
    printf("r=%d\n", (int)r);
    for (int q=0;q<64;q++) if (before[q]!=after[q]) printf("L+%02x: %08x -> %08x  (delta %d)\n", q*4, before[q], after[q], (int)(after[q]-before[q]));
    return 0;
  }
  if (k >= 400) {
    u32 L = mkstate();
    u32 A = 0x4000000, B = 0x4000100, NAMEP = 0x4000200;
    static const char* s0 = "return 1+1"; u32 n0 = (u32)strlen(s0);
    memcpy(g_mem.data + A, s0, n0 + 1);
    memcpy(g_mem.data + B, "!@", 3);
    memcpy(g_mem.data + NAMEP, "=chunk", 7);
    u32 c = (u32)(k-400);
    u32 r1 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32))c5[c])(&inst, L, NAMEP, A, n0, 0u);
    u32 r2 = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32))c5[c])(&inst, L, NAMEP, B, 2u, 0u);
    printf("d%d r1=%d r2=%d\n", c, (int)r1, (int)r2);
    return 0;
  }
  if ((k >= 200 && k < 300) || k < 0) {
    static const char* src0 = "return 1+1";
    u32 n0 = (u32)strlen(src0);
    u32 SRCP0 = 0x4000000, OUTP0 = 0x4010000;
    memcpy(g_mem.data + SRCP0, src0, n0 + 1);
    memset(g_mem.data + OUTP0, 0, 32);
    for (unsigned ci = 0; ci < sizeof(c4)/sizeof(c4[0]); ci++) {
      if (k >= 200 && (unsigned)(k-200) != ci) continue;
      memset(g_mem.data + OUTP0, 0, 32);
      u32 r = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32))c4[ci])(&inst, SRCP0, n0, 0u, OUTP0);
      u32 os = 0; memcpy(&os, g_mem.data + OUTP0, 4);
      printf("c4[%u] ret=0x%x outsize=%u", ci, r, os);
      if (r && r < 200u<<20) { printf(" bytes="); for (int b=0;b<16;b++) printf("%02x", g_mem.data[r+b]); }
      printf("\n");
    }
    return 0;
  }
  if (k >= 300) {
    u32 L = mkstate();
    u32 SRCP = 0x4000000, NAMEP = 0x4000100;
    static const char* s0 = "return 1+1"; u32 n0 = (u32)strlen(s0);
    memcpy(g_mem.data + SRCP, s0, n0 + 1);
    memcpy(g_mem.data + NAMEP, "=chunk", 7);
    u32 t1=0,t2=0;
    memcpy(&t1, g_mem.data+L+0x10, 4); memcpy(&t2, g_mem.data+L+0x14, 4);
    u32 c = (u32)(k-300);
    u32 r = ((u32(*)(w2c_WebClient*,u32,u32,u32,u32,u32))c5[c])(&inst, L, NAMEP, SRCP, n0, 0u);
    u32 a1=0,a2=0;
    memcpy(&a1, g_mem.data+L+0x10, 4); memcpy(&a2, g_mem.data+L+0x14, 4);
    printf("c%d ret=%d top(0x10)=%08x->%08x (0x14)=%08x->%08x\n", c, (int)r, t1,a1,t2,a2);
    return 0;
  }
  if (k == 100) {
    u64 slot = inst.w2c_0x5F_indirect_function_table.size - 2;
    wasm_rt_funcref_t* t = inst.w2c_0x5F_indirect_function_table.data;
    int found=0;
    for (u64 i = 0; i + 1 < inst.w2c_0x5F_indirect_function_table.size; i++) {
      if (t[i].func_type && memcmp(t[i].func_type, w2c_WebClient_t9, 32) == 0) {
        t[slot] = t[i];
        printf("matched type at slot %llu\n",(unsigned long long)i);
        found=1;
        break;
      }
    }
    printf("found=%d\n",found);
    t[slot].func = (wasm_rt_function_ptr_t)my_alloc;
    t[slot].module_instance = &inst;
    printf("slot=%llu\n", (unsigned long long)slot);
    printf("slottype=%p t9=%p eq=%d\n", (void*)t[slot].func_type, (void*)w2c_WebClient_t9, t[slot].func_type ? memcmp(t[slot].func_type, w2c_WebClient_t9, 32) : -99);
    printf("func=%p\n", (void*)t[slot].func);
    u32 L = w2c_WebClient_f151875(&inst, (u32)slot, 0u);
    printf("lua_newstate -> 0x%x  hdr=%d,%d\n", L, g_mem.data[L], g_mem.data[L+1]);
    { u32 w[12]; memcpy(w, g_mem.data+L, 48); for (int q=0;q<12;q++) printf("L[%02x]=%08x\n", q*4, w[q]); }
    return L ? 0 : 3;
  }
  if (k >= 0) { u32 r = ((u32(*)(w2c_WebClient*))cands[k])(&inst); printf("cand %d -> 0x%x\n", k, (unsigned)r); return r ? 0 : 2; }
  const char* src = "return 1+1"; u32 n = (u32)strlen(src);
  u32 SRCP = 0x4000000, OUTP = 0x4010000;
  memcpy(g_mem.data + SRCP, src, n + 1); memset(g_mem.data + OUTP, 0, 16);
  printf("parse=%d\n", (int)w2c_WebClient_f151425(&inst, SRCP, n));
  u32 out = 0; u32 bc = w2c_WebClient_f151424(&inst, SRCP, n, OUTP);
  memcpy(&out, g_mem.data + OUTP, 4);
  printf("compile ret=0x%x outsize=%u\n", bc, out);
  return bc ? 0 : 1;
}

/* rt2.o renames wasm_rt_trap so it does not collide with ours; wrap it to see
 * the code when the runtime throws without an active exception. */
void __real_wc_rt2_trap(int code);
void __wrap_wc_rt2_trap(int code) {
  fprintf(stderr, "[rt2 ] trap %d depth=%u\n", code, (unsigned)wasm_rt_call_stack_depth);
  __real_wc_rt2_trap(code);
}
