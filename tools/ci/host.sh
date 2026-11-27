#!/usr/bin/env bash
# STAGE B (cheap, every edit): compile the host and link against the cached
# engine library produced by engine.sh.  The 100 MB wasm is never touched.
#
#   WC_HOST_SRCS="host/main.c host/imports.c ..." tools/ci/host.sh
#
# With no WC_HOST_SRCS, auto-discovers the parked v3-wasm2c host sources if
# they exist; otherwise it just verifies that the generated header compiles
# and reports what to supply.  That keeps this script useful before the host
# has been ported to the generated-C ABI.
#
# Env: WC_ENGINE_DIR, WC_HOST_SRCS, WC_HOST_OUT, WC_LAX=1, CC, WC_BUILD_DIR.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

ENGINE_DIR=${WC_ENGINE_DIR:-}
if [[ -z $ENGINE_DIR && -f $CI_BUILD_DIR/engine_dir ]]; then
  ENGINE_DIR=$(cat "$CI_BUILD_DIR/engine_dir")
fi
if [[ -z $ENGINE_DIR || ! -f $ENGINE_DIR/env.sh ]]; then
  echo "[host] no engine library found - run tools/ci/engine.sh first" >&2
  exit 2
fi
# shellcheck source=/dev/null
. "$ENGINE_DIR/env.sh"
ci_metric engine_dir "$ENGINE_DIR"
ci_metric wasm_sha256 "$WASM_SHA256"

CC=${CC:-cc}
CFLAGS="-w -fno-asynchronous-unwind-tables -I$GEN_INCLUDE -I$WABT_INCLUDE"
HOST_OUT=${WC_HOST_OUT:-"$CI_BUILD_DIR/host/weblox"}
mkdir -p "$(dirname "$HOST_OUT")" "$CI_BUILD_DIR/host/obj"

# Always prove the public ABI is usable, even without host sources.
echo '[host] checking generated header'
printf '#include "WC.h"\nint main(void){return 0;}\n' \
  | "$CC" -x c - $CFLAGS -o "$CI_BUILD_DIR/host/header_check" -lm -lpthread
ci_metric header_check ok

if [[ -n ${WC_HOST_SRCS:-} ]]; then
  read -r -a SRCS <<< "${WC_HOST_SRCS//,/ }"
else
  SRCS=()
  for glob in "$CI_ROOT/v3-wasm2c/host/"*.c \
              "$CI_ROOT/v3-wasm2c/impl/"*.c \
              "$CI_ROOT/v3-wasm2c/harness.c"; do
    for f in $glob; do [[ -f $f ]] && SRCS+=("$f"); done
  done
fi

if [[ ${#SRCS[@]} -eq 0 ]]; then
  echo "[host] no host sources supplied; header check only" >&2
  echo "[host] set WC_HOST_SRCS='file.c ...' to build an executable" >&2
  ci_metric mode header_check
  ci_finish
  exit 0
fi

echo "[host] compiling ${#SRCS[@]} host files"
OBJS=()
for f in "${SRCS[@]}"; do
  o="$CI_BUILD_DIR/host/obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
  if [[ ! -f $o || $f -nt $o ]]; then
    "$CC" -c $CFLAGS "$f" -o "$o"
  fi
  OBJS+=("$o")
done

LDFLAGS=""
[[ -n ${WC_LAX:-} ]] && LDFLAGS="$LDFLAGS -Wl,--unresolved-symbols=ignore-all"
"$CC" "${OBJS[@]}" "$ENGINE_LIB" $LDFLAGS -o "$HOST_OUT" -lm -lpthread
ci_metric mode executable
ci_metric host_bytes "$(wc -c < "$HOST_OUT" | tr -d ' ')"
ci_metric host_binary "$HOST_OUT"
ci_finish
echo "[host] done: $HOST_OUT"
