#!/usr/bin/env bash
# Compile the pre-generated wasm2c C tree from a Release into the standard
# Stage A library contract.  Skips translation entirely; useful when the
# runner cannot fit WABT's WriteC but a generated tree already exists.
#
# Env: GENERATED_RELEASE (default ci-local-wasm2c-generated), WC_OPT,
#      WC_JOBS, SIMDE_DIR, WC_BUILD_DIR.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
# shellcheck source=platform.sh
. "$HERE/platform.sh"
ci_init
ci_metric platform "$PLATFORM"

RELEASE=${GENERATED_RELEASE:-ci-local-wasm2c-generated}
GEN="$CI_BUILD_DIR/generated"
OBJ="$CI_BUILD_DIR/generated-obj"
OUT="$CI_BUILD_DIR/engine/generated-$RELEASE"
mkdir -p "$GEN" "$OBJ" "$OUT"

ci_fetch_wabt

if [[ ! -f $GEN/WC.h ]]; then
  echo "[gen] fetching generated C from release $RELEASE" >&2
  if command -v gh >/dev/null 2>&1; then
    gh release download "$RELEASE" --repo "${ENGINE_REPO:-kingdudely/Weblox}" --dir "$GEN" \
      --pattern 'WC*.c' --pattern 'WC.h' --pattern 'WC-impl.h'
  else
    API="https://api.github.com/repos/kingdudely/Weblox/releases/tags/$RELEASE"
    curl -fL --retry 3 "$API" | grep -o '"browser_download_url": *"[^"]*"' \
      | cut -d'"' -f4 | while read -r url; do
          case "$url" in
            *WC*.c|*WC.h|*WC-impl.h) ci_download "$url" "$GEN/$(basename "$url")" ;;
          esac
        done
  fi
fi

SIMDE_DIR=${SIMDE_DIR:-"$CI_BUILD_DIR/simde"}
if [[ ! -f $SIMDE_DIR/simde/wasm/simd128.h ]]; then
  echo "[gen] fetching SIMDE" >&2
  mkdir -p "$SIMDE_DIR"
  curl -fL --retry 3 -o "$CI_BUILD_DIR/simde.tar.gz" \
    https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz
  tar -xzf "$CI_BUILD_DIR/simde.tar.gz" -C "$SIMDE_DIR" --strip-components=1
fi

OPT=${WC_OPT:-O0}
OFLAG="-${OPT#-}"
JOBS=${WC_JOBS:-$(ci_nproc)}
CC=${CC:-${WC_CC:-}}
if [[ -z $CC ]]; then
  CC=$(command -v clang >/dev/null 2>&1 && echo clang || echo cc)
fi
CC_BASE=$CC
if command -v ccache >/dev/null 2>&1 && [[ -z ${WC_NO_CCACHE:-} ]]; then
  export CCACHE_DIR="$CI_BUILD_DIR/ccache"
  WRAP="$CI_BUILD_DIR/cc-$RANDOM"
  printf '#!/bin/sh\nexec ccache %s "$@"\n' "$CC" > "$WRAP"
  chmod +x "$WRAP"
  CC="$WRAP"
fi
CFLAGS="-w -Wno-error=incompatible-pointer-types -Wno-error=int-conversion \
  -Wno-error=pointer-sign -Wno-error=builtin-declaration-mismatch \
  -fno-asynchronous-unwind-tables -fno-stack-protector $PIC_FLAG \
  -I$GEN -I$WASM2C_INCLUDE -I$SIMDE_DIR"

# Every generated TU includes the same ~9 MB WC-impl.h plus WABT/SIMDE headers.
# A precompiled header avoids re-parsing that 32 times.  clang supports
# -include-pch; gcc falls back to a plain -include.
PCH_FLAGS=""
if [[ $CC_BASE == *clang* ]]; then
  PRE="$GEN/weblox-pre.h"
  {
    echo '#include "wasm-rt.h"'
    echo '#include "wasm-rt-exceptions.h"'
    echo '#include "WC.h"'
    # WC-impl.h has no include guard: each generated TU includes it itself,
    # so putting it in the PCH would cause redefinitions.
  } > "$PRE"
  if "$CC_BASE" -x c-header $CFLAGS "$PRE" -o "$GEN/weblox-pre.h.pch" 2>/dev/null; then
    PCH_FLAGS="-include-pch $GEN/weblox-pre.h.pch"
    ci_metric pch 1
  fi
fi
[[ -n $PCH_FLAGS ]] || ci_metric pch 0

TRANSLATED=()
while IFS= read -r f; do TRANSLATED+=("$f"); done < <(find "$GEN" -maxdepth 1 -name 'WC_*.c' -print | sort)
ci_metric translated_tus "${#TRANSLATED[@]}"
ci_metric translated_bytes "$(cat "${TRANSLATED[@]}" | wc -c | tr -d ' ')"
[[ ${#TRANSLATED[@]} -gt 0 ]] || { echo "[gen] no generated C found" >&2; exit 2; }

echo "[gen] compiling ${#TRANSLATED[@]} generated units with $JOBS jobs" >&2
T0=$SECONDS
export WC_OBJ_DIR="$OBJ" WC_CC_BIN="$CC" WC_GEN_DIR="$GEN" \
       WC_WABT_INC="$WASM2C_INCLUDE" WC_SIMDE_INC="$SIMDE_DIR" WC_OFLAG="$OFLAG" \
       WC_PCH="$PCH_FLAGS"
printf '%s\n' "${TRANSLATED[@]}" | xargs -P "$JOBS" -I{} sh -c '
  f="$1"
  o="$WC_OBJ_DIR/$(basename "$f" .c).o"
  [ -f "$o" ] || "$WC_CC_BIN" -c '"$WC_OFLAG $CFLAGS $WC_PCH"' \
    -I"$WC_GEN_DIR" -I"$WC_WABT_INC" -I"$WC_SIMDE_INC" "$f" -o "$o"
' sh {}
ci_metric compile_secs $((SECONDS - T0))

for f in "$WASM2C_RUNTIME_DIR"/wasm-rt*.c; do
  [[ -f $f ]] || continue
  "$CC" -c -w $PIC_FLAG -I"$WASM2C_INCLUDE" -I"$GEN" -I"$SIMDE_DIR" "$f" -o "$OBJ/rt-$(basename "$f" .c).o"
done

SHARED="$OUT/$SHARED_NAME"
IMPORT=""
if [[ $PLAT_OS == windows ]]; then
  # No edits to generated C: derive exports from the compiled objects.
  DEF="$OUT/exports.def"
  printf 'EXPORTS\n' > "$DEF"
  # PE export tables cap at 65535 symbols; the module has 170853 functions.
  # WC.h declares only the host-visible ABI (~57 names), which is what a host
  # actually calls.  Derive exports from that header, not from every object.
  grep -oE '\b(w2c_WebClient_[A-Za-z0-9_]+|wasm2c_WebClient_[A-Za-z0-9_]+)\b' \
    "$GEN/WC.h" | sort -u >> "$DEF"
  IMPORT="$OUT/$IMPORT_NAME"
  # Use the clang driver so the CRT libraries are added automatically.  Host
  # imports are resolved by the executable, so force unresolved externals.
  # The - spelling avoids Git Bash rewriting /DEF: into a Windows path.
  "$CC" -shared -fuse-ld=lld -Wl,-FORCE -Wl,-DEF:"$DEF" \
    -Wl,-IMPLIB:"$IMPORT" -o "$SHARED" "$OBJ"/*.o
else
  # Generated objects call host imports that live in the executable.  Linux
  # .so allows undefined symbols by default; macOS needs dynamic_lookup.
  UNDEFINED_FLAG=""
  LLD_FLAG=""
  [[ $PLAT_OS == macos ]] && UNDEFINED_FLAG="-Wl,-undefined,dynamic_lookup"
  [[ $PLAT_OS == linux ]] && command -v ld.lld >/dev/null 2>&1 && LLD_FLAG="-fuse-ld=lld"
  "$CC" $SHARED_FLAG $UNDEFINED_FLAG $LLD_FLAG -o "$SHARED" "$OBJ"/*.o -lm -lpthread
fi
ci_metric shared_bytes "$(wc -c < "$SHARED" | tr -d ' ')"

cp "$GEN/WC.h" "$OUT/WC.h"
cp "$GEN/WC-impl.h" "$OUT/WC-impl.h" 2>/dev/null || true
if command -v zstd >/dev/null 2>&1; then
  # One tar.zst per platform so CMake FetchContent can download + extract it.
  BUNDLE="$OUT/engine-$PLATFORM.tar.zst"
  if [[ -n $IMPORT ]]; then
    (cd "$OUT" && tar -cf - "$SHARED_NAME" "$(basename "$IMPORT")") \
      | zstd -T0 -19 -q > "$BUNDLE"
  else
    (cd "$OUT" && tar -cf - "$SHARED_NAME") | zstd -T0 -19 -q > "$BUNDLE"
  fi
  ci_metric zstd_bytes "$(wc -c < "$BUNDLE" | tr -d ' ')"
fi
{
  echo '{'
  echo "  \"task\": \"compile-generated\","
  echo "  \"release\": \"$RELEASE\","
  echo "  \"generated_tus\": \"${#TRANSLATED[@]}\","
  echo "  \"compile_secs\": \"$((SECONDS - T0))\","
  echo "  \"platform\": \"$PLATFORM\","
  echo "  \"shared\": \"$SHARED_NAME\","
  echo "  \"header\": \"WC.h\""
  echo '}'
} > "$OUT/manifest.json"
{
  echo "ENGINE_DIR='$OUT'"
  echo "ENGINE_LIB='$SHARED'"
  echo "ENGINE_SO='$SHARED'"
  echo "ENGINE_IMPORT='$IMPORT'"
  echo "GEN_INCLUDE='$OUT'"
  echo "WABT_INCLUDE='$WASM2C_INCLUDE'"
  echo "WASM_VERSION='$WASM_VERSION'"
  echo "WASM_SHA256='$WASM_SHA256'"
} > "$OUT/env.sh"

ci_metric engine_dir "$OUT"
printf '%s\n' "$OUT" > "$CI_BUILD_DIR/engine_dir"
ci_finish
echo "[gen] done: $SHARED" >&2
