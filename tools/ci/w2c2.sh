#!/usr/bin/env bash
# Benchmark turbolent/w2c2 on the pinned *stock* engine wasm.
#
# The success path mirrors the output contract of tools/ci/engine.sh (WABT
# wasm2c): $CI_BUILD_DIR/engine/<key>/ gets libweblox_engine.a, WC.h,
# manifest.json and env.sh, and build/ci/engine_dir points at that directory.
# The translator is fetched and built *inside the runner* (turbolent/w2c2 has
# no binary releases; the crates.io "w2c2" crate is an unrelated Rust wrapper
# with no CLI).  Generated C, objects and the translator are never committed.
#
# EH check: this module uses exnref / try_table.  The translator sources are
# probed for exception-handling support and the stock wasm is translated for
# real.  If EH support is missing, or translation/compilation fails, the
# script reports status=blocked with a clear blocker= and produces NO library;
# metrics are still written so the workflow uploads them.
#
# Env: WC_OPT, WC_OUTPUTS (w2c2 -f: functions per output file), WC_JOBS,
#      CC, AR, WC_BUILD_DIR, WC_WASM, WASM_VERSION, WASM_SHA256,
#      W2C2_REF, W2C2_BIN, SIMDE_REF, WC_FORCE.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

# turbolent/w2c2 main as of 2026-11-11; there are no tagged releases to pin.
W2C2_REF=${W2C2_REF:-bc3fd9ac23ea176555e2530503fffed13de48228}
SIMDE_REF=${SIMDE_REF:-v0.8.2}
OPT=${WC_OPT:-O0}
OUTPUTS=${WC_OUTPUTS:-512}
JOBS=${WC_JOBS:-$(ci_nproc)}
CC=${CC:-${WC_CC:-cc}}
AR=${AR:-ar}

ci_metric task w2c2
ci_metric wasm_version "$WASM_VERSION"
ci_metric w2c2_ref "$W2C2_REF"
ci_metric opt "$OPT"
ci_metric outputs "$OUTPUTS"
ci_metric functions_per_file "$OUTPUTS"
ci_metric jobs "$JOBS"
ci_metric compiler "$("$CC" --version 2>/dev/null | sed -n '1p' || true)"

# Keep metrics.txt strictly key=value, one bounded line per metric.
oneline() { printf '%s' "$1" | tr '\r\n' '  ' | cut -c1-600; }

blocked() {
  ci_metric status blocked
  ci_metric blocker "$(oneline "$1")"
  [[ -n ${2:-} ]] && ci_metric translate_error "$(oneline "$2")"
  echo "[w2c2] BLOCKED: $1" >&2
  ci_finish
  exit 0
}

echo "[w2c2] fetching pinned engine wasm ($WASM_VERSION)"
WASM=$(ci_fetch_wasm) || blocked "cannot fetch pinned wasm: download failed or sha256 mismatch" ""
WASM_SHA=$(ci_sha256 "$WASM")
ci_metric wasm_sha256 "$WASM_SHA"
ci_metric wasm_bytes "$(wc -c < "$WASM" | tr -d ' ')"

# Anything that changes the generated code belongs in the cache key.
DESC="$CI_BUILD_DIR/w2c2-key.txt"
printf '%s\n' "$WASM_SHA" "$W2C2_REF" "$OPT" "$OUTPUTS" \
  "$(ci_sha256 "$HERE/w2c2.sh")" > "$DESC"
KEY=$(ci_sha256 "$DESC")
KEY=${KEY:0:16}
OUT="$CI_BUILD_DIR/engine/w2c2-$KEY"
GEN="$OUT/gen"
OBJ="$OUT/obj"
mkdir -p "$GEN" "$OBJ"

# --- obtain / build the translator ---------------------------------------
W2C2_INC=${W2C2_SRC:-}
BIN=${W2C2_BIN:-}
if [[ -z $BIN ]]; then
  SRC="$CI_BUILD_DIR/w2c2-src"
  if [[ -z $W2C2_INC ]]; then
    TARBALL="$CI_BUILD_DIR/w2c2-$W2C2_REF.tar.gz"
    if [[ ! -f $TARBALL ]]; then
      echo "[w2c2] fetching translator source at $W2C2_REF"
      ci_download "https://github.com/turbolent/w2c2/archive/$W2C2_REF.tar.gz" "$TARBALL" \
        || blocked "cannot fetch turbolent/w2c2 source at $W2C2_REF" ""
    fi
    ci_metric w2c2_source_sha256 "$(ci_sha256 "$TARBALL")"
    if [[ ! -d $SRC/w2c2 ]]; then
      mkdir -p "$SRC"
      tar -xzf "$TARBALL" -C "$SRC" --strip-components=1 \
        || blocked "cannot unpack turbolent/w2c2 source" ""
    fi
    W2C2_INC="$SRC/w2c2"
  else
    SRC=$(cd "$W2C2_INC/.." && pwd)
    ci_metric w2c2_source local
  fi

  echo "[w2c2] building translator"
  T0=$SECONDS
  BLDLOG="$CI_BUILD_DIR/w2c2-build.log"
  if command -v cmake >/dev/null 2>&1; then
    ( cmake -S "$W2C2_INC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
      && cmake --build "$SRC/build" --target w2c2 --parallel "$JOBS" \
    ) > "$BLDLOG" 2>&1 \
      || blocked "w2c2 build failed (see w2c2-build.log)" "$(tail -n 8 "$BLDLOG" 2>/dev/null || true)"
  else
    make -C "$W2C2_INC" -j"$JOBS" > "$BLDLOG" 2>&1 \
      || blocked "w2c2 build failed (see w2c2-build.log)" "$(tail -n 8 "$BLDLOG" 2>/dev/null || true)"
  fi
  ci_metric tool_build_secs $((SECONDS - T0))

  if [[ -x $SRC/build/w2c2 ]]; then
    BIN="$SRC/build/w2c2"
  elif [[ -x $SRC/build/w2c2/w2c2 ]]; then
    BIN="$SRC/build/w2c2/w2c2"
  elif [[ -x $W2C2_INC/w2c2 ]]; then
    BIN="$W2C2_INC/w2c2"
  else
    blocked "w2c2 build produced no executable" "$(tail -n 8 "$BLDLOG" 2>/dev/null || true)"
  fi
fi
ci_metric w2c2_bin "$BIN"
HELP_OUT=$("$BIN" -h 2>&1 || true)
W2C2_VERSION=$(printf '%s\n' "$HELP_OUT" | sed -n '1p')
ci_metric w2c2_version "$W2C2_VERSION"

# --- honest EH capability probe -------------------------------------------
# A presence check, not a fake feature flag: tokens must exist in the actual
# translator sources (host code, not the test suite) to claim EH support.
EH_SUPPORTED=1
if [[ -n $W2C2_INC ]]; then
  EH_HITS=$(grep -RIlE 'exnref|try_table|catch_all_ref|throw_ref|wasmOpcodeTryTable|wasmOpcodeThrow|tag section' \
    "$W2C2_INC" 2>/dev/null | grep -vE '_test\.c$' | tr '\n' ' ' || true)
  if [[ -n $EH_HITS ]]; then
    ci_metric eh_support 1
    ci_metric eh_evidence "$(oneline "$EH_HITS")"
  else
    EH_SUPPORTED=0
    ci_metric eh_support 0
  fi
else
  ci_metric eh_support unknown
fi

# --- translate the unmodified wasm ----------------------------------------
FLAGS=(-f "$OUTPUTS")
# -t only exists in pthread-enabled builds; probe instead of assuming it.
if [[ $HELP_OUT == *"-t N"* ]]; then FLAGS+=(-t "$JOBS"); fi
ci_metric w2c2_flags "${FLAGS[*]}"
echo "[w2c2] translating ($OUTPUTS functions/file, $JOBS threads; takes minutes)"
T0=$SECONDS
set +e
"$BIN" "${FLAGS[@]}" "$WASM" "$GEN/WC.c" > "$OUT/translate.log" 2>&1
RC=$?
set -e
ci_metric translate_secs $((SECONDS - T0))
ci_metric translate_exit_code "$RC"
ERR=$(grep -iE 'error|unsupported|unknown|unimplemented|exception|exnref|try_table|tag ' \
  "$OUT/translate.log" 2>/dev/null | tail -n 8 || true)
if [[ -z $ERR ]]; then
  ERR=$(tail -n 12 "$OUT/translate.log" 2>/dev/null || true)
fi
ci_metric translate_error "$(oneline "$ERR")"
# Keep the translator's own diagnostic in the run log the workflow uploads.
echo "[w2c2] translator output (last 12 lines):" >&2
tail -n 12 "$OUT/translate.log" >&2 || true

# Count whatever was emitted, even on failure, so the cost is visible.
TRANSLATED=()
while IFS= read -r f; do TRANSLATED+=("$f"); done < <(find "$GEN" -maxdepth 1 -type f -name '*.c' -print | LC_ALL=C sort)
ci_metric translated_tus "${#TRANSLATED[@]}"
GEN_BYTES=0
if ((${#TRANSLATED[@]})); then
  GEN_BYTES=$(cat "${TRANSLATED[@]}" | wc -c | tr -d ' ')
fi
ci_metric translated_bytes "$GEN_BYTES"

if [[ $EH_SUPPORTED -ne 1 ]]; then
  blocked "turbolent/w2c2 $W2C2_REF has no WebAssembly exception-handling support: no exnref/try_table/throw_ref/catch_*_ref handling in translator sources; the pinned WebClient.wasm uses exnref/try_table and cannot be translated faithfully (translation exit $RC)" "$ERR"
fi
[[ $RC -eq 0 ]] || blocked "w2c2 translation failed on the pinned WebClient.wasm (exit $RC)" "$ERR"

HEADER="$GEN/WC.h"
if [[ ! -f $HEADER ]]; then
  ALT=$(find "$GEN" -maxdepth 1 -type f -name '*.h' -print | LC_ALL=C sort | sed -n '1p')
  [[ -n $ALT ]] && HEADER=$ALT
fi
[[ -f $HEADER ]] || blocked "w2c2 produced no generated header" "$ERR"

# --- compile generated C with SIMDe ---------------------------------------
SIMDE="$CI_BUILD_DIR/simde-$SIMDE_REF"
if [[ ! -d $SIMDE/simde ]]; then
  echo "[w2c2] fetching SIMDe $SIMDE_REF"
  STARBALL="$CI_BUILD_DIR/simde-$SIMDE_REF.tar.gz"
  [[ -f $STARBALL ]] || ci_download \
    "https://github.com/simd-everywhere/simde/archive/refs/tags/$SIMDE_REF.tar.gz" "$STARBALL" \
    || blocked "cannot fetch SIMDe $SIMDE_REF" ""
  mkdir -p "$SIMDE"
  tar -xzf "$STARBALL" -C "$SIMDE" --strip-components=1 \
    || blocked "cannot unpack SIMDe $SIMDE_REF" ""
fi
ci_metric simde_ref "$SIMDE_REF"

# Helper takes paths/flags as arguments, so no generated C or binary is ever
# written into the repo and no quoting trickery is needed inside xargs.
COMPILE_SH="$OUT/compile-one.sh"
cat > "$COMPILE_SH" <<'EOF'
#!/bin/sh
set -e
f=$1
objdir=$2
cc=$3
opt=$4
inc=$5
simde=$6
gen=$7
o="$objdir/$(basename "$f" .c).o"
exec "$cc" -c "-$opt" -std=c99 -w -DW2C2_RUNTIME_CHECKS=0 \
  -I"$gen" -I"$inc" -I"$simde" \
  "-DW2C2_SIMDE_HEADER=\"$simde/simde/wasm/simd128.h\"" \
  "$f" -o "$o"
EOF
chmod +x "$COMPILE_SH"

echo "[w2c2] compiling ${#TRANSLATED[@]} generated TUs with $JOBS jobs"
T0=$SECONDS
if ! printf '%s\n' "${TRANSLATED[@]}" \
  | xargs -P "$JOBS" -I{} "$COMPILE_SH" {} "$OBJ" "$CC" "$OPT" "$W2C2_INC" "$SIMDE" "$GEN"; then
  blocked "compilation of w2c2-generated C failed" "$(find "$OBJ" -name '*.o' -print | wc -l | tr -d ' ') objects produced"
fi
ci_metric compile_secs $((SECONDS - T0))

LIB="$OUT/libweblox_engine.a"
"$AR" rcs "$LIB" "$OBJ"/*.o
ci_metric lib_bytes "$(wc -c < "$LIB" | tr -d ' ')"
cp "$HEADER" "$OUT/WC.h"
ci_metric header_bytes "$(wc -c < "$OUT/WC.h" | tr -d ' ')"

# Self-describing manifest + env file, matching the Stage A contract.
{
  echo '{'
  echo "  \"wasm_version\": \"$WASM_VERSION\","
  echo "  \"wasm_sha256\": \"$WASM_SHA\","
  echo "  \"translator\": \"turbolent/w2c2\","
  echo "  \"translator_ref\": \"$W2C2_REF\","
  echo "  \"w2c2_version\": \"$W2C2_VERSION\","
  echo "  \"opt\": \"$OPT\","
  echo "  \"functions_per_file\": \"$OUTPUTS\","
  echo "  \"translated_tus\": \"${#TRANSLATED[@]}\","
  echo "  \"translated_bytes\": \"$GEN_BYTES\","
  echo "  \"library\": \"libweblox_engine.a\","
  echo "  \"header\": \"WC.h\""
  echo '}'
} > "$OUT/manifest.json"

{
  echo "ENGINE_DIR='$OUT'"
  echo "ENGINE_LIB='$LIB'"
  echo "GEN_INCLUDE='$GEN'"
  echo "W2C2_INCLUDE='$W2C2_INC'"
  echo "SIMDE_DIR='$SIMDE'"
  echo "WASM_VERSION='$WASM_VERSION'"
  echo "WASM_SHA256='$WASM_SHA'"
} > "$OUT/env.sh"

ci_metric status ok
ci_metric engine_dir "$OUT"
printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
ci_finish
echo "[w2c2] done: $LIB"
