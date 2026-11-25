// Deno desktop exposes no pointer-lock API. On X11 we hide the OS cursor and
// warp it back to the window centre while the engine holds a lock, so the
// pointer stays inside the window and relative mouse motion keeps flowing.
// Any failure (non-X11 session, missing libs) disables the emulation quietly.

type Pointer = Deno.PointerValue;

const lib = (() => {
  try {
    const X = Deno.dlopen("libX11.so.6", {
      XOpenDisplay: { parameters: ["pointer"], result: "pointer" },
      XDefaultRootWindow: { parameters: ["pointer"], result: "u64" },
      XWarpPointer: {
        parameters: ["pointer", "u64", "u64", "i32", "i32", "u32", "u32", "i32", "i32"],
        result: "i32",
      },
      XFlush: { parameters: ["pointer"], result: "i32" },
    });
    const F = Deno.dlopen("libXfixes.so.3", {
      XFixesHideCursor: { parameters: ["pointer", "u64"], result: "i32" },
      XFixesShowCursor: { parameters: ["pointer", "u64"], result: "i32" },
    });
    const dpy = X.symbols.XOpenDisplay(null as Pointer);
    if (!dpy) return null;
    return { X, F, dpy, root: X.symbols.XDefaultRootWindow(dpy) };
  } catch {
    return null;
  }
})();

export function lockCursor(on: boolean) {
  if (!lib) return;
  try {
    if (on) lib.F.symbols.XFixesHideCursor(lib.dpy, lib.root);
    else lib.F.symbols.XFixesShowCursor(lib.dpy, lib.root);
    lib.X.symbols.XFlush(lib.dpy);
  } catch { /* emulation is best-effort */ }
}

export function recenterCursor(x: number, y: number) {
  if (!lib) return;
  try {
    lib.X.symbols.XWarpPointer(lib.dpy, 0n, lib.root, 0, 0, 0, 0, Math.round(x), Math.round(y));
    lib.X.symbols.XFlush(lib.dpy);
  } catch { /* emulation is best-effort */ }
}
