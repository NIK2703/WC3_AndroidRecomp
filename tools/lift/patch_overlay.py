#!/usr/bin/env python3
"""Compatibility entry point for the general patch pipeline.

The implementation lives in `patch_pipeline.py`.  This wrapper keeps the old
command name usable while the project migrates its build integration.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from patch_pipeline import main


if __name__ == "__main__":
    raise SystemExit(main())
