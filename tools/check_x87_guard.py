#!/usr/bin/env python3
"""Static check for the O-22 x87 SoftFloat publication guard.

The guard is intentionally tiny: it skips a thread-local SoftFloat store only
when the stored value is already equal to the target.  It must not become an
unconditional write, and x87_end() must keep clearing the exception flags.
This checker does not claim exactness; it only prevents the source shape from
drifting away from the reviewed form.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
X87 = ROOT / "runtime" / "cpu" / "x87.h"


def function_body(text: str, name: str) -> str:
    match = re.search(
        r"static inline void " + re.escape(name) + r"\(CPU \*c\) \{(.*?)\n\}",
        text,
        re.S,
    )
    if not match:
        raise SystemExit(f"x87_guard: {name} not found in {X87}")
    return match.group(1)


def main() -> int:
    text = X87.read_text(encoding="utf-8", errors="replace")
    begin = function_body(text, "x87_begin")
    end = function_body(text, "x87_end")

    required = [
        "const uint8_t mode = x87_round_mode(c);",
        "const uint8_t precision = x87_precision(c);",
        "if (softfloat_roundingMode != mode)",
        "if (extF80_roundingPrecision != precision)",
        "if (softfloat_exceptionFlags)",
    ]
    missing = [fragment for fragment in required if fragment not in begin]
    if missing:
        print("x87_guard: missing publication guard:")
        for fragment in missing:
            print(f"  {fragment}")
        return 1

    lines = begin.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"\s*softfloat_roundingMode\s*=", line):
            previous = lines[index - 1] if index else ""
            if "if (softfloat_roundingMode !=" not in previous:
                print("x87_guard: unconditional softfloat_roundingMode write")
                return 1
        if re.match(r"\s*extF80_roundingPrecision\s*=", line):
            previous = lines[index - 1] if index else ""
            if "if (extF80_roundingPrecision !=" not in previous:
                print("x87_guard: unconditional extF80_roundingPrecision write")
                return 1
        if re.match(r"\s*softfloat_exceptionFlags\s*=", line):
            previous = lines[index - 1] if index else ""
            if "if (softfloat_exceptionFlags)" not in previous:
                print("x87_guard: unconditional softfloat_exceptionFlags write")
                return 1

    if "if (softfloat_exceptionFlags)" not in end:
        print("x87_guard: x87_end must clear flags conditionally")
        return 1
    end_lines = end.splitlines()
    for index, line in enumerate(end_lines):
        if re.match(r"\s*softfloat_exceptionFlags\s*=", line):
            previous = end_lines[index - 1] if index else ""
            if "if (softfloat_exceptionFlags)" not in previous:
                print("x87_guard: unconditional x87_end flag clear")
                return 1

    print("x87_guard: publication guard OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
