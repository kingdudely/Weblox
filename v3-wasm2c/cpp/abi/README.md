# WABT 1.0.42 wasm2c runtime

These files are copied verbatim from the **WABT 1.0.42 release source tarball**
(`wabt-1.0.42.tar.xz`, `wasm2c/`), SHA-256
`a76cda3c174a43097863a07fc0b0c202f770f53e21806ea2636f167d1ffb1e30`.

They are vendored rather than fetched at configure time for two reasons:

1. `WC.h` includes `wasm-rt.h` and `wasm-rt-exceptions.h`. Struct layouts such
   as `wasm_rt_shared_memory_t` must be **identical** to the ones the prebuilt
   engine library was compiled against. WABT's git master differs (for example
   `data_end` was removed), so a "latest" header would silently break the ABI.
2. The Windows DLL exports only the 57 names declared in `WC.h`. `wasm_rt_*`
   functions are not among them, so the host cannot call `wasm_rt_init` or
   `wasm_rt_allocate_memory_shared` from the DLL. The host compiles the runtime
   sources itself instead.

Do not edit these files. If the engine is rebuilt against a different WABT
version, replace the whole directory together with the engine bundles and
update `WABT_SOURCE_SHA256` in `tools/ci/pins.env`.
