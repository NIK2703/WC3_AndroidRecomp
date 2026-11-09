#!/usr/bin/env python3
"""Validate the pinned SoftFloat source manifest used by native TLS.

The legacy ``libsoftfloat.a`` is an emulated-TLS archive.  O-52 can only
replace it with a target-tree build when every object named by
``third_party/softfloat/android_sources.txt`` exists exactly once and has a
unique basename.  This is asset-free T0 work: it does not compile anything or
touch the legacy tree.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOFTFLOAT = ROOT / "third_party" / "softfloat"
MANIFEST = SOFTFLOAT / "android_sources.txt"
EXPECTED_COUNT = 302


def main() -> int:
    if not MANIFEST.is_file():
        raise SystemExit(f"missing SoftFloat manifest: {MANIFEST}")

    names: dict[str, Path] = {}
    for raw in MANIFEST.read_text(encoding="ascii").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        rel = line.replace("\\", "/")
        path = SOFTFLOAT / rel
        if not path.is_file():
            raise SystemExit(f"missing SoftFloat source: {path}")
        stem = path.stem
        if stem in names:
            raise SystemExit(
                f"duplicate SoftFloat object name {stem}: "
                f"{names[stem]} and {path}")
        names[stem] = path

    if len(names) != EXPECTED_COUNT:
        raise SystemExit(
            f"SoftFloat manifest has {len(names)} sources, "
            f"expected {EXPECTED_COUNT}")
    print(f"check_softfloat_manifest: OK ({len(names)} sources)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
