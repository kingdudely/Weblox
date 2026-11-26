#!/bin/sh
# fail if any w2c_ import is still undefined (ignore-all would turn it into call 0)
out=$(clang -fuse-ld=lld -no-pie -Wl,--wrap=wc_rt2_trap obj/WC_*.o obj/host_*.o obj/impl_*.o obj/harness.o obj/rt2.o obj/exc.o \
  obj/c5tab.o obj/c6tab.o obj/c7tab.o -o /dev/null -lm -lpthread -lSDL3 \
  -Wl,--unresolved-symbols=report-all 2>&1 | grep -E 'undefined reference to .w2c_' )
if [ -n "$out" ]; then echo "$out"; echo "UNRESOLVED"; exit 1; fi
echo CHECK_OK
