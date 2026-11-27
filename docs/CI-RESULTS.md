# Heavy-build comparison results

Fill this table from the `metrics.json` artifact of each Actions run.

| Run | Tool | Opt | TUs | Translate | Compile | lib size | Peak RSS | Notes |
|---|---|---|---|---|---|---|---|---|
| — | WABT wasm2c (`--num-outputs=512`) | O1 | — | — | — | — | — | pending first CI run |
| — | WABT wasm2c | O0 | — | — | — | — | — | pending |
| — | w2c2 | O0 | — | — | — | — | — | pending; known EH blocker |
| — | Cranelift object | — | — | — | — | — | — | pending; historical ~4 min |

Reference points already measured locally (do not re-measure on the dev box):

- w2c2 + clang `-O0`, 304 files / 688 MiB C: **4,665 s (78 min)** on 2 GB RAM.
- Cranelift `.o` for the 154k-function module: **~261 s**, 342 MB object.
- Wasmtime JIT compile: **~2.4 s**; output ~14% slower than LLVM codegen.
- `wasm-opt --dce` removes **zero** functions (see `STRIP-DCE-TEST.md`).
