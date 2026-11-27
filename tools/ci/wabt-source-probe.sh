#!/usr/bin/env bash
# Diagnostic: build WABT 1.0.36 from source on the runner and try translating
# the pinned engine.  The 1.0.36 prebuilt needs libcrypto.so.1.1, which the
# current ubuntu-latest image no longer ships.  Source build avoids that.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

WASM=$(ci_fetch_wasm)
SRC="$CI_BUILD_DIR/wabt-src"
ROOT="$CI_BUILD_DIR/wabt-1.0.36-source"
if [[ ! -x $ROOT/bin/wasm2c ]]; then
  mkdir -p "$SRC" "$ROOT"
  if [[ ! -f $SRC/CMakeLists.txt ]]; then
    echo "[wsp] fetching WABT 1.0.36 source" >&2
    curl -fL --retry 3 -o "$CI_BUILD_DIR/wabt-1.0.36.tar.xz" \
      https://github.com/WebAssembly/wabt/releases/download/1.0.36/wabt-1.0.36.tar.xz
    tar -xf "$CI_BUILD_DIR/wabt-1.0.36.tar.xz" -C "$SRC" --strip-components=1
  fi
  echo "[wsp] building WABT 1.0.36" >&2
  cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF -DWABT_INSTALL_BINDIR=bin
  cmake --build "$SRC/build" -j "$(ci_nproc)" --target wasm2c
  mkdir -p "$ROOT/bin" "$ROOT/wasm2c"
  cp "$SRC/build/wasm2c" "$ROOT/bin/wasm2c"
  cp "$SRC"/wasm2c/*.h "$ROOT/wasm2c/"
  cp "$SRC"/wasm2c/*.c "$ROOT/wasm2c/"
fi

"$ROOT/bin/wasm2c" --version 2>/dev/null || true
OUT="$CI_BUILD_DIR/probe-source"
mkdir -p "$OUT"
T0=$SECONDS
set +e
( ulimit -c 0
  "$ROOT/bin/wasm2c" --enable-exceptions --enable-threads --no-debug-names \
    --module-name=WebClient --num-outputs=64 "$WASM" -o "$OUT/WC.c" ) \
    > "$OUT/stdout.log" 2> "$OUT/stderr.log"
rc=$?
set -e
ci_metric rc "$rc"
ci_metric secs $((SECONDS - T0))
ci_metric bytes "$(cat "$OUT"/WC*.c 2>/dev/null | wc -c | tr -d ' ')"
echo "[wsp] rc=$rc secs=$((SECONDS - T0))" >&2
sed 's/^/[wsp]   /' "$OUT/stderr.log" | tail -12 >&2
ci_finish
