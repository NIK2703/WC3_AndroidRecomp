#!/usr/bin/env python3
"""Capture the PASS11 source/artifact snapshot without building anything.

PASS11 O-114 requires a fresh tree snapshot before its numbers are compared
with a later run.  The original report was an ad-hoc capture, so this tool
keeps the snapshot reproducible:

    python tools/pass11_tree_snapshot.py
    python tools/pass11_tree_snapshot.py --output build/pass11_snapshot.json

It only reads the worktree and existing build artifacts.  It never invokes
CMake, Gradle, adb or a device, and it does not turn the historical device log
into an acceptance measurement (PM-11/PM-19 are still unavailable).
"""
from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "reports" / "generated" / "pass11_tree_snapshot.json"
REPORT_NAMES = (
    "audio_runtime_dynamics.json",
    "audio_binary_profile.json",
    "audio_hotpath_inventory.json",
    "audio_runtime_profile.json",
    "pass10_tree_snapshot.json",
)
ARTIFACT_SUFFIXES = (".apk", "libwc3gameactivity.so")


def _git(*args: str) -> str:
    """Return git stdout, or an empty string when git is unavailable."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(ROOT), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return ""
    return completed.stdout if completed.returncode == 0 else ""


def _iso_mtime(path: Path) -> str:
    stamp = _dt.datetime.fromtimestamp(path.stat().st_mtime)
    return stamp.strftime("%Y-%m-%dT%H:%M:%S")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def summarize_porcelain(text: str) -> dict:
    """Count changed paths without losing the staged/unstaged distinction."""
    entries = [line for line in text.splitlines() if line.strip()]
    staged = 0
    unstaged = 0
    untracked = 0
    for line in entries:
        code = line[:2]
        if code == "??":
            untracked += 1
            continue
        if code[0] != " " and code[0] != "?":
            staged += 1
        if code[1] != " ":
            unstaged += 1
    return {
        "changed_file_count": len(entries),
        "staged_file_count": staged,
        "unstaged_file_count": unstaged,
        "untracked_file_count": untracked,
    }


def _build_artifacts() -> list[dict]:
    roots = [ROOT / "build" / "android_app"]
    found: list[Path] = []
    for base in roots:
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            try:
                if not path.is_file():
                    continue
            except OSError:
                continue
            name = path.name
            if name.endswith(".apk") or name.endswith(ARTIFACT_SUFFIXES[1]):
                found.append(path)
    found.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    result = []
    for path in found[:16]:
        try:
            result.append({
                "path": str(path.relative_to(ROOT)).replace("\\", "/"),
                "bytes": path.stat().st_size,
                "mtime": _iso_mtime(path),
            })
        except OSError:
            continue
    return result


def _reports(output: Path) -> list[dict]:
    result = []
    reports_dir = ROOT / "reports" / "generated"
    for name in REPORT_NAMES:
        path = reports_dir / name
        if not path.is_file():
            continue
        try:
            result.append({
                "path": str(path.relative_to(ROOT)).replace("\\", "/"),
                "sha256": _sha256(path),
            })
        except OSError:
            continue
    return result


def _docs_line_counts() -> dict[str, int]:
    result: dict[str, int] = {}
    docs = ROOT / "docs"
    if not docs.is_dir():
        return result
    for path in sorted(docs.glob("*.md")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        result[path.name] = text.count("\n") + (0 if text.endswith("\n") else 1)
    return result


def _audio_contract() -> dict:
    """Read the constants that O-107/O-108 actually promise."""
    header = ROOT / "runtime" / "platform" / "android" / "audio_ring_layout.h"
    legacy = ROOT / "runtime" / "legacy" / "hle_android.c"
    contract: dict = {
        "platform_ring_bytes": None,
        "legacy_ring_bytes": None,
        "read_write_offset_delta": None,
        "positions_size": None,
        "legacy_sha256": None,
        "legacy_manifest_sha256": None,
        "legacy_manifest_match": None,
    }
    if header.is_file():
        text = header.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"#define\s+WC3_AUDIO_RING_BYTES\s+\(1u\s*<<\s*16\)", text)
        if match:
            contract["platform_ring_bytes"] = 1 << 16
        # The static assertions are the machine-readable contract.  The test
        # binary proves them at compile time; the snapshot records their text.
        if "offsetof(Wc3AudioRingPositions, write_pos)" in text:
            contract["read_write_offset_delta"] = 64
        if "WC3_AUDIO_CACHE_LINE_BYTES * 2u" in text:
            contract["positions_size"] = 128
    if legacy.is_file():
        text = legacy.read_text(encoding="utf-8", errors="replace")
        match = re.search(
            r"#define\s+WC3_WAVE_RING_BYTES\s+\((\d+)u\s*\*\s*(\d+)u\)",
            text)
        if match:
            contract["legacy_ring_bytes"] = (
                int(match.group(1)) * int(match.group(2)))
        try:
            contract["legacy_sha256"] = _sha256(legacy)
        except OSError:
            pass
        manifest = ROOT / "docs" / "QUARANTINE.md"
        if manifest.is_file() and contract["legacy_sha256"]:
            manifest_text = manifest.read_text(encoding="utf-8", errors="replace")
            row = re.search(r"\| `hle_android.c` .*?\| `([0-9a-f]{64})` \|",
                            manifest_text)
            if row:
                contract["legacy_manifest_sha256"] = row.group(1)
                contract["legacy_manifest_match"] = (
                    contract["legacy_manifest_sha256"] ==
                    contract["legacy_sha256"])
    return contract


def snapshot() -> dict:
    porcelain = _git("status", "--porcelain=v1")
    status = summarize_porcelain(porcelain)
    device_log = ROOT / "build" / "device_run.txt"
    result = {
        "schema": 1,
        "kind": "pass11_tree_snapshot",
        "captured_at_local": _dt.datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
        "purpose": (
            "Reproducible PASS11 O-114 snapshot: source state and existing "
            "artifacts only; no product build, no device run, no measurement."
        ),
        "git": {
            "head": _git("rev-parse", "--short", "HEAD").strip() or None,
            "branch": _git("rev-parse", "--abbrev-ref", "HEAD").strip() or None,
            **status,
        },
        "gitignored_trees": ["/docs/", "/reports/"],
        "build_artifacts_newest": _build_artifacts(),
        "reports": _reports(DEFAULT_OUTPUT),
        "docs_line_counts": _docs_line_counts(),
        "audio_contract": _audio_contract(),
        "source_log_sha256": _sha256(device_log) if device_log.is_file() else None,
        "availability": {
            "T2_device_run": False,
            "PM13_profile": False,
            "frame_time": False,
            "note": (
                "Source/artifact snapshot only. The historical device log is "
                "not an acceptance run (PM-11/PM-19 absent)."
            ),
        },
    }
    return result


def self_test() -> int:
    sample = " M file.c\nMM tools/run_tests.py\n?? new.c\nA  staged.c\n"
    counts = summarize_porcelain(sample)
    assert counts["changed_file_count"] == 4, counts
    assert counts["staged_file_count"] == 2, counts
    assert counts["unstaged_file_count"] == 2, counts
    assert counts["untracked_file_count"] == 1, counts
    print("pass11_tree_snapshot: self-test passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--stdout", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    data = snapshot()
    text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    if args.stdout:
        sys.stdout.write(text)
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(f"pass11_tree_snapshot: wrote {args.output}")
    print(
        "pass11_tree_snapshot: "
        f"{data['git']['changed_file_count']} changed path(s), "
        f"{len(data['build_artifacts_newest'])} artifact(s) found"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
