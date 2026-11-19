#include <string.h>

wasm_rt_shared_memory_t g_mem;
char g_env[64];
char g_wasi[64];

wasm_rt_shared_memory_t* w2c_env_memory(struct w2c_env* e){ (void)e; return (wasm_rt_shared_memory_t*)&g_mem; }

u32 w2c_env_HaveOffsetConverter(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_RBX_restoreWasmStackTraceLimit(struct w2c_env*a0, f64 a1) { (void)a0; (void)a1; }
f64 w2c_env_RBX_setFullWasmStackTrace(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_0x5F_assert_fail(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5FrbxFutexWaitMainJspi(struct w2c_env*a0, u32 a1, u32 a2, f64 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_clear(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_contains(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_file_size(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_buffer_offset(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_get_offset(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_list_sized(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_env_0x5F_asyncjs_0x5Frbx_opfs_open(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_remove_batch(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_0x5F_asyncjs_0x5Frbx_opfs_set(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
void w2c_env_0x5F_call_sighandler(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
u32 w2c_env_0x5F_pthread_create_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_0x5F_syscall_getegid32(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_0x5F_syscall_geteuid32(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_0x5F_syscall_getgid32(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_0x5F_syscall_getuid32(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_0x5Fabort_js(struct w2c_env*a0) { (void)a0; }
void w2c_env_0x5Fembind_register_bigint(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u64 a4, u64 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_0x5Fembind_register_bool(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_0x5Fembind_register_emval(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_0x5Fembind_register_float(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_0x5Fembind_register_function(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; }
void w2c_env_0x5Fembind_register_integer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_0x5Fembind_register_memory_view(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_0x5Fembind_register_std_string(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_0x5Fembind_register_std_wstring(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_0x5Fembind_register_void(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
u32 w2c_env_0x5Femscripten_fetch_get_response_headers(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_0x5Femscripten_fetch_get_response_headers_length(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
extern void* g_inst;
void w2c_env_0x5Femscripten_init_main_thread_js(struct w2c_env*a0, u32 a1) {
  (void)a0;
  w2c_WebClient_0x5Femscripten_thread_init((void*)g_inst, a1, 1u, 1u, 1u, 262144u, 0u);
  w2c_WebClient_0x5Femscripten_tls_init((void*)g_inst);
}
void w2c_env_0x5Femscripten_log_formatted(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_0x5Femscripten_notify_mailbox_postmessage(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
f64 w2c_env_0x5Femscripten_receive_on_main_thread_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; return 0; }
void w2c_env_0x5Femscripten_runtime_keepalive_clear(struct w2c_env*a0) { (void)a0; }
void w2c_env_0x5Femscripten_thread_cleanup(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_0x5Femscripten_thread_mailbox_await(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_0x5Femscripten_thread_set_strongref(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_env_0x5Fgmtime_js(struct w2c_env*a0, u64 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_0x5Flocaltime_js(struct w2c_env*a0, u64 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u64 w2c_env_0x5Fmktime_js(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_0x5Fmsync_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u64 a6) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return 0; }
void w2c_env_0x5Ftzset_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_applyPendingCanvasResize(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_env_blur_text_input(struct w2c_env*a0) { (void)a0; }
void w2c_env_choose_current_text_input(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_copy_pending_text(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_emscripten_asm_const_async_on_main_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
f64 w2c_env_emscripten_asm_const_double(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_asm_const_int(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_asm_const_int_sync_on_main_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
void w2c_env_emscripten_async_call(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_emscripten_check_blocking_allowed(struct w2c_env*a0) { (void)a0; }
void w2c_env_emscripten_console_error(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_emscripten_console_log(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_emscripten_console_warn(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
f64 w2c_env_emscripten_date_now(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_emscripten_errn(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; if(!g_mem.data) return; fwrite(g_mem.data+a1,1,a2,stderr); fputc(10,stderr); }
u32 w2c_env_emscripten_exit_pointerlock(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_emscripten_exit_with_live_runtime(struct w2c_env*a0) { (void)a0; }
void w2c_env_emscripten_fetch_free(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_env_emscripten_get_canvas_element_size(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
f64 w2c_env_emscripten_get_device_pixel_ratio(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_emscripten_get_element_css_size(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_get_gamepad_status(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_emscripten_get_heap_max(struct w2c_env*a0) { (void)a0; return 0; }
f64 w2c_env_emscripten_get_now(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_emscripten_get_num_gamepads(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_emscripten_num_logical_cores(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_emscripten_outn(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; if(!g_mem.data) return; fwrite(g_mem.data+a1,1,a2,stdout); fputc(10,stdout); }
u32 w2c_env_emscripten_pc_get_function(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_emscripten_request_pointerlock(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_emscripten_resize_heap(struct w2c_env*a0, u32 a1) { (void)a0;
  if ((uint64_t)a1 <= g_mem.size) return 1;
  uint64_t pg = ((uint64_t)a1 + 65535) / 65536;
  if (pg > g_mem.max_pages) return 0;
  return wasm_rt_grow_memory_shared(&g_mem, pg - g_mem.pages) == (uint64_t)-1 ? 0 : 1; }
void w2c_env_emscripten_runtime_keepalive_push(struct w2c_env*a0) { (void)a0; }
u32 w2c_env_emscripten_sample_gamepad_data(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_emscripten_set_blur_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_canvas_element_size(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_set_focus_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_gamepadconnected_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_emscripten_set_gamepaddisconnected_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_emscripten_set_keydown_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_keyup_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_mousedown_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_mouseenter_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_mouseleave_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_mousemove_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_mouseup_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_pointerlockerror_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_touchcancel_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_touchend_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_touchmove_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_touchstart_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_set_wheel_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return 0; }
u32 w2c_env_emscripten_stack_snapshot(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_emscripten_stack_unwind_buffer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
void w2c_env_emscripten_start_fetch(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_emscripten_unwind_to_js_event_loop(struct w2c_env*a0) { (void)a0; }
u32 w2c_env_emscripten_webgl_create_context(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_emscripten_webgl_destroy_context(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_emscripten_webgl_get_context_attributes(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_emscripten_webgl_make_context_current(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_emscripten_websocket_close(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_websocket_delete(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_emscripten_websocket_new(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_emscripten_websocket_send_binary(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_emscripten_websocket_send_utf8_text(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_emscripten_websocket_set_onclose_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_emscripten_websocket_set_onerror_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_emscripten_websocket_set_onmessage_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
u32 w2c_env_emscripten_websocket_set_onopen_callback_on_thread(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
void w2c_env_exit(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_fiber_destroy_js(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_focus_text_input(struct w2c_env*a0) { (void)a0; }
f64 w2c_env_get_effective_device_pixel_ratio(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_get_hardware_concurrency(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_glActiveTexture(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glAttachShader(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBindAttribLocation(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glBindBuffer(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBindBufferBase(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glBindBufferRange(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glBindFramebuffer(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBindRenderbuffer(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBindTexture(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBindVertexArray(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glBlendFunc(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glBlendFuncSeparate(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glBlitFramebuffer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; }
void w2c_env_glBufferData(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glBufferSubData(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_glCheckFramebufferStatus(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
void w2c_env_glClear(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glClearBufferfi(struct w2c_env*a0, u32 a1, u32 a2, f32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glClearBufferfv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glClearBufferiv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glClearColor(struct w2c_env*a0, f32 a1, f32 a2, f32 a3, f32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glClearDepthf(struct w2c_env*a0, f32 a1) { (void)a0; (void)a1; }
void w2c_env_glClearStencil(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_env_glClientWaitSync(struct w2c_env*a0, u32 a1, u32 a2, u64 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
void w2c_env_glClipControlEXT(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glColorMask(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glCompileShader(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glCompressedTexImage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; }
void w2c_env_glCompressedTexImage3D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; }
void w2c_env_glCompressedTexSubImage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; }
void w2c_env_glCompressedTexSubImage3D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10, u32 a11) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; }
void w2c_env_glCopyTexSubImage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; }
u32 w2c_env_glCreateProgram(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_glCreateShader(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
void w2c_env_glCullFace(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDeleteBuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDeleteFramebuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDeleteProgram(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDeleteRenderbuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDeleteShader(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDeleteSync(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDeleteTextures(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDeleteVertexArrays(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDepthFunc(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDepthMask(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDisable(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDisableVertexAttribArray(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glDrawArrays(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glDrawArraysInstanced(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glDrawBuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glDrawElements(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glDrawElementsInstanced(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glEnable(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glEnableVertexAttribArray(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_env_glFenceSync(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_env_glFramebufferRenderbuffer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glFramebufferTexture2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glFramebufferTextureLayer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glGenBuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGenFramebuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGenRenderbuffers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGenTextures(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGenVertexArrays(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGenerateMipmap(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glGetActiveUniform(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; }
void w2c_env_glGetActiveUniformBlockiv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_glGetError(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_glGetIntegerv(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glGetProgramInfoLog(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glGetProgramiv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glGetShaderInfoLog(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glGetShaderiv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
u32 w2c_env_glGetString(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
u32 w2c_env_glGetUniformBlockIndex(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_env_glGetUniformLocation(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_env_glInvalidateFramebuffer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glLinkProgram(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_env_glMapBufferRange(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
void w2c_env_glPixelStorei(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glPolygonOffset(struct w2c_env*a0, f32 a1, f32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glReadPixels(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; }
void w2c_env_glRenderbufferStorage(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glRenderbufferStorageMultisample(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glScissor(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glShaderSource(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_glStencilFunc(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glStencilMask(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glStencilOp(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glTexImage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; }
void w2c_env_glTexImage3D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; }
void w2c_env_glTexParameterf(struct w2c_env*a0, u32 a1, u32 a2, f32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glTexParameterfv(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glTexParameteri(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_glTexStorage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_glTexStorage3D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; }
void w2c_env_glTexSubImage2D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; }
void w2c_env_glTexSubImage3D(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10, u32 a11) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; }
void w2c_env_glUniform1i(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_glUniformBlockBinding(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
u32 w2c_env_glUnmapBuffer(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
void w2c_env_glUseProgram(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_glVertexAttribPointer(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; }
void w2c_env_glViewport(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_has_keyboard(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_has_mouse(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_install_pointer_source_tracking(struct w2c_env*a0) { (void)a0; }
u32 w2c_env_is_mobile_browser(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_is_mouse_sourced_event(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_is_touch_mobile(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_is_touch_screen_device(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_js_copy_keyboard_layout_key(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_js_external_content_sharing_deliver(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
u32 w2c_env_js_fetch_file(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_env_js_keyboard_layout_status(struct w2c_env*a0) { (void)a0; return 0; }
u32 w2c_env_js_linking_open_url(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_env_js_request_keyboard_layout(struct w2c_env*a0) { (void)a0; }
u32 w2c_env_js_web_client_page_visible(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_preallocate_emscripten_key_event_buffer(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_rbx_opfs_close(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_rbx_opfs_free(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_rbx_opfs_install_helpers(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
u32 w2c_env_rbx_opfs_read_sync_if_cached(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; return 0; }
void w2c_env_rbx_wasm_rna_close_js(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_rbx_wasm_rna_connect_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10, u32 a11, u32 a12, u32 a13) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; (void)a12; (void)a13; }
u32 w2c_env_rbx_wasm_rna_is_supported_js(struct w2c_env*a0) { (void)a0; return 0; }
void w2c_env_rbx_wasm_rna_open_stream_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_rbx_wasm_rna_pause_stream_read_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_rbx_wasm_rna_receive_ring_consumed_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_rbx_wasm_rna_resume_stream_read_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_rbx_wasm_rna_send_connection_close_js(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_rbx_wasm_rna_send_datagram_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_rbx_wasm_rna_shutdown_stream_read_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_rbx_wasm_rna_shutdown_stream_write_js(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_rbx_wasm_rna_write_stream_js(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_env_read_host_info_int(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; return 0; }
void w2c_env_read_host_info_string(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_report_crash(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_reset_current_text_input(struct w2c_env*a0) { (void)a0; }
void w2c_env_schedule_main_loop(struct w2c_env*a0) { (void)a0; }
void w2c_env_set_crash_attribute(struct w2c_env*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void w2c_env_set_text_box_horiz_alignment(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_set_text_box_overlay(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_set_text_box_vert_alignment(struct w2c_env*a0, u32 a1) { (void)a0; (void)a1; }
void w2c_env_set_text_color(struct w2c_env*a0, f32 a1, f32 a2, f32 a3, f32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
void w2c_env_setup_font(struct w2c_env*a0, u32 a1, u32 a2, u32 a3, f32 a4, u32 a5) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; }
void w2c_env_shift_canvas(struct w2c_env*a0, f32 a1) { (void)a0; (void)a1; }
void w2c_env_sync_text_and_pos(struct w2c_env*a0, u32 a1, u32 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; }
void w2c_env_update_text_box_rect(struct w2c_env*a0, f32 a1, f32 a2, f32 a3, f32 a4) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; }
u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1*a0, u32 a1, u64 a2, u32 a3) { (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1*a0, u32 a1) { (void)a0; (void)a1; }
u32 w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1*a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; return 0; }
