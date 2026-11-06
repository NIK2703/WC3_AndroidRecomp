"""Canonical x86-32 static recompiler tooling.

The public entry point is `tools/wc3lift.py`; the modules here are the PE
reader, the function inventory, the instruction lifter, the corpus manifest and
the equivalence checker.
"""

LIFT_REV = 1
GENERATOR = "wc3lift"
