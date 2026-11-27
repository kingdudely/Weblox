# Heavy-build comparison results

Fill this table from the `metrics.json` artifact of each Actions run.

| Run | Tool | Opt | TUs | Translate | Compile | lib size | Peak RSS | Notes |
|---|---|---|---|---|---|---|---|---|
| — | WABT wasm2c (`--num-outputs=512`) | O1 | — | — | — | — | — | pending first CI run |
| — | WABT wasm2c | O0 | — | — | — | — | — | pending |
| [45927563289](https://github.com/kingdudely/Weblox/actions/runs/45927563289) | w2c2 `bc3fd9ac` | O0 | 0 | <1 s | n/a | none | 154 MiB | **blocked**: translator has no EH support (`invalid value type` on stock module). `tools/ci/w2c2.sh`, branch `ci-w2c2` |
| [45927451165](https://github.com/kingdudely/Weblox/actions/runs/45927451165) | Cranelift `wasm2obj --all` | — | 154,550 | **100 s** (98 s codegen + 1.2 s object) | n/a | 346,096,072 B `.a` | 4,949,432 KB | status=partial: symbol-only `WC.h`, no C-ABI trampolines/VMContext. Branch `ci-cranelift` |
| [45927142057](https://github.com/kingdudely/Weblox/actions/runs/45927142057) | WABT wasm2c 1.0.42 | O1 | 512 | aborted after 136 s | n/a | none | 5,113,220 KB | `WABT_CATCH_BAD_ALLOC_AND_EXIT` — OOM in `WriteC` |
| [45928461139](https://github.com/kingdudely/Weblox/actions/runs/45928461139) | WABT 1.0.36 (source build) | — | — | <1 s | n/a | none | 380,748 KB | cannot parse `exnref` type: `expected valid result type (got -0x17)` |
| [45934107270](https://github.com/kingdudely/Weblox/actions/runs/45934107270) | generated C from Release, clang `-O0` | O0 | 32 | n/a | **590 s** | **1,114,530,586 B** `.a` | 8,685,540 KB | success: `libweblox_engine.a` + `WC.h` published to `ci-45934107270` |
| [45935598599](https://github.com/kingdudely/Weblox/actions/runs/45935598599) | fetch + smoke link | — | 277 stubs | n/a | run 85 s | 949,585,328 B exe | 1,168,620 KB | **`SMOKE_OK mem=268435456`**, `smoke_rc=0` |
| [45943370929](https://github.com/kingdudely/Weblox/actions/runs/45943370929) | OS matrix (e.g. generated C → clang -O0) | O0 | 32 | n/a | Linux 429 s, Windows ~445 s | Linux `.a`, Windows `.lib` | — | Linux + Windows success; macOS failed on WABT asset name |
| [45944686964](https://github.com/kingdudely/Weblox/actions/runs/45944686964) | OS matrix retry | O0 | 32 | n/a | Linux 429 s, Windows ~445 s | published | — | macOS failed at dylib link (undefined imports) |
| [46025388958](https://github.com/kingdudely/Weblox/actions/runs/46025388958) | macOS-only retry | O0 | 32 | n/a | **588 s** | `.a` 930,493,008 B + `.dylib` 830,133,728 B | — | success with `-undefined dynamic_lookup` |
| [46027259411](https://github.com/kingdudely/Weblox/actions/runs/46027259411) | full OS matrix | O0 | 32 | n/a | Linux 504 s, Windows 438 s, macOS 1187 s | all three platforms in `ci-matrix-46027259411` | — | **success**: `.a`+`.so` (Linux), `.a`+`.dylib` (macOS), `.lib` (Windows) |

## Revival attempt (2026-11-27, free runners)

The historical wasm2c tree was found on disk at `/home/john/w2cout/`: a
complete WABT 1.0.42 translation of the 169,929-function stock build with all
12 RNA imports, 32 `WC_*.c` files totalling 2.3 GB. It was never committed and
its smoke harness used hardcoded guest indices/pointers, so it was abandoned.

Translations of both `version-f807b21caba84809` (32 and 64 outputs) on
`ubuntu-latest` abort in `WriteC` at ~4.9 GiB peak even though `/proc/meminfo`
reports 15.6 GiB — the runner is memory-capped well below its apparent RAM.
The generated C was therefore uploaded to Release `ci-local-wasm2c-generated`
and compiled directly by `tools/ci/compile-generated.sh`.

## WABT wasm2c abort investigation

Probe [45927708434](https://github.com/kingdudely/Weblox/actions/runs/45927708434)
showed the abort is independent of `--num-outputs` (1, 64 and 512 all abort at
~12 s with 4.88 GiB peak RSS). Probe
[45927542617](https://github.com/kingdudely/Weblox/actions/runs/45927542617)
showed `--enable-threads` is required (shared memory) and that every
threads-enabled attempt aborts silently. WABT 1.0.42's `wasm2c.cc` wraps
`main` in `WABT_CATCH_BAD_ALLOC_AND_EXIT`, so the silent abort with a ~4.9 GiB
peak is an allocation failure inside `WriteC`, not a flag or EH problem.
Cranelift succeeds at a similar peak (4.7 GiB), so the standard runner's
memory ceiling sits just between the two.

**Conclusion (2026-11-27):** on a standard free runner, only Cranelift produces
an engine library. WABT 1.0.36 cannot parse the module's `exnref` type;
WABT 1.0.42 parses it but OOMs in `WriteC`; w2c2 has no EH support at all.
A larger (billed) runner could confirm whether 1.0.42 is purely memory-bound,
but that path is not portable to low-RAM machines. `tools/ci/engine.sh` is kept
as the portable Stage A reference for any machine/runner with more RAM.

Reference points already measured locally (do not re-measure on the dev box):

- w2c2 + clang `-O0`, 304 files / 688 MiB C: **4,665 s (78 min)** on 2 GB RAM.
- Cranelift `.o` for the 154k-function module: **~261 s**, 342 MB object.
- Wasmtime JIT compile: **~2.4 s**; output ~14% slower than LLVM codegen.
- `wasm-opt --dce` removes **zero** functions (see `STRIP-DCE-TEST.md`).
