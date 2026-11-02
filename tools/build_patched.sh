#!/usr/bin/env bash
# Rebuild the patched engine module from a stock WebClient.wasm.
#   ./tools/build_patched.sh <stock.wasm> <out.wasm>
set -euo pipefail
src="${1:?stock wasm}"
dst="${2:?output wasm}"
here="$(cd "$(dirname "$0")/.." && pwd)"
abi="${dst%.wasm}.ring.json"
python3 "$here/tools/patch.py" "$src" "$dst" \
  --patch getaddrinfo --patch nosocket --patch nofd --patch udp_send \
  --byte-patch portsub --transport --stash-peer \
  --abi "$abi"
if [ -x "$here/.cache/wabt-src/build/wasm-validate" ]; then
  "$here/.cache/wabt-src/build/wasm-validate" --enable-all "$dst" && echo "validate ok"
fi
