#!/usr/bin/env bash
# STAGE A benchmark (expensive, rare): Cranelift wasm -> native ELF object,
# with a WABT wasm2c translation run alongside it as the comparison point.
#
# This is a measurement task, not a cacheable engine build: the pinned 100 MB
# module is fetched as-is (never patched) and every defined function is
# compiled.  No guest function index, pointer or VMContext offset is written
# here; every symbol in the header comes from the generated ELF symbol table.
#
# Output contract (same as tools/ci/engine.sh):
#   $CI_BUILD_DIR/engine/<key>/libweblox_engine.a
#   $CI_BUILD_DIR/engine/<key>/WC.h
#   $CI_BUILD_DIR/engine/<key>/manifest.json
#   $CI_BUILD_DIR/engine/<key>/env.sh
#   $CI_BUILD_DIR/engine_dir
#
# Env: WC_WASM, WASM_VERSION, WASM_SHA256, WABT_VERSION, WABT_ROOT, WASM2C,
#      WC_BUILD_DIR, WC_OPT, WC_OUTPUTS, WC_JOBS, WC_FORCE, WC_WASM2OBJ_DIR,
#      WC_WASM2OBJ_BIN, WC_CARGO/CARGO, CARGO_TARGET_DIR, AR, NM, WC_TIME.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

log()  { printf '[cranelift-object] %s\n' "$*"; }
fail() { printf '[cranelift-object] %s\n' "$*" >&2; exit 2; }

WASM=$(ci_fetch_wasm)
ci_fetch_wabt   # baseline wasm2c + WABT_INCLUDE for env.sh

WASM2OBJ_DIR=${WC_WASM2OBJ_DIR:-"$CI_ROOT/tools/wasm2obj"}
CARGO=${WC_CARGO:-${CARGO:-cargo}}
AR=${AR:-ar}
NM=${NM:-nm}
TIME_TOOL=${WC_TIME:-/usr/bin/time}
OPT=${WC_OPT:-O0}
OUTPUTS=${WC_OUTPUTS:-512}
JOBS=${WC_JOBS:-$(ci_nproc)}

[[ -d $WASM2OBJ_DIR && -f $WASM2OBJ_DIR/Cargo.toml ]] || fail "no wasm2obj sources at $WASM2OBJ_DIR"
if ! command -v "$CARGO" >/dev/null 2>&1; then
  if [[ -n ${HOME:-} && -x $HOME/.cargo/bin/cargo ]]; then
    CARGO="$HOME/.cargo/bin/cargo"
  else
    fail "cargo not found (set WC_CARGO)"
  fi
fi
CARGO_VERSION=$("$CARGO" --version | head -1)
WASM_SHA=$(ci_sha256 "$WASM")

ci_metric task cranelift-object
ci_metric wasm_version "$WASM_VERSION"
ci_metric wasm_sha256 "$WASM_SHA"
ci_metric wasm_bytes "$(wc -c < "$WASM" | tr -d ' ')"
ci_metric wabt_version "$WASM2C_VERSION"
ci_metric cargo_version "$CARGO_VERSION"
ci_metric opt "$OPT"
ci_metric outputs "$OUTPUTS"
ci_metric jobs "$JOBS"
ci_metric wasm_patched 0

# Any input that changes the emitted object is part of the key.  This run is
# never skipped (it is a benchmark), but the key identifies the result.
DESC="$CI_BUILD_DIR/cranelift-object-key.txt"
{
  printf '%s\n' "$WASM_SHA" "$WASM_VERSION" "$CARGO_VERSION" "$OPT" "$OUTPUTS"
  ci_sha256 "$WASM2OBJ_DIR/src/main.rs"
  ci_sha256 "$WASM2OBJ_DIR/Cargo.toml"
  ci_sha256 "$HERE/cranelift-object.sh"
} > "$DESC"
KEY=$(ci_sha256 "$DESC")
KEY=${KEY:0:16}

OUT="$CI_BUILD_DIR/engine/$KEY"
GEN="$OUT/gen"
OBJDIR="$OUT/obj"
LIB="$OUT/libweblox_engine.a"
OBJFILE="$OBJDIR/wasm2obj.o"
rm -rf "$OUT"
mkdir -p "$GEN" "$OBJDIR"
printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
ci_metric engine_dir "$OUT"
ci_metric cache_key "$KEY"
ci_metric cached 0

# ---------------------------------------------------------------- baseline --
# wasm2c only translates here; compiling ~700 MB of generated C at O0 took
# 4665 s in docs/NATIVE-AOT-FINDINGS.md and is intentionally not repeated.
BASEGEN="$CI_BUILD_DIR/baseline/wabt-$KEY"
mkdir -p "$BASEGEN"
BASEFLAGS=$(ci_wasm2c_flags "$WASM2C")
BASELOG="$CI_BUILD_DIR/baseline-wasm2c.log"
ci_metric wabt_baseline_scope translation_only
log "baseline: wasm2c translation with $OUTPUTS outputs"
if T0=$SECONDS; "$WASM2C" $BASEFLAGS --module-name=WebClient \
    --num-outputs="$OUTPUTS" "$WASM" -o "$BASEGEN/WC.c" > "$BASELOG" 2>&1; then
  WABT_TRANSLATE=$((SECONDS - T0))
  ci_metric wabt_status ok
  ci_metric wabt_translate_secs "$WABT_TRANSLATE"
  ci_metric wabt_flags "$BASEFLAGS"
  mapfile -t BASE_TUS < <(find "$BASEGEN" -maxdepth 1 -name 'WC*.c' -print | sort)
  ci_metric wabt_translated_tus "${#BASE_TUS[@]}"
  if ((${#BASE_TUS[@]})); then
    ci_metric wabt_translated_bytes "$(cat "${BASE_TUS[@]}" | wc -c | tr -d ' ')"
  else
    ci_metric wabt_translated_bytes 0
  fi
else
  # The baseline is informational; a feature the pinned WABT cannot translate
  # must not hide a successful Cranelift measurement.
  WABT_TRANSLATE=0
  ci_metric wabt_status failed
  log "wasm2c baseline failed; see $BASELOG"
  tail -20 "$BASELOG" || true
fi
rm -rf "$BASEGEN"

# ------------------------------------------------------------ cranelift --
TARGET_DIR=${CARGO_TARGET_DIR:-"$WASM2OBJ_DIR/target"}
if [[ -n ${WC_WASM2OBJ_BIN:-} ]]; then
  TOOL=$WC_WASM2OBJ_BIN
  ci_metric tool_source external
  ci_metric tool_build_secs 0
else
  TOOL="$TARGET_DIR/release/wasm2obj"
  if [[ ! -x $TOOL || -n ${WC_FORCE:-} ]]; then
    log "building wasm2obj with cargo (cold build takes minutes)"
    T0=$SECONDS
    CARGO_TARGET_DIR="$TARGET_DIR" "$CARGO" build --release \
      --manifest-path "$WASM2OBJ_DIR/Cargo.toml" 2>&1 | tee "$CI_BUILD_DIR/cargo-build.log"
    ci_metric tool_build_secs $((SECONDS - T0))
    ci_metric tool_source built
  else
    ci_metric tool_source cached
    ci_metric tool_build_secs 0
  fi
fi
[[ -x $TOOL ]] || fail "wasm2obj binary not found: $TOOL"
ci_metric tool_path "$TOOL"

export RAYON_NUM_THREADS="$JOBS"
CODEGEN_LOG="$CI_BUILD_DIR/codegen.log"
CODEGEN_TIME="$CI_BUILD_DIR/codegen-time.txt"
log "Cranelift codegen: all defined functions, unpatched wasm (allow several minutes)"
T0=$SECONDS
if [[ -x $TIME_TOOL ]]; then
  "$TIME_TOOL" -v -o "$CODEGEN_TIME" "$TOOL" --all --out "$OBJFILE" "$WASM" 2>&1 | tee "$CODEGEN_LOG"
else
  "$TOOL" --all --out "$OBJFILE" "$WASM" 2>&1 | tee "$CODEGEN_LOG"
fi
CODEGEN_SECS=$((SECONDS - T0))
ci_metric codegen_secs "$CODEGEN_SECS"

# The tool prints phase timings with an ISO-free fixed format, e.g.
#   [t= 12.345s] compiled 154552/154552 functions in 250.234s
#   [t=260.000s] object emitted in 9.000s: 342188600 bytes -> ... (154560 functions)
num_field() {
  # usage: num_field "line substring" "exact field" "before|after" file
  local line_pat=$1 field=$2 which=$3 file=$4
  awk -v pat="$line_pat" -v field="$field" -v which="$which" '
    index($0, pat) {
      for (i = 1; i <= NF; i++) {
        if ($i == field) {
          v = (which == "after" ? $(i + 1) : $(i - 1))
          sub(/s:?$/, "", v)
          sub(/,$/, "", v)
          print v
          exit
        }
      }
    }' "$file"
}
DEFINED=$(num_field "defined functions" "defined" before "$CODEGEN_LOG")
CLIF_TRANSLATE=$(num_field "translation done" "in" after "$CODEGEN_LOG")
COMPILE_FUNCS=$(num_field "compiled " "in" after "$CODEGEN_LOG")
OBJ_PHASE=$(num_field "object emitted" "in" after "$CODEGEN_LOG")
ci_metric defined_functions "${DEFINED:-unknown}"
ci_metric clif_translate_secs "${CLIF_TRANSLATE:-unknown}"
ci_metric compile_funcs_secs "${COMPILE_FUNCS:-unknown}"
ci_metric object_phase_secs "${OBJ_PHASE:-unknown}"

if [[ -f $CODEGEN_TIME ]]; then
  RSS=$(awk -F: '/Maximum resident set size \(kbytes\)/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CODEGEN_TIME")
  ci_metric codegen_peak_rss_kb "${RSS:-unknown}"
else
  RSS=unknown
  ci_metric codegen_peak_rss_kb unknown
fi

[[ -f $OBJFILE ]] || fail "wasm2obj produced no object at $OBJFILE"
OBJ_BYTES=$(wc -c < "$OBJFILE" | tr -d ' ')
ci_metric object_bytes "$OBJ_BYTES"

# ------------------------------------------------- generated header --
# Read the object's own global symbols; never hand-write an index or guess
# an import signature from the wasm.
obj_global_symbols() {
  local obj=$1
  if "$NM" --version >/dev/null 2>&1; then
    "$NM" --defined-only --extern-only --format=posix "$obj" | awk 'NF >= 2 {print $1}'
  else
    "$NM" -gU "$obj" | awk 'NF >= 3 && $2 ~ /[TtWw]/ {print $3}'
  fi
}
SYMFILE="$CI_BUILD_DIR/obj-global-symbols.txt"
obj_global_symbols "$OBJFILE" | sort -u > "$SYMFILE"
GUEST_SYMS="$CI_BUILD_DIR/guest-symbols.txt"
BUILTIN_SYMS="$CI_BUILD_DIR/builtin-symbols.txt"
awk '/^_wasm_function_[0-9]+$/' "$SYMFILE" > "$GUEST_SYMS"
awk '/^wasmtime_(patchable_)?builtin_[A-Za-z0-9_]+$/' "$SYMFILE" > "$BUILTIN_SYMS"
FUNC_COUNT=$(wc -l < "$GUEST_SYMS" | tr -d ' ')
BUILTIN_COUNT=$(wc -l < "$BUILTIN_SYMS" | tr -d ' ')
if "$NM" --version >/dev/null 2>&1; then
  UNDEFINED=$("$NM" --undefined-only "$OBJFILE" 2>/dev/null | wc -l | tr -d ' ')
else
  UNDEFINED=unknown
fi
ci_metric function_count "$FUNC_COUNT"
ci_metric builtin_trampolines "$BUILTIN_COUNT"
ci_metric undefined_symbols "$UNDEFINED"

HEADER="$GEN/WC.h"
{
  cat <<'EOF'
/* Generated by tools/ci/cranelift-object.sh from the wasm2obj ELF symbol
 * table.  DO NOT EDIT.  status=partial: this is NOT a wasm2c-compatible ABI.
 *
 * Real declarations below: one per generated global `_wasm_function_N`
 * symbol.  These are the tool's own symbol names (N is a defined-function
 * index derived by the tool), not hand-written guest indices.  The module's
 * exported functions are among them, but wasm2obj does not emit export names
 * or C ABI trampolines (`FuncKey::ArrayToWasmTrampoline` is not compiled), so
 * a wasm2c-style named export declaration cannot be produced.
 *
 * These functions use the Wasmtime VMContext ABI: argument 0 is a VMContext
 * pointer, not a wasm2c instance pointer.
 *
 * What is missing, and therefore not fabricated here:
 *   - no wasm2c `wasm2c_rt_*` struct/init declarations: wasm2obj emits none;
 *   - no import declarations: the module's function imports are pointers in
 *     the VMContext imported-function array, not ELF undefined symbols;
 *   - no VMContext layout: generate it from the vendored vmoffsets.rs /
 *     vmctxtypes.rs and fill stack limit, memory base/bound, tables, GC heap
 *     and instance pointer before calling any generated function;
 *   - no host implementations for the builtin/libcall trampolines listed
 *     below, and no `.wasmtime.traps` signal handler.
 * tools/ci/host.sh can compile this header, but linking the v3-wasm2c C host
 * against this archive is NOT ABI-compatible.
 */
#ifndef WEBLOX_WC_H
#define WEBLOX_WC_H

#ifdef __cplusplus
extern "C" {
#endif
EOF
  if ((BUILTIN_COUNT > 0)); then
    printf '\n/* builtin trampolines present in the object (host must implement):\n'
    while IFS= read -r sym; do printf ' * %s\n' "$sym"; done < "$BUILTIN_SYMS"
    printf ' */\n'
  fi
  printf '\n'
  while IFS= read -r sym; do
    printf 'extern void %s(void *vmctx);\n' "$sym"
  done < "$GUEST_SYMS"
  cat <<'EOF'

#ifdef __cplusplus
}
#endif
#endif /* WEBLOX_WC_H */
EOF
} > "$HEADER"
# host.sh includes from $GEN; the release step publishes from $ENGINE_DIR.
cp "$HEADER" "$OUT/WC.h"

ci_metric status partial
ci_metric header_contract partial
ci_metric header_symbols "$FUNC_COUNT"
ci_metric header_missing "wasm2c_abi,import_symbols,vmctx_layout,builtin_impls,trap_handler"

# ------------------------------------------------------------- package --
log "archiving $LIB"
"$AR" rcs "$LIB" "$OBJFILE"
LIB_BYTES=$(wc -c < "$LIB" | tr -d ' ')
ci_metric lib_bytes "$LIB_BYTES"
ci_metric header_bytes "$(wc -c < "$HEADER" | tr -d ' ')"

{
  echo '{'
  echo '  "task": "cranelift-object",'
  echo '  "status": "partial",'
  echo "  \"wasm_version\": \"$WASM_VERSION\","
  echo "  \"wasm_sha256\": \"$WASM_SHA\","
  echo "  \"wabt_version\": \"$WASM2C_VERSION\","
  echo "  \"cargo_version\": \"$CARGO_VERSION\","
  echo "  \"opt\": \"$OPT\","
  echo "  \"outputs\": \"$OUTPUTS\","
  echo "  \"cache_key\": \"$KEY\","
  echo '  "library": "libweblox_engine.a",'
  echo '  "header": "WC.h",'
  echo "  \"object_bytes\": \"$OBJ_BYTES\","
  echo "  \"archive_bytes\": \"$LIB_BYTES\","
  echo "  \"function_count\": \"$FUNC_COUNT\","
  echo "  \"defined_functions\": \"${DEFINED:-unknown}\","
  echo "  \"codegen_secs\": \"$CODEGEN_SECS\","
  echo "  \"compile_funcs_secs\": \"${COMPILE_FUNCS:-unknown}\","
  echo "  \"peak_rss_kb\": \"$RSS\","
  echo "  \"wabt_translate_secs\": \"$WABT_TRANSLATE\","
  echo '  "header_contract": "partial",'
  echo '  "header_limitations": ['
  echo '    "wasm2obj emits Wasmtime-ABI _wasm_function_N symbols, not wasm2c C ABI exports",'
  echo '    "imports are VMContext function pointers, not ELF symbols: no import declarations generated",'
  echo '    "no VMContext layout, builtin implementations or trap signal handler is included"'
  echo '  ],'
  echo '  "note": "wasm2c baseline is translation only; see docs/NATIVE-AOT-FINDINGS.md for the full C compile result"'
  echo '}'
} > "$OUT/manifest.json"

# Sourced by tools/ci/host.sh; same variable names as tools/ci/engine.sh.
{
  echo "ENGINE_DIR='$OUT'"
  echo "ENGINE_LIB='$LIB'"
  echo "GEN_INCLUDE='$GEN'"
  echo "WABT_INCLUDE='$WASM2C_INCLUDE'"
  echo "WASM_VERSION='$WASM_VERSION'"
  echo "WASM_SHA256='$WASM_SHA'"
  echo "ENGINE_FORMAT='cranelift-object'"
} > "$OUT/env.sh"

printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
ci_metric engine_dir "$OUT"
ci_finish
log "done: $LIB ($LIB_BYTES bytes, $FUNC_COUNT functions, status=partial)"
