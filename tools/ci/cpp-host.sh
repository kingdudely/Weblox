#!/usr/bin/env bash
# Build the C++ host skeleton against the newest wasm2c engine library.
# All heavy work happens on the runner; local machines only need the sources.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

bash "$HERE/fetch_engine.sh"
ENGINE_DIR=$(cat "$CI_BUILD_DIR/engine_dir")
# The downloaded env.sh has local paths (fetch_engine.sh rewrites it).
# shellcheck source=/dev/null
. "$ENGINE_DIR/env.sh"

if [[ ! -f $WABT_INCLUDE/wasm-rt.h ]]; then
  ci_fetch_wabt
  WABT_INCLUDE="$WASM2C_INCLUDE"
fi

CPP="$ENGINE_DIR/cpp"
mkdir -p "$CPP"
python3 "$HERE/gen_stubs.py" "$GEN_INCLUDE/WC.h" \
  "$CI_ROOT/v3-wasm2c/tools/implemented.txt" > "$CPP/stubs.c"
ci_metric stubs "$(grep -c '^u32 w2c_\|^void w2c_\|^f64 w2c_\|^u64 w2c_' "$CPP/stubs.c")"

CFLAGS="-w -I$GEN_INCLUDE -I$WABT_INCLUDE"
CXXFLAGS="-std=c++17 -w -DWASM_RT_C11_AVAILABLE=1 -I$GEN_INCLUDE -I$WABT_INCLUDE -I$CI_ROOT/v3-wasm2c/cpp"
CC=${CC:-$(command -v clang >/dev/null 2>&1 && echo clang || echo cc)}
CXX=${CXX:-$(command -v clang++ >/dev/null 2>&1 && echo clang++ || echo c++)}

"$CC" -c $CFLAGS "$CPP/stubs.c" -o "$CPP/stubs.o"
"$CC" -c $CFLAGS "$CI_ROOT/v3-wasm2c/cpp/mem.c" -o "$CPP/mem.o"
CPP_SRCS="host.cpp window.cpp platform.cpp net_stub.cpp net_curl.cpp imports.cpp"
CPP_OBJS=""
for src in $CPP_SRCS; do
  "$CXX" -c $CXXFLAGS "$CI_ROOT/v3-wasm2c/cpp/$src" -o "$CPP/${src%.cpp}.o"
  CPP_OBJS="$CPP_OBJS $CPP/${src%.cpp}.o"
done
"$CXX" $CPP_OBJS "$CPP/mem.o" "$CPP/stubs.o" "$ENGINE_LIB" \
  -o "$CPP/weblox-host" -lm -lpthread
ci_metric host_bytes "$(wc -c < "$CPP/weblox-host" | tr -d ' ')"

set +e
timeout "${WC_SMOKE_TIMEOUT:-120}" "$CPP/weblox-host" > "$CPP/run.log" 2>&1
rc=$?
set -e
ci_metric host_rc "$rc"
cat "$CPP/run.log"
ci_finish
exit "$rc"
