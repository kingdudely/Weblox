#!/usr/bin/env bash
# Reproducible native C++ host build, from a stock WebClient.wasm to host/demo.
#
# Usage:
#   ./host/build_all.sh [stock-WebClient.wasm]
#
# Required environment (all have defaults; see host/BUILD.md):
#   WC_STOCK_WASM    stock version-1ef59424f70d4932 module (or use $1)
#   WC_WASM          patched module produced here; input to the generators
#   WC_OBJ           Cranelift object produced here; linked into demo
#   WC_QUICHE_DIR    libquiche prefix (include/, lib/, link.mk)
#
# Optional incremental skips: WC_SKIP_PATCH=1, WC_SKIP_OBJ=1,
# WC_SKIP_QUICHE=1 (reuse existing artifacts), JOBS=N (default 4).
#
# No generated binary, object, wasm or C++ source is written into the repo.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="$ROOT/host"
PATCH_NEW="$ROOT/v2-wasmtime/tools/patch_new.py"

export WC_WASM="${WC_WASM:-/tmp/opencode/WebClient.raknet.wasm}"
export WC_OBJ="${WC_OBJ:-/tmp/opencode/out.full.o}"
export WC_QUICHE_DIR="${WC_QUICHE_DIR:-/tmp/weblox-quiche}"
WC_STOCK_WASM="${1:-${WC_STOCK_WASM:-/tmp/opencode/WebClient.orig.wasm}}"
WASM2OBJ="${WASM2OBJ:-$ROOT/tools/wasm2obj/target/release/wasm2obj}"
JOBS="${JOBS:-4}"

say() { printf '[build_all] %s\n' "$*"; }
die() { printf '[build_all] ERROR: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have python3 || die "python3 not found"
have cargo   || die "cargo not found"
have make    || die "make not found"
[ -f "$PATCH_NEW" ] || die "$PATCH_NEW missing"
command -v clang++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 \
  || die "clang++ or g++ not found"

TMPD="$(mktemp -d "${TMPDIR:-/tmp/opencode}/weblox-host-build.XXXXXX")"
trap 'rm -rf "$TMPD"' EXIT

# ---------------------------------------------------------------- 1. wasm --
if [ "${WC_SKIP_PATCH:-0}" = 1 ] && [ -f "$WC_WASM" ]; then
  say "reusing patched wasm $WC_WASM (WC_SKIP_PATCH=1)"
else
  [ -f "$WC_STOCK_WASM" ] || die "stock wasm not found: $WC_STOCK_WASM
  Fetch version-1ef59424f70d4932 from setup.rbxcdn.com (see host/BUILD.md)
  and pass it as the first argument or set WC_STOCK_WASM."

  say "stock wasm: $WC_STOCK_WASM ($(stat -c %s "$WC_STOCK_WASM") bytes)"

  # Gate patch: func[17777] body at file offset 0x117c06c (in the stock
  # module) becomes `i32.const 1; nop x32; end` -- the browser-side play gate
  # must always pass. Use patch_new.py's own section parser so this is keyed
  # on the function index and exact body, never on a blind byte overwrite.
  say "gate patch: func[17777] -> i32.const 1"
  python3 - "$PATCH_NEW" "$WC_STOCK_WASM" "$TMPD/gated.wasm" <<'PY'
import importlib.util
import pathlib
import sys

patch_new_path, src, dst = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("patch_new", patch_new_path)
pn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pn)

GATE_FUNC = 17777
EXPECTED = bytes.fromhex(
    "00 02 7f 20 00 45 04 40 41 00 41 b4 9b 84 04 2d 00 00 "
    "41 01 71 45 0d 01 1a 0b 41 a4 c6 97 04 2d 00 00 0b 0b"
)

buf = pathlib.Path(src).read_bytes()
imported = pn.count_imported_funcs(buf)
_, _, body = pn.func_body(buf, GATE_FUNC)
if body != EXPECTED:
    raise SystemExit("gate patch: unexpected body for func[%d]: %s" %
                     (GATE_FUNC, body.hex()))
# Keep the zero-local-group prefix; replace the rest with a constant-1
# return padded to the original 36-byte body length.
new_body = body[:1] + b"\x41\x01" + b"\x01" * 32 + b"\x0b"
assert len(new_body) == len(body) == 36
out = pn.replace_functions(buf, {GATE_FUNC - imported: new_body})
pathlib.Path(dst).write_bytes(out)
print("gate patch: func[%d] %d -> %d bytes" %
      (GATE_FUNC, len(body), len(new_body)))
PY

  # Socket/RakNet patches from the existing tool. Its preflight assertions
  # abort on a different engine build instead of silently patching bad sites.
  say "patch_new.py --all"
  python3 "$PATCH_NEW" "$TMPD/gated.wasm" "$TMPD/WebClient.raknet.wasm" \
    --all --abi "$(dirname "$WC_WASM")/WebClient.raknet.abi.json"
  mkdir -p "$(dirname "$WC_WASM")"
  mv "$TMPD/WebClient.raknet.wasm" "$WC_WASM"
  say "patched wasm: $WC_WASM ($(stat -c %s "$WC_WASM") bytes)"
fi

# ------------------------------------------------------------- 2. object --
if [ "${WC_SKIP_OBJ:-0}" = 1 ] && [ -f "$WC_OBJ" ]; then
  say "reusing object $WC_OBJ (WC_SKIP_OBJ=1)"
else
  say "building tools/wasm2obj (release)"
  cargo build --release --manifest-path "$ROOT/tools/wasm2obj/Cargo.toml"
  [ -x "$WASM2OBJ" ] || die "wasm2obj not found at $WASM2OBJ"
  say "compiling 154,552 functions with wasm2obj --all (~260 s)"
  mkdir -p "$(dirname "$WC_OBJ")"
  "$WASM2OBJ" --all --out "$TMPD/out.full.o" "$WC_WASM"
  mv "$TMPD/out.full.o" "$WC_OBJ"
  say "object: $WC_OBJ ($(stat -c %s "$WC_OBJ") bytes)"
fi

# ------------------------------------------------------------ 3. quiche --
if [ "${WC_SKIP_QUICHE:-0}" = 1 ] && [ -f "$WC_QUICHE_DIR/link.mk" ]; then
  say "reusing libquiche at $WC_QUICHE_DIR (WC_SKIP_QUICHE=1)"
else
  say "building libquiche into $WC_QUICHE_DIR (first build ~6 min)"
  QUICHE_PREFIX="$WC_QUICHE_DIR" WC_QUICHE_DIR="$WC_QUICHE_DIR" \
    "$HOST/build_quiche.sh"
fi
[ -f "$WC_QUICHE_DIR/link.mk" ] \
  || die "$WC_QUICHE_DIR/link.mk missing after libquiche build"

# ------------------------------------------------- 4. generated sources --
say "generating table.cpp / runtime_data.cpp from $WC_WASM"
( cd "$HOST" \
  && python3 gen_table.py --wasm "$WC_WASM" --out-dir . \
  && python3 gen_runtime_data.py --wasm "$WC_WASM" --out-dir . )

# -------------------------------------------------------------- 5. link --
say "linking host/demo"
make -C "$HOST" -j"$JOBS" demo

say "done: $HOST/demo ($(stat -c %s "$HOST/demo") bytes)"
say "smoke test: cd host && timeout 60s ./demo --skip-ctors"
