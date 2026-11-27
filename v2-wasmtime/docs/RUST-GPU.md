# Rust host: native GPU path (this branch)

Status: **real native rendering is wired for both backends**. The Rust host no
longer hands the guest fake renderer strings or synthetic device handles as a
substitute for GPU work. It creates an X11 window, a real EGL/GLES3 context and
a real wgpu Vulkan device/surface on that window. No game client, browser or
Deno process was launched for the verification below.

## What is real

### `src/gfx.rs` — window, EGL/GLES3, ABI marshalling

* Xlib window: `XOpenDisplay` / `XCreateSimpleWindow` / `XMapWindow`, event
  masks, `WM_DELETE_WINDOW`, keyboard/mouse forwarding into
  `imports::input`, resize/close handling and X11 screenshot support.
* EGL 1.5 via `khronos-egl`'s dynamic loader (`libEGL.so.1`), X11 platform
  first with a plain `eglGetDisplay` fallback. Config search asks for
  RGBA8 + depth24 + stencil8, then relaxes to GLES3-only and finally
  window-bit-only. A genuine GLES 3.0 context is created and made current.
  Verified renderer: `OpenGL ES 3.2 Mesa 25.0.7 | Mesa Intel(R) UHD Graphics
  620 (KBL GT2)`.
* The real visual id and screen index are queried from
  `XGetWindowAttributes` and exposed via `gfx::handles_ex()` so wgpu's Xlib
  Vulkan surface matches the X window (a guessed visual id of 0 is avoided).
* **Guest pointer translation**: wasmtime's shared linear memory is a host
  allocation, so an i32 guest offset is not a host address. `gfx::call()`
  translates every pointer argument listed in `ptr_args()` into a host
  address before entering GLES; NULL stays NULL. The only exception is
  `glDrawElements*` / `glVertexAttribPointer`, whose last word is a byte
  offset into a bound GL buffer and must be passed through unchanged.
* `glShaderSource` gets a second translation pass: the guest passes an array
  of guest string offsets, which is rebuilt as a host `char**` array.
* **Typed function pointers for floats**: `glClearColor`, `glClearDepthf`,
  `glPolygonOffset`, `glClearBufferfi` and `glTexParameterf` are dispatched
  with typed `extern "system" fn(f32, ...)` signatures. The old generic
  12×`u64` call put floats in GPRs instead of XMM registers, so the driver read
  garbage. Integer/pointer imports still use the generic dispatcher (valid on
  x86-64 SysV).
* `glMapBufferRange` / `glUnmapBuffer` copy through a guest `malloc` buffer;
  `glGetString` copies the real driver string into guest memory;
  `glGetStringi` enumerates `GL_EXTENSIONS` from the live context (using
  `gfx::call_raw`, since that pointer is a host stack address).
* `gfx::poll()` / `gfx::present()` are driven from `main.rs` when the guest
  created a GL context (`St.gl_ctx`).

### `src/imports/gl.rs` — 103 GL imports + 4 context imports

* Every GL import in `TABLE` forwards to the real GLES3 context. Embind and
  guest-memory semantics are unchanged: results are written back as `I32`
  wasm values, and pointer outputs land in guest memory via the translation
  above. `glFenceSync` / `glClientWaitSync` / `glDeleteSync` translate host
  `GLsync*` values through a small id table so the guest never sees a host
  pointer it cannot represent.
* `emscripten_webgl_create_context` now initialises the EGL context, honours
  the glue's `explicitSwapControl` rejection (attributes byte 25), sets
  `St.gl_ctx` (so the main loop polls/presents) and returns context handle 1.
  `make_context_current` accepts 1; `destroy_context` clears `gl_ctx`.
* `emscripten_webgl_get_context_attributes` reports the **actual** EGL config
  (`GL_ALPHA_BITS`, `GL_DEPTH_BITS`, `GL_STENCIL_BITS`, `GL_SAMPLES`) in the
  Emscripten layout, with GLES3 major version 3. No hardcoded ANGLE strings.
* `applyPendingCanvasResize` now reports the window size on the first poll and
  on subsequent changes, matching `WebClient.js::ga` (`applyPendingResize`).
  Previously it swallowed the first notification.

### `src/imports/webgpu.rs` — 61 WebGPU imports

* `ensure()` creates an X11 window (through `gfx::ensure()`), a wgpu Xlib
  surface with the window's real visual id, a `HighPerformance` adapter
  compatible with that surface, and a device with the adapter's features and
  limits. `requestAdapter`, `requestDevice`, `Surface::configure`,
  `get_current_texture`, command submission and `Queue::present` are all real.
  Verified adapter: `Intel(R) UHD Graphics 620 (KBL GT2)`, backend `Vulkan`,
  surface format `Bgra8UnormSrgb`, 1280×720.
* Synthetic handles start at `0x7000_0000`, above the guest heap, so they
  cannot collide with the C++ wrapper pointers the engine passes as `out`
  parameters. If the guest export `emwgpuCreateDevice` is missing,
  `emscripten_webgpu_get_device` returns a real synthetic device handle
  instead of 0 (device-loss workaround removed).
* `wgpuQueueSubmit` is implemented: it reads the guest array of command-buffer
  handles, takes each `Obj::Cmd` exactly once and calls `Queue::submit`.
  Previously it was a no-op, so all engine rendering was silently dropped.
* `wgpuSurfaceConfigure` resolves a requested format against the adapter's
  actual surface capabilities and handles a reconfigure safely (no `unwrap`
  on a missing adapter). `applyPendingBrowserCanvasResize` reconfigures with
  the live surface capabilities.
* `presentBrowserFrame` copies the engine's finished texture into the real
  swapchain texture, submits the copy and presents it. `WC_NORENDER` remains
  an explicit opt-out for headless work.
* All descriptor readers (render pipeline, bind groups, writes, copies,
  queries, limits, mapping, shader modules, async map completion) use guest
  memory reads via `M` and real wgpu objects in the handle table; those were
  already real and remain unchanged.

### `src/imports/wgt.rs` — enum tables

* Filled the 14 placeholder texture-format indices 50–63 with wgpu's
  `Bc1RgbaUnorm` … `Bc7RgbaUnormSrgb` (WebGPU's BC1–BC7 order), so
  `tex_format()` and `tex_format_index()` no longer return `None` for those
  formats. The other tables (topology, blend, samplers, features, …) are
  unchanged.

## Verification evidence

Commands run in this worktree (`rust-host`), no client launched:

```sh
cd v2-wasmtime
cargo check --offline                     # Finished, no errors
cargo test --offline gfx::tests -- --nocapture
```

Test output (abbreviated):

```
[gfx ] egl Mesa Project / 1.5
[gfx ] OpenGL ES 3.2 Mesa 25.0.7-2+deb13u1 | Mesa Intel(R) UHD Graphics 620 (KBL GT2)
[selftest] GL PASS OpenGL ES 3.2 Mesa 25.0.7 | Mesa Intel(R) UHD Graphics 620 (KBL GT2)
[wgpu] Intel(R) UHD Graphics 620 (KBL GT2) Vulkan
[selftest] PASS adapter=Intel(R) UHD Graphics 620 (KBL GT2) backend=Vulkan format=Bgra8UnormSrgb size=1280x720 present=ok
test result: ok. 2 passed; 0 failed
```

`gfx::tests::gl_context` creates the EGL context, issues a **typed**
`glClearColor(0.1, 0.2, 0.3, 1.0)` + `glClear` and checks `glGetError() == 0`.
`gfx::tests::gpu_selftest` calls `imports::webgpu::selftest()`, which creates
the adapter/device/surface through the same `ensure()` path as the imports,
configures it, renders one clear pass, submits it and presents it. Both tests
are no-ops when `DISPLAY` is unset. `WC_GFXPROBE=1 <binary>` additionally
prints the wgpu adapter/device capabilities on the normal window path.

## Remaining gaps (honest list)

* **No full join/render run was attempted** (out of scope). The tests above
  exercise the native GPU stack, not engine wasm call sequences. Descriptor
  offsets in `webgpu.rs` are inherited from the previous implementation and
  match the C++ bridge (`host/wgpu-bridge`) which was validated against the
  same engine glue; they are not independently re-derived here.
* Debug-group imports (`wgpu*PushDebugGroup` / `PopDebugGroup`) and the
  compute-pass debug groups remain no-ops. They affect only labels.
* Dynamic bind-group offsets are ignored (`&[]` is passed to wgpu).
* `emwgpuShaderModuleGetCompilationInfo` still reports zero messages; it does
  not query real WGSL diagnostics.
* `getWebGpuTimestampInterval` is still a zero-returning stub in
  `imports/stubs.rs` (not owned by this change).
* `wgpuSurfaceGetCurrentTexture` remains a no-op import; the engine ABI in
  `docs/imports.tsv` does not contain it — the swapchain texture is acquired
  inside `presentBrowserFrame`.
* Window event polling is driven by `St.gl_ctx` in `main.rs`. On a pure
  WebGPU join (no GL context) X11 events are not drained by the main loop, so
  an interactive resize/close is not handled until the GL path is active.
* The GL path uses raw GL object names from the driver (no per-type guest
  handle table like `host/gl_bridge.cpp`). Names are opaque i32 values and
  the translation layer passes them through; this is sufficient for the
  engine's WebGL2-shaped calls but is not a validation layer.
* Multiple GL contexts are not supported; `emscripten_webgl_create_context`
  always returns the single real EGL context (handle 1), matching Emscripten's
  single-context model.
