#!/usr/bin/env python3
"""Generate C definitions for every wasm2c host import from a generated WC.h.

Usage:
    python3 tools/ci/gen_stubs.py <path/to/WC.h> > stubs.c

No indices or guest pointers are involved: the declarations come straight
from the generated header, so an engine update only requires a new header.
"""


from __future__ import annotations

import re
import sys
from pathlib import Path


DECL = re.compile(
    r"^(?P<ret>void|u8|u16|u32|u64|f32|f64|v128)\s+"
    r"(?P<name>w2c_env_[A-Za-z0-9_]+|w2c_wasi__snapshot__preview1_[A-Za-z0-9_]+)"
    r"\((?P<args>[^;]*)\);\s*$"
)


def zero(ret: str) -> str:
    if ret == "void":
        return ""
    if ret.startswith("f"):
        return " return 0.0;"
    if ret == "v128":
        return " return wasm_rt_v128_zero();"
    return " return 0;"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gen_stubs.py WC.h", file=sys.stderr)
        return 2

    text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    seen: set[str] = set()
    out = [
        "/* Generated from WC.h by tools/ci/gen_stubs.py. DO NOT EDIT. */",
        '#include "WC.h"',
        "#include <string.h>",
        "",
        "wasm_rt_shared_memory_t g_mem;",
    ]
    # WABT 1.0.42 forward-declares these structs in WC.h; the host owns them.
    if "struct w2c_env {" not in text:
        out.append("struct w2c_env { int unused; };")
    if "struct w2c_wasi__snapshot__preview1 {" not in text:
        out.append("struct w2c_wasi__snapshot__preview1 { int unused; };")
    out += [
        "struct w2c_env g_env;",
        "struct w2c_wasi__snapshot__preview1 g_wasi;",
        "",
        "wasm_rt_shared_memory_t* w2c_env_memory(struct w2c_env* e) {",
        "  (void)e; return (wasm_rt_shared_memory_t*)&g_mem;",
        "}",
        "",
    ]

    for line in text.splitlines():
        match = DECL.match(line.strip())
        if not match:
            continue
        name = match.group("name")
        if name == "w2c_env_memory" or name in seen:
            continue
        seen.add(name)
        raw_args = [a.strip() for a in match.group("args").split(",") if a.strip()]
        args = [] if raw_args == ["void"] else raw_args
        params = []
        ignored = []
        for index, arg in enumerate(args):
            # Declarations have bare types (e.g. `struct w2c_env*`, `u32`).
            param = f"{arg} a{index}"
            params.append(param)
            ignored.append(f"  (void)a{index};")
        signature = f"{match.group('ret')} {name}({', '.join(params)})"
        out.append(f"{signature} {{")
        out.extend(ignored)
        body = zero(match.group("ret"))
        if body:
            out.append(body.strip())
        out.append("}")
        out.append("")

    sys.stdout.write("\n".join(out) + "\n")
    print(f"// generated {len(seen)} import stubs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
