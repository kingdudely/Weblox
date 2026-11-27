#!/usr/bin/env bash
# Diagnostic: find a wasm2c flag combination that survives on the CI runner.
# Each attempt is short (~12 s).  Stderr tails go to stdout so they land in
# the Actions artifact's run.log.  Nothing is uploaded as a library.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

WASM=$(ci_fetch_wasm)
ci_fetch_wabt

PROBE_DIR="$CI_BUILD_DIR/probe"
mkdir -p "$PROBE_DIR"

attempt() {
  local name=$1; shift
  local out="$PROBE_DIR/$name"
  mkdir -p "$out"
  echo "[probe] === $name: wasm2c $* ===" >&2
  local t0=$SECONDS rc
  set +e
  ( ulimit -c 0
    "$WASM2C" "$@" --module-name=WebClient --num-outputs=1 \
      "$WASM" -o "$out/WC.c" ) > "$out/stdout.log" 2> "$out/stderr.log"
  rc=$?
  set -e
  ci_metric "${name}_rc" "$rc"
  ci_metric "${name}_secs" $((SECONDS - t0))
  ci_metric "${name}_bytes" "$(wc -c < "$out/WC.c" 2>/dev/null || echo 0)"
  echo "[probe] $name rc=$rc secs=$((SECONDS - t0))" >&2
  sed 's/^/[probe]   /' "$out/stderr.log" | tail -12 >&2
  if [[ $rc -eq 0 ]]; then ci_metric survivor "$name"; fi
  rm -rf "$out"
}

attempt bare
attempt threads --enable-threads
attempt nonames --no-debug-names
attempt both --enable-threads --no-debug-names
attempt eh --enable-exceptions --enable-threads --no-debug-names

ci_finish
