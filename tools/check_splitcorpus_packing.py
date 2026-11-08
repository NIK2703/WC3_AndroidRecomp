#!/usr/bin/env python3
"""Source check for O-23 component-aware chunk packing.

This does not replace a rebuild of splitcorpus: it only catches accidental
removal of the planner that keeps a fitting call-graph component on one side
of a chunk boundary.  The executable test `splitcorpus_order` still owns the
runtime ordering check.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "lift" / "csrc_local" / "splitcorpus.c"

REQUIRED = (
    "comp_end[ncomp++] = norder;",
    "for (ci = 0; ci < ncomp; ci++)",
    "size_t comp_stop = comp_end[ci];",
    "int fits_fresh;",
    "if (out && fits_fresh && count > 0",
    "comp_start = comp_stop;",
    "free(comp_end);",
)


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8", errors="replace")
    missing = [fragment for fragment in REQUIRED if fragment not in text]
    if missing:
        print("splitcorpus_packing: missing component-aware planner:")
        for fragment in missing:
            print(f"  {fragment}")
        return 1
    print("splitcorpus_packing: component-aware planner OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
