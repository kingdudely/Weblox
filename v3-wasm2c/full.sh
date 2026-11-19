#!/bin/sh
# Full build. The 512 translated module TUs are only recompiled when the wasm
# itself changes; host/impl/harness always go through ccache.
set -e
mkdir -p obj

CC="clang"
# a local ccache (tools are not installable without sudo); it makes the rare
# whole-module rebuild cheap and the host loop effectively free
if [ -x /home/john/deps/ccache/bin/ccache-wrap ] && [ -z "$WC_NO_CCACHE" ]; then
  CC="/home/john/deps/ccache/bin/ccache-wrap clang"
  export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
  export CCACHE_COMPILERCHECK=content
fi
# nothing below the pch should change while iterating: WC.h / WC-impl.h are
# hashed into the stamp, and editing either rebuilds all 511 module TUs.

F="-O0 -w -fno-asynchronous-unwind-tables -fno-stack-protector -DWASM_RT_STACK_DEPTH_COUNT=1"
I="-I. -I/home/john/deps/wabt/wasm2c -I/home/john/deps/simde"
if [ ! -f pre.h.pch ] || [ -n "$(find pre.h WC.h WC-impl.h -newer pre.h.pch -print -quit)" ]; then
  $CC -O0 -w $I pre.h -o pre.h.pch
fi
# gcc compiles this output roughly 4x faster than clang at -O0 but is stricter
# about the pointer types wasm2c emits, so silence those diagnostics.
MOD_CC=${WC_MOD_CC:-clang}
case "$MOD_CC" in
  *gcc*) F="$F -D_GNU_SOURCE -include pre.h -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-pointer-sign -Wno-builtin-declaration-mismatch" ;;
  *)     F="$F -include-pch pre.h.pch" ;;
esac

WASM=${WC_WASM:-/home/john/rr/WebClient.wasm}
STAMP=$(cat "$WASM" WC-impl.h pre.h | sha256sum | cut -d' ' -f1)
if [ -n "$WC_KEEP" ] && [ "$(ls obj/WC_*.o 2>/dev/null | wc -l)" -gt 500 ]; then
  echo "WC_KEEP: reusing module objects"
elif [ -z "$WC_FORCE" ] && [ -f obj/stamp ] && [ "$(cat obj/stamp)" = "$STAMP" ]; then
  :
else
  echo "wasm changed - rebuilding module objects"
  rm -f obj/WC_*.o obj/stamp
fi

ls WC_*.c | xargs -P "$(nproc)" -I{} sh -c "[ -f obj/{}.o ] || $MOD_CC -c $F $I {} -o obj/{}.o"
if [ -z "$WC_KEEP" ] || [ ! -f obj/stamp ]; then echo "$STAMP" > obj/stamp; fi

# the runtime is a fixed object; depth counting keeps a fibre from dying on a
# host stack overflow we cannot attribute (see impl/fiber.c)
[ -f obj/rt2.o ] || $CC -c -O0 -w -DWASM_RT_STACK_DEPTH_COUNT=1 \
  -Dwasm_rt_trap=wc_rt2_trap \
  -I/home/john/deps/wabt/wasm2c /home/john/deps/wabt/wasm2c/wasm-rt-impl.c -o obj/rt2.o

WC_CC="$CC" sh build_host.sh
$CC -fuse-ld=lld -no-pie -Wl,--wrap=wc_rt2_trap obj/WC_*.o obj/host_*.o obj/impl_*.o obj/harness.o obj/rt2.o obj/exc.o obj/c5tab.o obj/c6tab.o obj/c7tab.o \
  -o ./luau_probe2 -lm -lpthread -lSDL3 -Wl,--unresolved-symbols=ignore-all
echo FULL_OK
