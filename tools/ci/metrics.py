#!/usr/bin/env python3
"""Fold tools/ci metrics.txt and GNU `time -v` output into one JSON document.

Usage:
    python3 tools/ci/metrics.py build/ci/metrics.txt build/ci/time.txt \
      > build/ci/metrics.json

Values that look numeric are emitted as numbers so the results table in
docs/CI-RESULTS.md can be generated mechanically.
"""


from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def coerce(value: str):
    value = value.strip()
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    if re.fullmatch(r"-?\d+\.\d+", value):
        return float(value)
    return value


def main() -> int:
    out: dict[str, object] = {}

    metrics = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if metrics and metrics.exists():
        for line in metrics.read_text(encoding="utf-8", errors="replace").splitlines():
            key, sep, value = line.partition("=")
            if sep:
                out[key.strip()] = coerce(value)

    timing = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if timing and timing.exists():
        patterns = {
            "wall_secs": r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(\S+)",
            "user_secs": r"User time \(seconds\):\s*(\S+)",
            "system_secs": r"System time \(seconds\):\s*(\S+)",
            "peak_rss_kb": r"Maximum resident set size \(kbytes\):\s*(\d+)",
            "page_faults": r"Major \(requiring I/O\) page faults:\s*(\d+)",
        }
        text = timing.read_text(encoding="utf-8", errors="replace")
        for key, pattern in patterns.items():
            match = re.search(pattern, text)
            if match:
                raw = match.group(1)
                if key == "wall_secs" and ":" in raw:
                    parts = [float(p) for p in raw.split(":")]
                    raw = str(sum(p * 60 ** i for i, p in enumerate(reversed(parts))))
                out[key] = coerce(raw)

    json.dump(out, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
