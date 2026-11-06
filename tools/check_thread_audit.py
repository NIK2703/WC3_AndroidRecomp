#!/usr/bin/env python3
"""Check that the thread/sync audit is explicit about what is still unknown.

An audit that has not been run against a licensed PE and an Android device is
`unavailable`, not `pass`.  The gate still checks that every thread/sync
mechanism has a row: deleting a row is not a way to make the audit green.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DOC = ROOT / "docs" / "THREAD_AUDIT.md"

REQUIRED = [
    "CreateThread",
    "ExitThread",
    "WaitForSingleObject",
    "WaitForMultipleObjects",
    "CreateEvent",
    "CreateMutex",
    "CreateSemaphore",
    "Critical sections",
    "TlsAlloc",
    "DllMain",
    "TerminateThread",
]


def split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--doc", type=Path, default=DEFAULT_DOC)
    parser.add_argument("--require-pass", action="store_true")
    parser.add_argument("--allow-unavailable", action="store_true")
    args = parser.parse_args(argv)

    text = args.doc.read_text(encoding="utf-8")
    if f"1.27b" not in text:
        print(f"ERROR: {args.doc} does not name corpus 1.27b", file=sys.stderr)
        return 1

    rows: list[list[str]] = []
    for line in text.splitlines():
        if line.startswith("|") and line.count("|") >= 8:
            cells = split_row(line)
            if cells and set(cells[0]) <= set("-: "):
                continue
            rows.append(cells)

    if not rows:
        print(f"ERROR: no audit table in {args.doc}", file=sys.stderr)
        return 1

    header = rows[0]
    if len(header) != 8:
        print(f"ERROR: expected 8 columns, got {len(header)}", file=sys.stderr)
        return 1
    body = rows[1:]
    by_name = {r[0]: r for r in body if r}
    errors: list[str] = []
    for need in REQUIRED:
        if not any(need in name for name in by_name):
            errors.append(f"missing audit row: {need}")

    unavailable = 0
    for name, row in by_name.items():
        if len(row) != 8:
            errors.append(f"row has {len(row)} columns: {name}")
            continue
        status = row[-1].lower()
        if status not in ("pass", "unavailable"):
            errors.append(f"{name}: status {row[-1]!r} is neither pass nor unavailable")
        if status == "unavailable":
            unavailable += 1
            # No evidence is the expected state before PE/device provisioning.
        elif not row[6] or row[6] in ("—", "-"):
            errors.append(f"{name}: pass with no evidence cell")

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    if unavailable:
        print(f"thread audit: unavailable ({unavailable} rows, not pass)")
        if args.require_pass:
            return 2
        return 0 if args.allow_unavailable else 2
    else:
        print(f"thread audit: pass ({len(by_name)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
