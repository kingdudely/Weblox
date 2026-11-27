# Build strategy

GitHub Actions is a builder. Host development happens in the repo's C sources
with an AI or a human editor.

```
pins.env (engine version + sha256)
        │
        ▼
GitHub Actions: wasm → generated C → per-OS engine bundles + WC.h
        │
        ▼
Release tagged with the pinned Roblox version
        │
        ▼
CMake >= 3.20: host sources + WC.h → executable
```

- `tools/ci/pins.env` is the source of truth for the engine build.
- The portable host builds with stock CMake >= 3.20 and the `zstd` CLI; see
  `v3-wasm2c/cpp/README.md`. The default engine mode consumes the pinned
  per-OS shared library; `-DWEBLOX_ENGINE_MODE=source` compiles the pinned
  generated-C bundle locally as the portable fallback (slow, validated in CI).
- `tools/ci/README.md` lists the canonical scripts and exact commands.
- `docs/CI-RESULTS.md` records measured tool comparisons.
- `v3-wasm2c/README.md` records what is missing from the parked wasm2c host.

Rules:

1. Heavy work (translate/compile/link) goes to Actions; local machine has ~2 GB.
2. Never patch the wasm.
3. Never hardcode guest pointers or import indices — generate them from `WC.h`.
4. Keep scripts small and readable; diagnostic experiments are labelled as such.
