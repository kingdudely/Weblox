CXX = clang++
WASM_OBJ ?= /tmp/opencode/out.full.o
WASM ?= /tmp/opencode/WebClient.raknet.wasm
CXXFLAGS ?= -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -I.
LDFLAGS ?= -rdynamic -lpthread -ldl -lm -lcurl

.PHONY: all run offsets table clean

all: demo

# The element section contains 161490 entries; generating the C++ source at
# build time keeps the (16 MB) output out of git.  table.h is checked in.
table: table.cpp

table.cpp: gen_table.py $(WASM)
	python3 gen_table.py --wasm $(WASM) --out-dir .

demo: main.cpp vmctx.cpp imports.cpp runtime_data.cpp table.cpp vmctx.h imports.h vmctx_offsets.h runtime_data.h table.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp vmctx.cpp imports.cpp runtime_data.cpp table.cpp $(WASM_OBJ) $(LDFLAGS)

# Regenerate the ABI header from wasmtime-environ 50.0.2 (needs the wasm file).
offsets:
	cargo run --offline --release --bin gen_vmctx -- /tmp/opencode/WebClient.raknet.wasm > vmctx_offsets.h

run: demo
	timeout 60s ./demo

clean:
	rm -f demo table.cpp
