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


def require_guarded_writes(
    body: str,
    writes: tuple[tuple[str, str, str], ...],
    where: str,
) -> int:
    """Check each assignment is directly preceded by its equality guard.

    The guard is deliberately adjacent, not merely present in the function:
    that keeps an unconditional store from hiding behind a conditional one
    that checks a different field.
    """
    lines = body.splitlines()
    for index, line in enumerate(lines):
        for pattern, guard, label in writes:
            if re.match(pattern, line):
                previous = lines[index - 1] if index else ""
                if guard not in previous:
                    print(f"x87_guard: unconditional {label} write in {where}")
                    return 1
    return 0


def main() -> int:
    text = X87.read_text(encoding="utf-8", errors="replace")
    begin = function_body(text, "x87_begin")
    end = function_body(text, "x87_end")

    required = [
        "const uint8_t mode = x87_round_mode(c);",
        "const uint8_t precision = x87_precision(c);",
        "softfloat_state *sf = &c->sf_state;",
        "if (softfloat_roundingMode != mode)",
        "if (extF80_roundingPrecision != precision)",
        "if (softfloat_exceptionFlags)",
        "if (sf->roundingMode != mode)",
        "if (sf->extF80RoundingPrecision != precision)",
        "if (sf->exceptionFlags)",
    ]
    missing = [fragment for fragment in required if fragment not in begin]
    if missing:
        print("x87_guard: missing publication guard:")
        for fragment in missing:
            print(f"  {fragment}")
        return 1

    legacy_begin = (
        (r"\s*softfloat_roundingMode\s*=",
         "if (softfloat_roundingMode !=", "softfloat_roundingMode"),
        (r"\s*extF80_roundingPrecision\s*=",
         "if (extF80_roundingPrecision !=", "extF80_roundingPrecision"),
        (r"\s*softfloat_exceptionFlags\s*=",
         "if (softfloat_exceptionFlags)", "softfloat_exceptionFlags"),
    )
    context_begin = (
        (r"\s*sf->roundingMode\s*=",
         "if (sf->roundingMode !=", "sf->roundingMode"),
        (r"\s*sf->extF80RoundingPrecision\s*=",
         "if (sf->extF80RoundingPrecision !=", "sf->extF80RoundingPrecision"),
        (r"\s*sf->exceptionFlags\s*=",
         "if (sf->exceptionFlags)", "sf->exceptionFlags"),
    )
    if require_guarded_writes(begin, legacy_begin, "x87_begin legacy") != 0:
        return 1
    if require_guarded_writes(begin, context_begin, "x87_begin context") != 0:
        return 1

    if "if (softfloat_exceptionFlags)" not in end:
        print("x87_guard: x87_end must clear SoftFloat flags conditionally")
        return 1
    if "if (c->sf_state.exceptionFlags)" not in end:
        print("x87_guard: x87_end must clear context flags conditionally")
        return 1
    legacy_end = (
        (r"\s*softfloat_exceptionFlags\s*=",
         "if (softfloat_exceptionFlags)", "softfloat_exceptionFlags"),
    )
    context_end = (
        (r"\s*c->sf_state\.exceptionFlags\s*=",
         "if (c->sf_state.exceptionFlags)", "c->sf_state.exceptionFlags"),
    )
    if require_guarded_writes(end, legacy_end, "x87_end legacy") != 0:
        return 1
    if require_guarded_writes(end, context_end, "x87_end context") != 0:
        return 1

    print("x87_guard: publication guard OK (legacy and CPU.sf_state branches)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
