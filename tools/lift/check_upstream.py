#!/usr/bin/env python3
"""Verify the pinned M5 toolchain inputs (BP-4.2, R19).

Compares every file listed in config/build_upstream.json with the bytes in
this tree.  A mismatch means someone edited a vendored C source in place;
fix it upstream or update the pin in the same commit, never silently.

Exit codes: 0 pass, 1 fail, 2 unavailable (manifest missing).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "config" / "build_upstream.json"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", type=Path, help="write a machine-readable report")
    args = ap.parse_args(argv)

    if not MANIFEST.is_file():
        print("upstream: unavailable (config/build_upstream.json missing)", file=sys.stderr)
        return 2
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    upstream = data.get("upstream", {})
    if upstream.get("allow_unpinned") is not False:
        print("upstream: FAIL allow_unpinned must be false", file=sys.stderr)
        return 1
    if not upstream.get("revision"):
        print("upstream: FAIL revision is empty", file=sys.stderr)
        return 1

    errors: list[str] = []
    checked = 0
    for component in data.get("components", []):
        base = ROOT / component["path"]
        for entry in component["files"]:
            path = base / entry["name"]
            if not path.is_file():
                errors.append(f"missing {path.relative_to(ROOT).as_posix()}")
                continue
            if path.stat().st_size != entry["bytes"]:
                errors.append(f"size mismatch {path.relative_to(ROOT).as_posix()}")
                continue
            if sha256(path) != entry["sha256"]:
                errors.append(f"sha256 mismatch {path.relative_to(ROOT).as_posix()}")
                continue
            checked += 1

    verdict = "pass" if not errors else "fail"
    report = {
        "schema": 1,
        "revision": upstream["revision"],
        "checked": checked,
        "errors": errors,
        "verdict": verdict,
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for error in errors:
        print(f"upstream: {error}", file=sys.stderr)
    print(f"upstream: {verdict} ({checked} pinned files, rev {upstream['revision'][:12]})")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
