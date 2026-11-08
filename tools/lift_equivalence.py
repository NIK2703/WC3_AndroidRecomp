#!/usr/bin/env python3
"""Documented entry point for the G-15 lift-equivalence checker.

The implementation lives in `tools/lift/check_equivalence.py` next to the
Python reference lifter.  This wrapper keeps the path named by
`docs/VERIFICATION.md` (G-15) stable for CI and for the Step 5b command line.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lift.check_equivalence import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
