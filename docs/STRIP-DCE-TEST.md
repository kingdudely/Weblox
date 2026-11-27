# Dead-code / strip test on the stock engine wasm (2026-11-26)

Tested on `/home/john/rr/WebClient.wasm` (102,268,042 B).

## Section layout

```
Type     894 types
Import   342 (341 funcs + 1 shared memory)
Function 154550
Table    1 funcref table (min=max=161491)
Tag      2
Global   15
Export   85
Start    func 344
Elem     1 active segment, 161490 entries
DataCount 3689
Code     154550 functions, 92,934,210 B
Data     3689 segments, 8,812,415 B
Custom   only "build_id", 26 B
```

## Commands and results

```sh
# 1. Remove custom sections
wasm-tools strip WebClient.wasm -o stripped.wasm
# 102,268,014 B  (-28 B)

# 2. DCE + remove-unused-module-elements + strip debug/producers
wasm-opt --all-features --dce --remove-unused-module-elements \
  --strip-debug --strip-producers WebClient.wasm -o dce.wasm
# 103,342,741 B  (+1,074,699 B)
# function count still 154550, exports still 85, elements still 161490,
# code section grew from 92,934,210 B to 93,801,741 B
wasm-tools validate --features all dce.wasm   # ok
```

## Verdict

**No useful gain.** There are only 26 B of custom sections to strip. DCE removes
**zero functions** because every defined function is reachable through the
exported `__indirect_function_table` (161490 entries) — exactly the Weblox
finding. Binaryen's re-encode actually *grows* the code section by ~0.9%.

Do not revisit dead-code stripping on this module. It cannot reduce the 92 MB
code section, which is where all the size and build cost lives.

## Note on the LLVM/WAMR benchmark claim

The "WAMR AOT 1.57x native" benchmark is **not** "wasm → LLVM → pure native
binary running in our own host". WAMR AOT compiles wasm to native code with
LLVM but still runs it through the WAMR runtime (`iwasm`) for memory/traps/
imports. It is not comparable to our Cranelift `.o` + thin C++ host; it is a
different runtime with AOT codegen.
