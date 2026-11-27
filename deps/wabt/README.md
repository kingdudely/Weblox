# WABT 1.0.42 wasm2c runtime (vendored)

Copied verbatim from the WABT 1.0.42 release source tarball (`wasm2c/`),
SHA-256 `a76cda3c174a43097863a07fc0b0c202f770f53e21806ea2636f167d1ffb1e30`.

`WC.h` includes `wasm-rt.h` and `wasm-rt-exceptions.h`. The host must see the
same `wasm_rt_shared_memory_t` layout as the prebuilt engine library, so these
headers are pinned rather than taken from WABT master (which differs).

The C files are compiled into the host only on platforms whose engine library
does not export the `wasm_rt_*` symbols.

Do not edit these files. Replacing them requires rebuilding all engine bundles
against the same WABT version and updating `deps/pins.env`.
