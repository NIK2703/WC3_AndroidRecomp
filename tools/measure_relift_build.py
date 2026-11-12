#!/usr/bin/env python3
"""Measure an already-built relift corpus without rebuilding it (O-20/PM-22).

The measurement is static: object sizes, helper symbols, `.text` bytes, the
recorded Ninja compile window and the selected optimisation level.  It never
runs CMake, Ninja, Gradle, ccache or a compiler, and it makes no speed claim.
A missing linker RSS sample is reported as `unavailable`, not as zero.

Usage:
  python tools/measure_relift_build.py \
      --build-dir build/android_app/debug/cmake \
      --corpus build/corpus-full \
      --out reports/generated/o20_relift_build.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOCUS_SYMBOLS = (
    "parity8",
    "flags_add",
    "flags_sub",
    "flags_logicz",
    "rd32",
    "op_shl",
    "x87_add",
    "sse_load_ss",
)
HELPER_PREFIXES = ("flags_", "op_", "x87_", "sse_", "mm_", "rd", "wr")
_SECTION_RE = re.compile(
    r"^\s*\d+\s+(\.[^\s]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            h.update(block)
    return h.hexdigest()


def last_optimization(flags: str) -> str | None:
    """Return the last -O flag, because the compiler uses the last one."""
    found = re.findall(r"(?<!\S)-O[0-3sg](?!\S)", flags)
    return found[-1] if found else None


def read_sample_flags(build_ninja: Path, object_name: str) -> str | None:
    """Read the FLAGS line for one relifted object from build.ninja."""
    text = build_ninja.read_text(encoding="utf-8", errors="replace")
    marker = f"build CMakeFiles/wc3_corpus_relifted.dir/"
    for block in text.split("\nbuild "):
        if object_name not in block:
            continue
        for line in block.splitlines():
            if line.startswith("  FLAGS = "):
                return line[len("  FLAGS = "):]
    return None


def defined_symbols(nm: str, obj: Path) -> set[str]:
    out = subprocess.run([nm, "--defined-only", str(obj)],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        raise SystemExit(f"measure_relift_build: nm failed for {obj}")
    symbols = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            symbols.add(parts[2])
    return symbols


def text_sections(objdump: str, path: Path) -> list[tuple[str, int]]:
    out = subprocess.run([objdump, "-h", str(path)],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        raise SystemExit(f"measure_relift_build: objdump failed for {path}")
    rows = []
    for line in out.stdout.splitlines():
        match = _SECTION_RE.match(line)
        if match:
            rows.append((match.group(1), int(match.group(2), 16)))
    return rows


def helper_sections(rows: list[tuple[str, int]]) -> list[tuple[str, int]]:
    result = []
    for name, size in rows:
        if not name.startswith(".text."):
            continue
        base = name[len(".text."):]
        if base.startswith(HELPER_PREFIXES):
            result.append((base, size))
    return result


def parse_ninja_log(path: Path) -> dict[str, object]:
    """Return the last recorded compile interval for each corpus object."""
    entries: dict[str, tuple[int, int]] = {}
    if not path.is_file():
        return {"status": "unavailable", "objects": 0}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        if "wc3_corpus_relifted.dir" not in parts[3] or "relifted" not in parts[3]:
            continue
        try:
            start, end = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        entries[parts[3]] = (start, end)
    if not entries:
        return {"status": "unavailable", "objects": 0}
    intervals = list(entries.values())
    return {
        "status": "available",
        "objects": len(intervals),
        "cpu_seconds": round(sum(end - start for start, end in intervals) /
                             1000.0, 3),
        "wall_seconds": round((max(end for _, end in intervals) -
                               min(start for start, _ in intervals)) / 1000.0,
                              3),
    }


def count_list_entries(path: Path) -> int | None:
    if not path.is_file():
        return None
    data = path.read_bytes()
    return len(re.findall(rb"(?m)^\s*X\([0-9A-Fa-f]+\)", data))


def measure(build_dir: Path, corpus: Path, sample_name: str) -> dict[str, object]:
    nm = shutil.which("nm")
    objdump = shutil.which("objdump")
    if not nm or not objdump:
        raise SystemExit("measure_relift_build: nm and objdump are required")

    relifted = build_dir / "relifted"
    chunks = sorted(relifted.rglob("*.c"))
    objects = sorted((build_dir / "CMakeFiles" / "wc3_corpus_relifted.dir")
                     .rglob("*.o"))
    if not chunks or not objects:
        raise SystemExit(f"measure_relift_build: no corpus under {build_dir}")

    functions = 0
    for path in chunks:
        functions += len(re.findall(
            rb"(?m)^void L_[0-9A-Fa-f]+\(CPU \*c\)", path.read_bytes()))
    list_entries = count_list_entries(corpus / "wc3_all_list.h")

    flags = read_sample_flags(build_dir / "build.ninja", sample_name)
    optimization = last_optimization(flags or "")

    focus_objects = {name: 0 for name in FOCUS_SYMBOLS}
    helper_bytes = 0
    helper_counts: dict[str, int] = {}
    for obj in objects:
        symbols = defined_symbols(nm, obj)
        for name in FOCUS_SYMBOLS:
            if name in symbols:
                focus_objects[name] += 1
        for base, size in helper_sections(text_sections(objdump, obj)):
            helper_bytes += size
            helper_counts[base] = helper_counts.get(base, 0) + 1

    sample = next((p for p in objects if p.name == sample_name), None)
    if sample is None:
        raise SystemExit(f"measure_relift_build: sample object not found: "
                         f"{sample_name}")
    sample_rows = text_sections(objdump, sample)
    sample_helpers = helper_sections(sample_rows)

    so = build_dir / "libwc3gameactivity.so"
    archive = build_dir / "libwc3_corpus_relifted.a"
    so_rows = text_sections(objdump, so) if so.is_file() else None
    so_text = (sum(size for name, size in so_rows
                   if name == ".text" or name.startswith(".text."))
               if so_rows is not None else None)
    identity = {
        "so_sha256": sha256_file(so) if so.is_file() else None,
        "archive_sha256": sha256_file(archive) if archive.is_file() else None,
    }
    identity_hash = hashlib.sha256(
        json.dumps(identity, sort_keys=True).encode()).hexdigest()

    ninja = parse_ninja_log(build_dir / ".ninja_log")
    checks = {
        "optimization": {
            "value": optimization,
            "expected": "-O2",
            "status": "pass" if optimization == "-O2" else "mismatch",
        },
        "functions": {
            "value": functions,
            "expected": list_entries,
            "status": ("pass" if functions == list_entries else "mismatch"),
        },
        "parity8_objects": {
            "value": focus_objects["parity8"],
            "expected": 0,
            "status": ("pass" if focus_objects["parity8"] == 0
                       else "mismatch"),
        },
        "chunk_object_count": {
            "value": len(objects),
            "expected": len(chunks),
            "status": ("pass" if len(objects) == len(chunks)
                       else "mismatch"),
        },
    }
    status = ("mismatch" if any(c["status"] == "mismatch"
                                for c in checks.values()) else "pass")
    return {
        "schema": "wc3.relift_build_measurement.v1",
        "status": "report-only",
        "verdict": status,
        "artifact_sha256": identity_hash,
        "build_dir": str(build_dir).replace("\\", "/"),
        "sample_object": sample_name,
        "sample_flags": flags,
        "optimization": optimization,
        "chunks": len(chunks),
        "objects": len(objects),
        "functions": functions,
        "list_entries": list_entries,
        "sample_text_sections": len(sample_rows),
        "sample_helper_sections": len(sample_helpers),
        "sample_helper_bytes": sum(size for _, size in sample_helpers),
        "all_helper_bytes": helper_bytes,
        "focus_objects": focus_objects,
        "helper_section_counts": dict(sorted(helper_counts.items())),
        "object_bytes": sum(p.stat().st_size for p in objects),
        "archive_bytes": archive.stat().st_size if archive.is_file() else None,
        "so_bytes": so.stat().st_size if so.is_file() else None,
        "so_text_bytes": so_text,
        "so_text_status": ("available" if so_text is not None else
                           "unavailable"),
        "compile": ninja,
        "link_peak_rss_mb": None,
        "link_peak_rss_status": "unavailable",
        "identity": identity,
        "checks": checks,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path,
                        default=ROOT / "build" / "android_app" / "debug" / "cmake")
    parser.add_argument("--corpus", type=Path,
                        default=ROOT / "build" / "corpus-full")
    parser.add_argument("--sample", default="game_closed_0000.c.o")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)

    result = measure(args.build_dir, args.corpus, args.sample)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8", newline="\n")
        print(f"measure_relift_build: {result['chunks']} chunks, "
              f"{result['objects']} objects -> {args.out}")
    else:
        print(text, end="")
    if result["verdict"] == "mismatch":
        print("measure_relift_build: measurement checks failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
