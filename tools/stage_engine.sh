#!/usr/bin/env bash
# Stage everything the app serves at runtime into the bundle root.
#
#   ./tools/stage_engine.sh
#
# Three things land in iwa/:
#   engine/WebClient.wasm        the patched module
#   engine/WebClient.js          the glue, with tools/worker-prelude.js in front
#   engine/WasmPlayer.js         the loader
#   abi.js                       the ring layout, from patch.py's --abi
#
# Content (fonts, meshes, sounds, sky) is not fetched here: it changes rarely and
# tools/mirror_engine.py owns it. This script is the part that must be redone
# after every module rebuild.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
engine="${ENGINE_DIR:-/tmp/opencode/rw}"
wasm="${WASM:-$here/build/WebClient.patched.wasm}"
out="$here/iwa/engine"

die() { echo "$*" >&2; exit 1; }

[ -f "$wasm" ] || die "no patched module at $wasm — build it: ./tools/build_patched.sh <stock.wasm> $wasm"
[ -f "$engine/WebClient.js" ] || die "no glue at $engine/WebClient.js — point ENGINE_DIR at a downloaded engine tree"

mkdir -p "$out"

# 100 MB; skip the copy when the source has not moved.
cp -u "$wasm" "$out/WebClient.wasm"
for f in WasmPlayer.js WebClient.metadata.json; do
  [ -f "$engine/$f" ] && cp -u "$engine/$f" "$out/$f"
done

# Always rebuilt from the pristine glue, so the stage is idempotent and a changed
# prelude always takes effect. 129 KB, not worth a staleness check.
tmp="$out/WebClient.js.tmp"
cat "$here/tools/worker-prelude.js" "$engine/WebClient.js" > "$tmp"
mv "$tmp" "$out/WebClient.js"

# The ring layout exists in exactly one place: patch.py's output.
abi="${wasm%.wasm}.ring.json"
if [ -f "$abi" ]; then
  printf 'window.__rkAbi = %s;\n' "$(cat "$abi")" > "$here/iwa/abi.js"
else
  echo "warning: no ring layout at $abi — /abi.js will not load" >&2
fi

printf 'staged: %s (%s), glue %s B\n' \
  "$(du -h "$out/WebClient.wasm" | cut -f1)" \
  "$(basename "$wasm")" \
  "$(wc -c < "$out/WebClient.js")"
