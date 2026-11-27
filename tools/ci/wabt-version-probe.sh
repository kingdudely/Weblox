#!/usr/bin/env bash
# Diagnostic: does an older WABT translate the module without OOM?
# Each attempt writes ~150 MB of C, then is discarded.  Only metrics uploaded.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

WASM=$(ci_fetch_wasm)
OUT="$CI_BUILD_DIR/probe-versions"
mkdir -p "$OUT"

for v in 1.0.36 1.0.42; do
  WABT_VERSION=$v
  WABT_ROOT=""
  WASM2C=""
  export WABT_VERSION WABT_ROOT WASM2C
  ci_fetch_wabt
  d="$OUT/$v"
  mkdir -p "$d"
  echo "[vp] === WABT $v ($WASM2C_VERSION) ===" >&2
  t0=$SECONDS
  set +e
  ( ulimit -c 0
    "$WASM2C" --enable-threads --no-debug-names --module-name=WebClient \
      --num-outputs=64 "$WASM" -o "$d/WC.c" ) \
      > "$d/stdout.log" 2> "$d/stderr.log"
  rc=$?
  set -e
  ci_metric "${v}_rc" "$rc"
  ci_metric "${v}_secs" $((SECONDS - t0))
  ci_metric "${v}_bytes" "$(cat "$d"/WC*.c 2>/dev/null | wc -c | tr -d ' ')"
  echo "[vp] $v rc=$rc secs=$((SECONDS - t0))" >&2
  sed 's/^/[vp]   /' "$d/stderr.log" | tail -8 >&2
  rm -rf "$d"
done
ci_finish
