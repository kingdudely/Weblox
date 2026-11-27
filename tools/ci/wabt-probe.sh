#!/usr/bin/env bash
# Diagnostic: find a wasm2c output layout that survives on the CI runner.
# Nothing is uploaded; only exit codes, timing and peak RSS are recorded.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

WASM=$(ci_fetch_wasm)
ci_fetch_wabt
FLAGS=$(ci_wasm2c_flags "$WASM2C")
ci_metric flags "$FLAGS"

PROBE_DIR="$CI_BUILD_DIR/probe"
mkdir -p "$PROBE_DIR"

for n in 1 64 512; do
  out="$PROBE_DIR/n$n"
  mkdir -p "$out"
  echo "[probe] num-outputs=$n" >&2
  T0=$SECONDS
  set +e
  ( ulimit -c 0
    "$WASM2C" $FLAGS --module-name=WebClient --num-outputs="$n" \
      "$WASM" -o "$out/WC.c" ) > "$out/stdout.log" 2> "$out/stderr.log"
  rc=$?
  set -e
  ci_metric "n${n}_secs" $((SECONDS - T0))
  ci_metric "n${n}_rc" "$rc"
  ci_metric "n${n}_tus" "$(find "$out" -maxdepth 1 -name 'WC*.c' | wc -l | tr -d ' ')"
  ci_metric "n${n}_bytes" "$(cat "$out"/WC*.c 2>/dev/null | wc -c | tr -d ' ')"
  echo "[probe] n=$n rc=$rc secs=$((SECONDS - T0))" >&2
  tail -5 "$out/stderr.log" | sed 's/^/[probe]   /' >&2
  if [[ $rc -eq 0 ]]; then
    ci_metric survivor "$n"
    break
  fi
  rm -rf "$out"
done
ci_finish
