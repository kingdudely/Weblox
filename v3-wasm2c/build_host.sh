#!/bin/sh
set -e
F="-O0 -w -fno-asynchronous-unwind-tables -fno-stack-protector"
[ -f pre.h.pch ] && F="$F -include-pch pre.h.pch"
I="-I. -I/home/john/deps/wabt/wasm2c -I/home/john/deps/simde"
CC=${WC_CC:-clang}
mkdir -p obj
for f in host/host.c host/plat_posix.c host/gl_sdl.c host/imports.c impl/*.c harness.c; do
  o=obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
  [ -f "$o" ] && [ -z "$(find "$f" -newer "$o" -print -quit)" ] && continue
  $CC -c $F $I "$f" -o "$o"
done
echo HOST_OK
