#!/usr/bin/env bash
# disasm.sh — build a current wabt (distro 1.0.36 cannot decode this module)
# and cache a full disassembly of WebClient.wasm.
#
# usage: tools/disasm.sh /path/to/WebClient.wasm [outdir]
set -euo pipefail

WASM="${1:?usage: disasm.sh WebClient.wasm [outdir]}"
OUT="${2:-$PWD/.cache}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WABT="$ROOT/.cache/wabt-src"

mkdir -p "$OUT"

if [ ! -x "$WABT/build/wasm-objdump" ]; then
  echo "building wabt (needed for exnref/post-MVP encodings)..."
  git clone --depth 1 https://github.com/WebAssembly/wabt "$WABT"
  cmake -B "$WABT/build" -S "$WABT" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  cmake --build "$WABT/build" -j"$(nproc)"
fi

echo "disassembling to $OUT/disasm.txt (expect ~2.8 GB for a 100 MB module)..."
"$WABT/build/wasm-objdump" -d "$WASM" > "$OUT/disasm.txt"
"$WABT/build/wasm-objdump" -x "$WASM" > "$OUT/sections.txt"

echo "done:"
ls -lh "$OUT/disasm.txt" "$OUT/sections.txt"
