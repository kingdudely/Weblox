CXX = clang++
WASM_OBJ ?= /tmp/opencode/out.full.o
CXXFLAGS ?= -std=c++17 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -I.
LDFLAGS ?= -lpthread -ldl -lm

.PHONY: all run offsets clean

all: demo

demo: main.cpp vmctx.cpp vmctx.h vmctx_offsets.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp vmctx.cpp $(WASM_OBJ) $(LDFLAGS)

# Regenerate the ABI header from wasmtime-environ 50.0.2 (needs the wasm file).
offsets:
	cargo run --offline --release --bin gen_vmctx -- /tmp/opencode/WebClient.raknet.wasm > vmctx_offsets.h

run: demo
	timeout 60s ./demo

clean:
	rm -f demo
