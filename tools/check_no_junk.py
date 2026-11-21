#!/usr/bin/env python3
"""Reject runtime logs, screenshots and device dumps in a commit.

The tree keeps source, fixtures and vendored inputs; run artifacts belong in
an ignored local directory.  The pre-commit hook checks the staged set, and
the same checker can scan the tracked tree in CI:

    python3 tools/check_no_junk.py --staged
    python3 tools/check_no_junk.py --tracked

A large staged file is rejected too: a renamed 20+ MiB dump is still a dump.
Use `git commit --no-verify` only for a deliberate, reviewed exception.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 20 * 1024 * 1024
LARGE_ALLOW = ("third_party/", "oracle/")

# Root-level names are anchored: a source file called run_notes.txt under a
# module is not a run log.  Generic extensions are checked everywhere.
JUNK_PATTERNS = tuple(
    re.compile(pattern)
    for pattern in (
        r"(^|/)logcat[^/]*\.txt$",
        r"(^|/)device_logcat[^/]*\.txt$",
        r"^game_mode[^/]*\.txt$",
        r"^grid_[^/]*\.png$",
        r"^run[^/]*\.txt$",
        r"^screen[^/]*\.(raw|png)$",
        r"^sel[0-9]+_trace[^/]*\.txt$",
        r"^sf[0-9]+\.txt$",
        r"^thr[0-9]+\.bin$",
        r"^wc3_log[^/]*\.txt$",
        r"\.log$",
        r"\.raw$",
        r"\.dmp$",
        r"(^|/)core\.[0-9]+$",
        r"(^|/)vgcore\.[0-9]+$",
    )
)


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout


def staged_paths() -> list[str]:
    output = git("diff", "--cached", "--name-only", "--diff-filter=ACMR")
    return [line for line in output.splitlines() if line]


def tracked_paths() -> list[str]:
    output = git("ls-files")
    return [line for line in output.splitlines() if line]


def file_size(rel: str) -> int:
    try:
        return (ROOT / rel).stat().st_size
    except OSError:
        return 0


def collect(paths: list[str]) -> list[str]:
    problems: list[str] = []
    for rel in paths:
        for pattern in JUNK_PATTERNS:
            if pattern.search(rel):
                problems.append(f"{rel}: runtime artifact ({pattern.pattern})")
                break
        else:
            if not rel.startswith(LARGE_ALLOW):
                size = file_size(rel)
                if size > MAX_BYTES:
                    problems.append(
                        f"{rel}: {size} bytes exceeds the {MAX_BYTES}-byte "
                        "staging limit"
                    )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--staged",
        action="store_true",
        help="check files staged for the next commit",
    )
    source.add_argument(
        "--tracked",
        action="store_true",
        help="check every tracked file (CI / tree audit)",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="explicit repo-relative paths to check instead of a git set",
    )
    args = parser.parse_args()

    if args.paths:
        paths = args.paths
    elif args.staged:
        paths = staged_paths()
    else:
        paths = tracked_paths()

    problems = collect(paths)
    if not problems:
        return 0

    print("junk files are not allowed in the tree or in a commit:", file=sys.stderr)
    for problem in problems:
        print(f"  {problem}", file=sys.stderr)
    print(
        "Move run output to an ignored local directory.  If a file is a "
        "deliberate fixture, add a justified exception instead of forcing it.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
