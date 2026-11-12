#!/usr/bin/env python3
"""Static T0 guard for the native x87 / no-SoftFloat product path.

This is intentionally a source-shape check, not a proof of arithmetic.  It
catches the regressions that would silently bring the removed SoftFloat path
back: a re-added include, a CPU.sf_state field, a generated-C call to an
extF80 helper, or a lifter that stops using the native f80/BCD codecs.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPU = ROOT / "runtime" / "cpu"
X87 = CPU / "x87.h"
CPU_H = CPU / "cpu.h"
LIFTER = ROOT / "tools" / "lift" / "lifter32.py"
LIFTER_C = ROOT / "tools" / "lift" / "csrc" / "liftemit.c"

FORBIDDEN = (
    "softfloat.h",
    "softfloat_state.h",
    "float64_t",
    "extFloat80_t",
    "X87_EXTF80_TO_F64",
    "CPU.sf_state",
    "->sf_state",
    "softfloat_exceptionFlags",
    "softfloat_roundingMode",
)

REQUIRED_CPU = (
    "double   d;",
    "uint64_t mm;",
    "_Static_assert(sizeof(x87_ext) == 16",
    "cpu_x87_init_thread",
    "&c->st[i & 7].v.mm",
)

REQUIRED_X87 = (
    "x87_invalid_result",
    "x87_ld_bcd",
    "x87_st_bcd",
    "x87_fprem1",
    "x87_fldcw",
    "x87_fnsave",
    "x87_frstor",
    "x87_fst_reg",
    "x87_d_to_f80",
    "x87_f80_to_d",
)

REQUIRED_LIFTER = (
    "x87_fldcw(c,",
    "x87_ld_bcd(c,",
    "x87_st_bcd(c,",
    "x87_fprem1",
    "x87_fst_reg(c,",
)

REQUIRED_LIFTER_C = (
    "x87_fldcw(c,",
    "x87_ld_bcd(c,",
    "x87_st_bcd(c,",
    "x87_fprem1",
    "x87_fst_reg(c,",
)


def fail(message: str) -> int:
    print(f"x87_guard: {message}", file=sys.stderr)
    return 1


def scan_sources() -> list[str]:
    problems: list[str] = []
    for path in sorted((ROOT / "runtime").rglob("*")):
        if path.suffix not in (".c", ".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in FORBIDDEN:
            if token in text:
                problems.append(f"{path.relative_to(ROOT)} contains {token}")
    return problems


def require(path: Path, needles: tuple[str, ...]) -> list[str]:
    if not path.is_file():
        return [f"missing {path.relative_to(ROOT)}"]
    text = path.read_text(encoding="utf-8", errors="replace")
    return [f"{path.relative_to(ROOT)} missing {needle}"
            for needle in needles if needle not in text]


def main() -> int:
    problems = scan_sources()
    problems += require(CPU_H, REQUIRED_CPU)
    problems += require(X87, REQUIRED_X87)
    problems += require(LIFTER, REQUIRED_LIFTER)
    problems += require(LIFTER_C, REQUIRED_LIFTER_C)

    # The old per-op SoftFloat publication path must not come back.
    if X87.is_file():
        text = X87.read_text(encoding="utf-8", errors="replace")
        if re.search(r"\bx87_(begin|end)\s*\(", text):
            problems.append("x87.h still contains x87_begin/x87_end")

    if problems:
        for problem in problems:
            print(f"x87_guard: {problem}", file=sys.stderr)
        return 1
    print("x87_guard: native x87, no SoftFloat references")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
