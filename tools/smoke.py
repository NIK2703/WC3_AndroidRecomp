#!/usr/bin/env python3
"""Capture and verify the pre-build baseline.

The baseline records source/config hashes, not build products.  A missing PE or
device is stored as `unavailable`; verify never converts that into `pass`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = [
    "config/imports.1.27b.json",
    "config/pe_imports.1.27b.json",
    "config/hle_arity.1.27b.json",
    "config/cdecl_allowlist.1.27b.json",
    "config/hle_table.1.27b.tsv",
    "docs/ARCHITECTURE_1.27B.md",
    "docs/THREAD_AUDIT.md",
    "runtime/hle/abi/hle_abi.h",
    "runtime/hle/abi/hle_abi.c",
    "runtime/guest/guest_layout.h",
    "runtime/platform/platform.h",
]
UNAVAILABLE = [
    ("windows_reference_host", "runtime/host_ref and wc3_host27.exe are not ported yet"),
    ("android_device", "no adb device and no Android SDK/NDK in this workspace"),
    ("android_build", "no Android SDK/NDK; host checks only"),
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def marker_tail(path: Path) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines:
        return ""
    return lines[-1][:240]


def record(out: Path, phase: str) -> int:
    rows = []
    for rel in ARTIFACTS:
        path = ROOT / rel
        if not path.exists():
            rows.append({"path": rel, "status": "missing"})
            continue
        size = path.stat().st_size
        if size == 0:
            rows.append({"path": rel, "status": "empty"})
            continue
        rows.append(
            {
                "path": rel,
                "status": "ok",
                "size": size,
                "sha256": sha256_file(path),
                "marker_tail": marker_tail(path),
            }
        )
    payload = {
        "schema": 1,
        "corpus": "1.27b",
        "phase": phase,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "artifacts": rows,
        "unavailable": [{"id": i, "reason": r} for i, r in UNAVAILABLE],
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    ok = sum(1 for r in rows if r["status"] == "ok")
    print(f"recorded {out}: ok={ok} unavailable={len(UNAVAILABLE)}")
    return 0


def verify(baseline: Path) -> int:
    data = json.loads(baseline.read_text(encoding="utf-8"))
    errors: list[str] = []
    for row in data.get("artifacts", []):
        rel = row["path"]
        path = ROOT / rel
        old = row.get("status")
        if not path.exists():
            errors.append(f"{rel}: missing (baseline {old})")
            continue
        if path.stat().st_size == 0:
            errors.append(f"{rel}: empty (baseline {old})")
            continue
        if old != "ok":
            errors.append(f"{rel}: baseline status {old}, file is now present")
            continue
        got = sha256_file(path)
        if got != row.get("sha256"):
            errors.append(f"{rel}: sha256 drift")
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    unavailable = data.get("unavailable", [])
    for item in unavailable:
        print(f"unavailable (not pass): {item['id']}: {item['reason']}")
    print("baseline hashes stable")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    rec = sub.add_parser("record")
    rec.add_argument("--phase", required=True, choices=["before", "after"])
    rec.add_argument("--out", type=Path, required=True)
    ver = sub.add_parser("verify")
    ver.add_argument("--baseline", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.cmd == "record":
        return record(args.out, args.phase)
    return verify(args.baseline)


if __name__ == "__main__":
    raise SystemExit(main())
