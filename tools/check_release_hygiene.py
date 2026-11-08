#!/usr/bin/env python3
"""BV-10 release-hygiene checker (O-72 partial).

The checker reads two existing ELF artifacts, release and debug, and looks for
string markers that distinguish target-side diagnostics from product errors:

* a debug-only marker must be absent from release and present in debug;
* a product marker must be present in both variants.

It does not build, link, strip or patch anything.  Missing artifacts are
reported as ``unavailable`` rather than as a pass.  The marker list is
deliberately small and stable; add a marker only together with the source
change that makes it a contract.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RELEASE = ROOT / "build" / "android_app" / "release" / "cmake" / \
    "libwc3gameactivity.so"
DEFAULT_DEBUG = ROOT / "build" / "android_app" / "debug" / "cmake" / \
    "libwc3gameactivity.so"

# Diagnostics that must not survive into a release APK.
DEBUG_ONLY_MARKERS = (
    "unknown guest target",
    "[wave2] disabled: as-built WINMM path",
    "[dispatch]",
)

# Target errors that are part of the product, not diagnostics.  They must stay
# in both variants so a release failure remains diagnosable.
PRODUCT_MARKERS = (
    "[wave2] USER32 rows missing",
    "[wave2] missing WINMM row",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def count_markers(path: Path, markers: Sequence[str]) -> dict[str, int]:
    data = path.read_bytes()
    return {marker: data.count(marker.encode("utf-8")) for marker in markers}


def check_files(release: Path, debug: Path) -> dict[str, object]:
    release_counts = count_markers(
        release, (*DEBUG_ONLY_MARKERS, *PRODUCT_MARKERS)
    )
    debug_counts = count_markers(
        debug, (*DEBUG_ONLY_MARKERS, *PRODUCT_MARKERS)
    )
    errors: list[str] = []
    rows: list[dict[str, object]] = []

    for marker in DEBUG_ONLY_MARKERS:
        r = release_counts[marker]
        d = debug_counts[marker]
        row = {
            "marker": marker,
            "kind": "debug-only",
            "release": r,
            "debug": d,
        }
        rows.append(row)
        if r != 0:
            errors.append(
                f"release contains debug-only marker {marker!r} ({r} times)"
            )
        if d == 0:
            errors.append(
                f"debug is missing debug-only marker {marker!r}"
            )

    for marker in PRODUCT_MARKERS:
        r = release_counts[marker]
        d = debug_counts[marker]
        row = {
            "marker": marker,
            "kind": "product",
            "release": r,
            "debug": d,
        }
        rows.append(row)
        if r == 0:
            errors.append(
                f"release is missing product marker {marker!r}"
            )
        if d == 0:
            errors.append(
                f"debug is missing product marker {marker!r}"
            )

    return {
        "release": str(release),
        "debug": str(debug),
        "release_sha256": sha256(release),
        "debug_sha256": sha256(debug),
        "rows": rows,
        "errors": errors,
    }


def print_report(report: dict[str, object]) -> None:
    print(f"release: {report['release']}")
    print(f"  sha256 {report['release_sha256']}")
    print(f"debug:   {report['debug']}")
    print(f"  sha256 {report['debug_sha256']}")
    for row in report["rows"]:  # type: ignore[assignment]
        print(
            "  {kind:10s} release={release:<3d} debug={debug:<3d} {marker}".format(
                **row
            )
        )
    errors = report["errors"]
    if errors:
        print("release hygiene: FAILED")
        for error in errors:  # type: ignore[union-attr]
            print(f"  - {error}")
    else:
        print("release hygiene: ok")


def run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="wc3_release_hygiene_") as tmp:
        root = Path(tmp)
        debug = root / "debug.bin"
        release = root / "release.bin"
        debug.write_text(
            "\n".join((*DEBUG_ONLY_MARKERS, *PRODUCT_MARKERS)),
            encoding="utf-8",
        )
        release.write_text("\n".join(PRODUCT_MARKERS), encoding="utf-8")

        report = check_files(release, debug)
        if report["errors"]:
            print("release hygiene self-test: false failure")
            return 1

        release.write_text(
            "\n".join((*PRODUCT_MARKERS, DEBUG_ONLY_MARKERS[0])),
            encoding="utf-8",
        )
        report = check_files(release, debug)
        if not report["errors"]:
            print("release hygiene self-test: false pass")
            return 1

    print("release hygiene self-test: ok")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, default=DEFAULT_RELEASE)
    parser.add_argument("--debug", type=Path, default=DEFAULT_DEBUG)
    parser.add_argument(
        "--json",
        type=Path,
        help="write the full report (including hashes and counts) as JSON",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check the checker on synthetic marker files, no ELF needed",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        return run_self_test()

    missing = [str(path) for path in (args.release, args.debug)
               if not path.is_file()]
    if missing:
        print("release hygiene: unavailable (missing artifacts)")
        for path in missing:
            print(f"  {path}")
        return 2

    report = check_files(args.release, args.debug)
    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"report: {args.json}")
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
