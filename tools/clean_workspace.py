#!/usr/bin/env python3
"""Remove regenerable workspace junk without touching reusable build caches.

The default mode is a dry run.  ``--apply`` deletes the listed paths.  The
scanner deliberately targets only artefacts that are recreated by the normal
build or test flow:

* retired patch overlays (``*.stale-*`` and ``.work.stale-*``) under ``build/``
* Python bytecode caches (``__pycache__`` and stray ``*.pyc``)
* Kotlin/Gradle compiler error logs (the Gradle/Kotlin caches stay in place)
* APK signature sidecars (``*.idsig``) under ``build/``
* optionally, old device logs and screenshots under the run-artifact folders

Reusable caches are never candidates: ccache objects, lift tools, the generated
corpus, PGO/profiling data, Gradle caches, and the current CMake build tree are
left untouched.  The patch pipeline itself also retires its work tree after a
successful run, so this tool is a safety net rather than the only cleanup path.

Usage:
    python3 tools/clean_workspace.py
    python3 tools/clean_workspace.py --apply
    python3 tools/clean_workspace.py --include-run-artifacts --older-than-days 7 --apply
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_ARTIFACT_DIRS = (
    ROOT / "build" / "device_logs",
    ROOT / "build" / "hpbar",
    ROOT / "build" / "sel24_test",
)
RUN_ARTIFACT_SUFFIXES = {".log", ".txt", ".png", ".raw", ".dmp", ".bin"}
KOTLIN_ERROR_DIRS = (
    ROOT / "apps" / "android_gameactivity" / ".kotlin" / "errors",
    ROOT / "apps" / "android_gameactivity" / ".gradle" / "kotlin" / "errors",
)
SKIP_DIR_NAMES = {".git", "third_party"}


@dataclass(frozen=True)
class Candidate:
    path: Path
    kind: str


def walk(base: Path, *, prune: set[str] | None = None,
         prune_stale: bool = False):
    """Yield (directory, dirnames, filenames), pruning heavy/foreign trees."""
    prune = prune or set()
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [
            name for name in dirnames
            if name not in SKIP_DIR_NAMES
            and name not in prune
            and not (prune_stale and ".stale-" in name)
        ]
        yield Path(dirpath), dirnames, filenames


def stale_dirs() -> list[Candidate]:
    out: list[Candidate] = []
    build = ROOT / "build"
    if not build.is_dir():
        return out
    for directory, dirnames, _ in walk(build):
        for name in list(dirnames):
            if ".stale-" in name:
                out.append(Candidate(directory / name, "retired overlay"))
                dirnames.remove(name)
    return out


def python_caches() -> list[Candidate]:
    out: list[Candidate] = []
    for directory, dirnames, filenames in walk(ROOT, prune={"build"}):
        for name in list(dirnames):
            if name == "__pycache__":
                out.append(Candidate(directory / name, "python cache"))
                dirnames.remove(name)
        for name in filenames:
            if name.endswith(".pyc"):
                out.append(Candidate(directory / name, "python bytecode"))
    return out


def kotlin_error_logs() -> list[Candidate]:
    out: list[Candidate] = []
    for directory in KOTLIN_ERROR_DIRS:
        if directory.is_dir():
            out.extend(
                Candidate(path, "kotlin error log")
                for path in sorted(directory.glob("*.log"))
            )
    return out


def apk_sidecars() -> list[Candidate]:
    build = ROOT / "build"
    if not build.is_dir():
        return []
    out: list[Candidate] = []
    for directory, _, filenames in walk(build, prune_stale=True):
        out.extend(
            Candidate(directory / name, "apk signature sidecar")
            for name in sorted(filenames)
            if name.endswith(".idsig")
        )
    return out


def run_artifacts(cutoff: float) -> list[Candidate]:
    out: list[Candidate] = []
    for directory in RUN_ARTIFACT_DIRS:
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if (
                path.is_file()
                and path.suffix.lower() in RUN_ARTIFACT_SUFFIXES
                and path.stat().st_mtime < cutoff
            ):
                out.append(Candidate(path, "old run artifact"))
    return out


def remove_path(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true",
                        help="delete the candidates (default: dry run)")
    parser.add_argument("--check", action="store_true",
                        help="dry run that exits 1 when anything is found")
    parser.add_argument("--include-run-artifacts", action="store_true",
                        help="also remove old logs/screenshots from run folders")
    parser.add_argument("--older-than-days", type=float, default=7.0,
                        help="age threshold for --include-run-artifacts "
                             "(default: 7 days)")
    args = parser.parse_args(argv)

    candidates: list[Candidate] = []
    candidates.extend(stale_dirs())
    candidates.extend(python_caches())
    candidates.extend(kotlin_error_logs())
    candidates.extend(apk_sidecars())
    if args.include_run_artifacts:
        cutoff = time.time() - max(args.older_than_days, 0.0) * 86400.0
        candidates.extend(run_artifacts(cutoff))

    # A path can match more than one category (a stale overlay may contain a
    # pycache); keep one deletion per path.
    unique: dict[Path, Candidate] = {}
    for item in candidates:
        unique.setdefault(item.path, item)
    ordered = sorted(unique.values(), key=lambda item: str(item.path))

    if not ordered:
        print("clean_workspace: no junk found")
        return 0

    for item in ordered:
        print(f"{item.kind}: {item.path.relative_to(ROOT)}")

    if args.apply:
        failed = 0
        for item in ordered:
            try:
                remove_path(item.path)
            except OSError as exc:
                failed += 1
                print(f"clean_workspace: failed to remove {item.path}: {exc}",
                      file=sys.stderr)
        print(f"clean_workspace: removed {len(ordered) - failed} item(s)")
        return 1 if failed else 0

    print(f"clean_workspace: {len(ordered)} item(s); dry run "
          "(use --apply to delete)")
    return 1 if args.check else 0


if __name__ == "__main__":
    raise SystemExit(main())
