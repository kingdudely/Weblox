#!/usr/bin/env bash
# STAGE A (expensive, rare): wasm -> generated C -> libweblox_engine.a
#
# This is the only step that touches the 100 MB module.  It runs on GitHub
# Actions or locally, is keyed by a hash of (wasm, tool version, flags, this
# script), and produces three small-to-medium release assets:
#
#   libweblox_engine.a   the whole translated module, no host code
#   WC.h                 generated public header (imports/exports/instance)
#   manifest.json        paths, hashes, versions, timings
#
# Host work never rebuilds this.  See docs/BUILD-STRATEGY.md.
#
# Env: WC_WASM, WASM_VERSION, WASM_SHA256, WABT_VERSION, WABT_ROOT, WASM2C,
#      SIMDE_DIR, CC, WC_OPT (O0/O1/O2/O3), WC_OUTPUTS (split count),
#      WC_FORCE=1, WC_JOBS, WC_BUILD_DIR.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

WASM=$(ci_fetch_wasm)
ci_fetch_wabt

OPT=${WC_OPT:-O1}
OFLAG="-${OPT#-}"
OUTPUTS=${WC_OUTPUTS:-512}
JOBS=${WC_JOBS:-$(ci_nproc)}
CC=${CC:-${WC_CC:-}}
if [[ -z $CC ]]; then
  CC=$(command -v clang >/dev/null 2>&1 && echo clang || echo cc)
fi
# ccache makes recompiles of an unchanged generated tree cheap.  A tiny
# wrapper keeps CC a single executable so the xargs calls stay quoted.
if command -v ccache >/dev/null 2>&1 && [[ -z ${WC_NO_CCACHE:-} ]]; then
  export CCACHE_DIR="$CI_BUILD_DIR/ccache"
  WRAP="$CI_BUILD_DIR/cc-$RANDOM"
  printf '#!/bin/sh\nexec ccache %s "$@"\n' "$CC" > "$WRAP"
  chmod +x "$WRAP"
  CC="$WRAP"
fi
CFLAGS="-w -fno-asynchronous-unwind-tables -fno-stack-protector -DWASM_RT_STACK_DEPTH_COUNT=1"
[[ ${SIMDE_DIR:-} ]] && CFLAGS="$CFLAGS -I$SIMDE_DIR"

ci_metric wasm_sha256 "$(ci_sha256 "$WASM")"
ci_metric wasm_bytes "$(wc -c < "$WASM" | tr -d ' ')"
ci_metric opt "$OPT"
ci_metric outputs "$OUTPUTS"
ci_metric jobs "$JOBS"
ci_metric compiler "$("$CC" --version 2>/dev/null | head -1)"

# Cache key: anything that can change the generated code must be in here.
DESC="$CI_BUILD_DIR/engine-key.txt"
printf '%s\n' "$(ci_sha256 "$WASM")" "$WASM2C_VERSION" "$OPT" "$OUTPUTS" \
  "$CFLAGS" "$(ci_sha256 "$HERE/engine.sh")" > "$DESC"
KEY=$(ci_sha256 "$DESC")
KEY=${KEY:0:16}

OUT="$CI_BUILD_DIR/engine/$KEY"
GEN="$OUT/gen"
OBJ="$OUT/obj"
LIB="$OUT/libweblox_engine.a"

if [[ -f $LIB && -f $GEN/WC.h && -z ${WC_FORCE:-} ]]; then
  echo "[engine] cached: $LIB"
  ci_metric cached 1
  ci_metric engine_dir "$OUT"
  printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
  ci_finish
  exit 0
fi
ci_metric cached 0
rm -rf "$OUT"
mkdir -p "$GEN" "$OBJ"

FLAGS=$(ci_wasm2c_flags "$WASM2C")
ci_metric wasm2c_flags "$FLAGS"

echo "[engine] translating (this takes minutes)"
T0=$SECONDS
"$WASM2C" $FLAGS --module-name=WebClient --num-outputs="$OUTPUTS" "$WASM" -o "$GEN/WC.c"
ci_metric translate_secs $((SECONDS - T0))

# WABT --num-outputs names the extra TUs WC-1.c, WC-2.c, ... ; older
# splitters used WC_1.c.  Accept either and never hardcode a count.
TRANSLATED=()
while IFS= read -r f; do TRANSLATED+=("$f"); done < <(find "$GEN" -maxdepth 1 -name 'WC*.c' -print | sort)
ci_metric translated_tus "${#TRANSLATED[@]}"
ci_metric translated_bytes "$(cat "${TRANSLATED[@]}" | wc -c | tr -d ' ')"

# WC-impl.h is a splitter artifact in the parked v3 tree.  Stock WABT does not
# emit it; create an empty compatibility header so old host sources still build.
[[ -f $GEN/WC-impl.h ]] || : > "$GEN/WC-impl.h"

echo "[engine] compiling ${#TRANSLATED[@]} translation units with $JOBS jobs"
T0=$SECONDS
export WC_OBJ_DIR="$OBJ" WC_CC_BIN="$CC" WC_GEN_DIR="$GEN" WC_WABT_INC="$WASM2C_INCLUDE" \
       WC_OFLAG="$OFLAG"
printf '%s\n' "${TRANSLATED[@]}" | xargs -P "$JOBS" -I{} sh -c '
  f="$1"
  o="$WC_OBJ_DIR/$(basename "$f" .c).o"
  [ -f "$o" ] || "$WC_CC_BIN" -c '"$WC_OFLAG $CFLAGS"' \
    -I"$WC_GEN_DIR" -I"$WC_WABT_INC" "$f" -o "$o"
' sh {}
ci_metric compile_secs $((SECONDS - T0))

# WABT runtime support belongs in the engine library, not the host.
RUNTIME_SRCS=()
for f in "$WASM2C_RUNTIME_DIR"/wasm-rt*.c; do
  [[ -f $f ]] && RUNTIME_SRCS+=("$f")
done
ci_metric runtime_sources "${#RUNTIME_SRCS[@]}"
for f in "${RUNTIME_SRCS[@]}"; do
  o="$OBJ/rt-$(basename "$f" .c).o"
  "$CC" -c -w -DWASM_RT_STACK_DEPTH_COUNT=1 -I"$WASM2C_INCLUDE" -I"$GEN" \
    "$f" -o "$o"
done

echo "[engine] archiving $LIB"
AR=${AR:-ar}
"$AR" rcs "$LIB" "$OBJ"/*.o
ci_metric lib_bytes "$(wc -c < "$LIB" | tr -d ' ')"
ci_metric header_bytes "$(wc -c < "$GEN/WC.h" | tr -d ' ')"

# Small, self-describing manifest so a host can consume the library without
# knowing the cache key or the tool versions used to build it.
MANIFEST="$OUT/manifest.json"
{
  echo '{'
  echo "  \"wasm_version\": \"$WASM_VERSION\","
  echo "  \"wasm_sha256\": \"$(ci_sha256 "$WASM")\","
  echo "  \"wabt_version\": \"$WASM2C_VERSION\","
  echo "  \"opt\": \"$OPT\","
  echo "  \"outputs\": \"$OUTPUTS\","
  echo "  \"library\": \"libweblox_engine.a\","
  echo "  \"header\": \"WC.h\","
  echo "  \"runtime_sources\": \"${#RUNTIME_SRCS[@]}\""
  echo '}'
} > "$MANIFEST"

# Sourced by Stage B so the host never has to guess paths or tool versions.
ENVFILE="$OUT/env.sh"
{
  echo "ENGINE_DIR='$OUT'"
  echo "ENGINE_LIB='$LIB'"
  echo "GEN_INCLUDE='$GEN'"
  echo "WABT_INCLUDE='$WASM2C_INCLUDE'"
  echo "WASM_VERSION='$WASM_VERSION'"
  echo "WASM_SHA256='$(ci_sha256 "$WASM")'"
} > "$ENVFILE"

ci_metric engine_dir "$OUT"
printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
ci_finish
echo "[engine] done: $LIB"
