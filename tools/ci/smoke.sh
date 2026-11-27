#!/usr/bin/env bash
# Stage B smoke link: compile generated import stubs + a tiny harness against
# the cached wasm2c engine library and instantiate the module once.
#
#   tools/ci/fetch_engine.sh      # or engine.sh on a machine with enough RAM
#   tools/ci/smoke.sh
#
# Env: WC_ENGINE_DIR, CC, WC_BUILD_DIR, WC_SMOKE_TIMEOUT.

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$HERE/common.sh"
ci_init

ENGINE_DIR=${WC_ENGINE_DIR:-}
if [[ -z $ENGINE_DIR && -f $CI_BUILD_DIR/engine_dir ]]; then
  ENGINE_DIR=$(cat "$CI_BUILD_DIR/engine_dir")
fi
[[ -f $ENGINE_DIR/env.sh ]] || {
  echo "[smoke] no engine library - run engine.sh or fetch_engine.sh first" >&2
  exit 2
}
# shellcheck source=/dev/null
. "$ENGINE_DIR/env.sh"

OUT="$CI_BUILD_DIR/smoke"
mkdir -p "$OUT"
CC=${CC:-cc}
INCLUDES="-I$GEN_INCLUDE -I$WABT_INCLUDE"
[[ -n ${SIMDE_DIR:-} ]] && INCLUDES="$INCLUDES -I$SIMDE_DIR"

echo "[smoke] generating import stubs from $GEN_INCLUDE/WC.h" >&2
python3 "$HERE/gen_stubs.py" "$GEN_INCLUDE/WC.h" > "$OUT/stubs.c"
ci_metric stubs "$(grep -c '^u32 w2c_\|^void w2c_\|^f64 w2c_\|^u64 w2c_' "$OUT/stubs.c")"

echo "[smoke] compiling and linking" >&2
"$CC" -w $INCLUDES "$OUT/stubs.c" "$HERE/smoke.c" \
  "$ENGINE_LIB" -o "$OUT/smoke" -lm -lpthread
ci_metric binary_bytes "$(wc -c < "$OUT/smoke" | tr -d ' ')"

echo "[smoke] running instantiation" >&2
set +e
timeout "${WC_SMOKE_TIMEOUT:-120}" "$OUT/smoke" > "$OUT/run.log" 2>&1
rc=$?
set -e
ci_metric smoke_rc "$rc"
cat "$OUT/run.log"
ci_finish
exit "$rc"
