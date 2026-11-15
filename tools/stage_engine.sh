#!/usr/bin/env bash
# Stage everything the app serves at runtime into the bundle root.
#
#   ./tools/stage_engine.sh
#
# Lands in iwa/:
#   engine/WebClient.wasm   the patched module, from build/
#   engine/WebClient.js     the glue, with tools/worker-prelude.js in front
#   engine/WasmPlayer.js    the loader
#   engine/*                audio-processor-fast.js and the metadata
#   abi.js                  the ring layout, from patch.py's --abi
#
# Content (fonts, meshes, sounds, sky) is not fetched here: tools/mirror_engine.py
# owns it. This script is the part that must be redone after every module build.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
engine="${ENGINE_DIR:-/tmp/opencode/rw}"
wasm="${WASM:-$here/build/WebClient.patched.wasm}"
out="$here/iwa/engine"

die() { echo "$*" >&2; exit 1; }

[ -f "$wasm" ] || die "no patched module at $wasm — build it: ./tools/build_patched.sh <stock.wasm> $wasm"

mkdir -p "$out"

# The module always comes from build/. 100 MB; skip the copy when the source
# has not moved.
cp -u "$wasm" "$out/WebClient.wasm"

if [ -f "$engine/WebClient.js" ]; then
  for f in WasmPlayer.js WebClient.metadata.json audio-processor-fast.js; do
    if [ -f "$engine/$f" ]; then cp -u "$engine/$f" "$out/$f"; fi
  done

  # Always rebuilt from the pristine glue, so the stage is idempotent and a
  # changed prelude always takes effect. 129 KB, not worth a staleness check.
  tmp="$out/WebClient.js.tmp"
  cat "$here/tools/worker-prelude.js" "$engine/WebClient.js" > "$tmp"
  mv "$tmp" "$out/WebClient.js"
elif [ -f "$out/WebClient.js" ] && [ -f "$out/WasmPlayer.js" ]; then
  # No engine tree on this machine: the glue already staged in iwa/engine has
  # the prelude in front, so keep it and only refresh the module above.
  echo "stage: no engine source at $engine — keeping the staged glue" >&2
else
  die "no glue at $engine/WebClient.js — set ENGINE_DIR to a downloaded engine tree"
fi

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
