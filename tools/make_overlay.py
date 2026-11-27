#!/usr/bin/env python3
"""Create a clang VFS overlay that fixes generated WC-impl.h atomics.

The published generated header contains non-atomic aliases for shared-memory
operations and three no-op static inline wait/notify helpers.  We never edit
the Release asset: a corrected copy is written into the build directory and a
clang VFS overlay maps the original path to it.

Usage: make_overlay.py <path/to/WC-impl.h> <output-dir>
Prints the path of the overlay JSON file.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# Shared-memory ops that were redirected to their non-atomic counterparts.
NONATOMIC_ALIAS = re.compile(
    rb"^#define i(?:32|64)_atomic_[^\r\n]*_shared_default32\([^\r\n]*\r?\n",
    re.MULTILINE,
)

REDIRECTS = b"""
/* Build-time fix: redirect WABT's no-op atomic wait/notify stubs. */
extern wasm_rt_shared_memory_t g_mem;
u32 weblox_atomic_notify(wasm_rt_shared_memory_t* mem, u32 address, u32 count);
u32 weblox_atomic_wait32(wasm_rt_shared_memory_t* mem, u32 address, u32 value, u64 timeout);
u32 weblox_atomic_wait64(wasm_rt_shared_memory_t* mem, u32 address, u64 value, u64 timeout);
#define wasm_rt_atomic_notify(base, size, address, count) \\
  weblox_atomic_notify(&g_mem, (address), (count))
#define wasm_rt_atomic_wait32(base, size, address, expected, timeout) \\
  weblox_atomic_wait32(&g_mem, (address), (expected), (timeout))
#define wasm_rt_atomic_wait64(base, size, address, expected, timeout) \\
  weblox_atomic_wait64(&g_mem, (address), (expected), (timeout))
"""


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_overlay.py WC-impl.h OUTPUT_DIR", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).resolve()
    out_dir = Path(sys.argv[2]).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    data, count = NONATOMIC_ALIAS.subn(b"", source.read_bytes())
    if count == 0:
        raise SystemExit(f"no non-atomic aliases found in {source}")
    data = data.rstrip() + b"\n" + REDIRECTS

    corrected = out_dir / "WC-impl-atomic.h"
    corrected.write_bytes(data)
    overlay = out_dir / "atomic-headers.json"
    overlay.write_text(
        json.dumps(
            {
                "version": 0,
                "roots": [
                    {
                        "type": "file",
                        "name": source.as_posix(),
                        "external-contents": corrected.as_posix(),
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    print(f"stripped {count} non-atomic aliases", file=sys.stderr)
    print(overlay.as_posix())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
