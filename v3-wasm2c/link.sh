#!/bin/sh
# link only (no recompiling of the translated module)
set -e
# fast path: host objects + link.  Warn if the translated module is stale, so a
# bad run is never mistaken for a source-level bug.
WASM=${WC_WASM:-/home/john/rr/WebClient.wasm}
STAMP=$(cat "$WASM" WC-impl.h pre.h | sha256sum | cut -d' ' -f1)
if [ -z "$WC_FORCE" ] && { [ ! -f obj/stamp ] || [ "$(cat obj/stamp)" != "$STAMP" ]; }; then
  echo "link.sh: module objects are stale (WC.h/WC-impl.h/wasm changed) - run full.sh" >&2
fi
sh build_host.sh
clang -fuse-ld=lld -no-pie -Wl,--wrap=wc_rt2_trap obj/WC_*.o obj/host_*.o obj/impl_*.o obj/harness.o obj/rt2.o obj/exc.o obj/c5tab.o obj/c6tab.o obj/c7tab.o \
  -o ./luau_probe2 -lm -lpthread -lSDL3 -Wl,--unresolved-symbols=ignore-all
echo LINK_OK
