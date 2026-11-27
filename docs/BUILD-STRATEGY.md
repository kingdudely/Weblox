# Build strategy

GitHub Actions is a builder. Host development happens in the repo's C sources
with an AI or a human editor.

```
pins.env (engine version + sha256)
        │
        ▼
GitHub Actions: wasm → generated C → libweblox_engine.a + WC.h
        │
        ▼
GitHub Release: ci-<run_id>
        │
        ▼
host sources in v3-wasm2c/ + WC.h → executable
```

- `tools/ci/pins.env` is the source of truth for the engine build.
- `tools/ci/README.md` lists the canonical scripts and exact commands.
- `docs/CI-RESULTS.md` records measured tool comparisons.
- `v3-wasm2c/README.md` records what is missing from the parked wasm2c host.

Rules:

1. Heavy work (translate/compile/link) goes to Actions; local machine has ~2 GB.
2. Never patch the wasm.
3. Never hardcode guest pointers or import indices — generate them from `WC.h`.
4. Keep scripts small and readable; diagnostic experiments are labelled as such.
