# Minimal C++ Wasmtime-ABI host build.
#
# Every large input is overridable from the environment (see host/BUILD.md):
#   WC_WASM        patched wasm feeding the C++ generators and wasm2obj
#   WC_OBJ         Cranelift object linked into `demo`
#   WC_QUICHE_DIR  libquiche prefix containing include/, lib/ and link.mk
# Defaults point at /tmp/opencode; nothing in this directory is committed.
CXX ?= clang++

# Legacy names from the older docs remain as aliases; WC_* win when both are
# set, otherwise an env-provided WASM/WASM_OBJ/QUICHE_PREFIX still works.
WASM ?= /tmp/opencode/WebClient.raknet.wasm
WASM_OBJ ?= /tmp/opencode/out.full.o
QUICHE_PREFIX ?= /tmp/weblox-quiche
WC_WASM ?= $(WASM)
WC_OBJ  ?= $(WASM_OBJ)
WC_QUICHE_DIR ?= $(QUICHE_PREFIX)

REPO_ROOT := $(abspath $(CURDIR)/..)
WASM2OBJ ?= $(REPO_ROOT)/tools/wasm2obj/target/release/wasm2obj
WASM2OBJ_ARGS ?= --all

# libquiche is built out of tree by ./build_quiche.sh; link.mk carries the
# flags (see host/RNA.md and host/WT.md).  Nothing binary is committed.
# If the path was not explicitly configured, accept the historical prefix.
WC_QUICHE_EXPLICIT := $(filter environment command line,$(origin WC_QUICHE_DIR) $(origin QUICHE_PREFIX))
ifeq ($(WC_QUICHE_EXPLICIT),)
ifeq ($(wildcard $(WC_QUICHE_DIR)/link.mk),)
ifneq ($(wildcard /tmp/wl-qtry-prefix/link.mk),)
WC_QUICHE_DIR := /tmp/wl-qtry-prefix
endif
endif
endif
ifneq ($(wildcard $(WC_QUICHE_DIR)/link.mk),)
include $(WC_QUICHE_DIR)/link.mk
endif

CXXFLAGS ?= -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -I. \
            $(WEBLOX_QUICHE_CXXFLAGS)
LDFLAGS ?= -rdynamic -lpthread -ldl -lm -lcurl $(WEBLOX_QUICHE_LDFLAGS)

.PHONY: all run offsets table runtime_data obj wasm2obj-tool clean

all: demo

# The element section contains 161490 entries; generating the C++ source at
# build time keeps the (16 MB) output out of git.  table.h is checked in.
table: table.cpp

table.cpp: gen_table.py $(WC_WASM)
	python3 gen_table.py --wasm $(WC_WASM) --out-dir .

# 27 MB of data-segment images; regenerate at build time, keep out of git.
runtime_data: runtime_data.cpp

runtime_data.cpp runtime_data.h: gen_runtime_data.py $(WC_WASM)
	python3 gen_runtime_data.py --wasm $(WC_WASM) --out-dir .

demo: main.cpp vmctx.cpp imports.cpp rna_bridge.cpp wt.cpp route.cpp \
      prepare.cpp runtime_data.cpp table.cpp \
      vmctx.h imports.h rna_bridge.h wt.h route.h prepare.h \
      vmctx_offsets.h runtime_data.h table.h
	@if [ ! -f "$(WC_OBJ)" ]; then \
	  echo "Makefile: $(WC_OBJ) missing; run 'make obj' or host/build_all.sh" >&2; \
	  exit 1; \
	fi
	$(CXX) $(CXXFLAGS) -o $@ main.cpp vmctx.cpp imports.cpp rna_bridge.cpp \
		wt.cpp route.cpp prepare.cpp runtime_data.cpp table.cpp $(WC_OBJ) \
		$(LDFLAGS)

# Build the Cranelift compiler tool and emit the object from $(WC_WASM).
# Codegen of all 154,552 functions takes ~260 s; see host/BUILD.md.
wasm2obj-tool:
	cargo build --release --manifest-path $(REPO_ROOT)/tools/wasm2obj/Cargo.toml

obj: wasm2obj-tool $(WC_WASM)
	mkdir -p $(dir $(WC_OBJ))
	$(WASM2OBJ) $(WASM2OBJ_ARGS) --out $(WC_OBJ) $(WC_WASM)

# Regenerate the ABI header from wasmtime-environ 50.0.2 (needs the wasm file).
offsets:
	cargo run --offline --release --bin gen_vmctx -- $(WC_WASM) > vmctx_offsets.h

run: demo
	timeout 60s ./demo

clean:
	rm -f demo table.cpp runtime_data.cpp
