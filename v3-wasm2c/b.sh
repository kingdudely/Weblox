#!/bin/sh
set -e
F="-O0 -w -fno-asynchronous-unwind-tables -fno-stack-protector -include-pch pre.h.pch"
I="-I. -I/home/john/deps/wabt/wasm2c -I/home/john/deps/simde"
ls WC_*.c | xargs -P 8 -I{} sh -c "[ -f obj/{}.o ] || clang -c $F $I {} -o obj/{}.o"
clang -c $F $I stubs.c -o obj/stubs.o
clang -c $F $I harness.c -o obj/harness.o
clang -fuse-ld=lld -Wl,--unresolved-symbols=ignore-all obj/*.o -o /home/john/luau_probe -lm -lpthread
echo BUILD_OK
