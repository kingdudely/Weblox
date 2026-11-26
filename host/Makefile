CXX ?= clang++
WASM_OBJ ?= /tmp/opencode/out.full.o
WASM ?= /tmp/opencode/WebClient.raknet.wasm

# libquiche is built out of tree by ./build_quiche.sh; link.mk carries the
# flags (see host/RNA.md and host/WT.md). Nothing binary is committed.
QUICHE_PREFIX ?= /tmp/wl-qtry-prefix
ifeq ($(wildcard $(QUICHE_PREFIX)/link.mk),)
QUICHE_PREFIX := /tmp/weblox-quiche
endif
ifneq ($(wildcard $(QUICHE_PREFIX)/link.mk),)
include $(QUICHE_PREFIX)/link.mk
endif

CXXFLAGS ?= -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -I. \
            $(WEBLOX_QUICHE_CXXFLAGS)
LDFLAGS ?= -rdynamic -lpthread -ldl -lm -lcurl $(WEBLOX_QUICHE_LDFLAGS)

.PHONY: all run offsets table clean

all: demo

# The element section contains 161490 entries; generating the C++ source at
# build time keeps the (16 MB) output out of git.  table.h is checked in.
table: table.cpp

table.cpp: gen_table.py $(WASM)
	python3 gen_table.py --wasm $(WASM) --out-dir .

# 27 MB of data-segment images; regenerate at build time, keep out of git.
runtime_data.cpp runtime_data.h: gen_runtime_data.py $(WASM)
	python3 gen_runtime_data.py --wasm $(WASM) --out-dir .

demo: main.cpp vmctx.cpp unwind.cpp imports.cpp rna_bridge.cpp wt.cpp route.cpp \
      prepare.cpp runtime_data.cpp table.cpp \
      vmctx.h unwind.h imports.h rna_bridge.h wt.h route.h prepare.h \
      vmctx_offsets.h runtime_data.h table.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp vmctx.cpp unwind.cpp imports.cpp \
		rna_bridge.cpp wt.cpp route.cpp prepare.cpp runtime_data.cpp table.cpp \
		$(WASM_OBJ) $(LDFLAGS)

# Regenerate the ABI header from wasmtime-environ 50.0.2 (needs the wasm file).
offsets:
	cargo run --offline --release --bin gen_vmctx -- /tmp/opencode/WebClient.raknet.wasm > vmctx_offsets.h

run: demo
	timeout 60s ./demo

clean:
	rm -f demo table.cpp
