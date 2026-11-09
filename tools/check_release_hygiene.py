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
import re
import sys
import tempfile
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RELEASE = ROOT / "build" / "android_app" / "release" / "cmake" / \
    "libwc3gameactivity.so"
DEFAULT_DEBUG = ROOT / "build" / "android_app" / "debug" / "cmake" / \
    "libwc3gameactivity.so"

# Diagnostics that must not survive into a release APK.  O-91 makes the whole
# v2 sink debug/headless-only, so its install errors are debug markers now.
DEBUG_ONLY_MARKERS = (
    "unknown guest target",
    "[wave2] disabled: as-built WINMM path",
    "[wave2] USER32 rows missing",
    "[wave2] missing WINMM row",
    "[dispatch]",
)

# Target errors that are part of the product, not diagnostics.  They must stay
# in both variants so a release failure remains diagnosable.  There are none
# for the audio path after O-91: the product fast-bind module has no
# unconditional error string, and v2 is absent from release.
PRODUCT_MARKERS = ()

# O-72 site-level companion to the marker scan.  These are the only ungated
# stderr sites in the product tree that are product errors rather than
# diagnostics.  Everything else must be inside a WC3_DEBUG guard; legacy and
# vendor sources are counted separately because they are read-only (R15).
# O-91 excludes waveout_hle.c from release as a whole, so it needs no
# site-level exceptions.
SOURCE_EXCEPTIONS = ()
SOURCE_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".hpp")
SOURCE_ROOTS = (ROOT / "runtime", ROOT / "apps")
SOURCE_EXCLUDED_PARTS = ("third_party", "vendor", "build", ".git")
SOURCE_NON_PRODUCT_PREFIXES = (
    "runtime/platform/headless/",
)
SOURCE_DEBUG_ONLY_PREFIXES = (
    "runtime/diag/",
    # O-91/BV-3: v2 is compiled only when WC3_DEBUG is on.
    "runtime/hle/win32/winmm/waveout_hle.c",
)
SOURCE_READ_ONLY_PREFIXES = (
    "runtime/legacy/",
)


def _mask_source(text: str) -> str:
    """Blank comments and strings, preserving offsets and newlines."""
    out = list(text)
    i = 0
    while i < len(text):
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if text[i] == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < len(text):
                if text[i] == "*" and i + 1 < len(text) and text[i + 1] == "/":
                    out[i] = out[i + 1] = " "
                    i += 2
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        if text[i] == "/" and nxt == "/":
            out[i] = out[i + 1] = " "
            i += 2
            while i < len(text) and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if text[i] in ("'", '"'):
            quote = text[i]
            out[i] = " "
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
                    if i < len(text):
                        if text[i] != "\n":
                            out[i] = " "
                        i += 1
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def _condition_kind(condition: str) -> str:
    """Return positive, negative, or neutral for a WC3_DEBUG condition."""
    compact = re.sub(r"\s+", "", condition)
    if (
        "!WC3_DEBUG" in compact
        or re.search(r"!\s*defined\s*\(?\s*WC3_DEBUG\b", compact)
        or "WC3_DEBUG==0" in compact
        or "0==WC3_DEBUG" in compact
    ):
        return "negative"
    if re.search(r"\bWC3_DEBUG\b", compact):
        return "positive"
    return "neutral"


def _debug_guards(lines: Sequence[str]) -> list[bool]:
    """Mark lines that are compiled only when WC3_DEBUG is enabled.

    This is a conservative preprocessor scan, not a full C expression
    evaluator.  A branch is debug-only only when a positive WC3_DEBUG guard is
    on the active path; a neutral tag guard inherits the enclosing guard.  A
    false branch of a positive WC3_DEBUG guard is never marked debug-only.
    """
    guards = [False] * len(lines)
    stack: list[dict[str, object]] = []
    parent = False

    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            directive = stripped[1:].lstrip()
            word = directive.split(None, 1)[0] if directive else ""
            condition = directive[len(word):].strip()
            if word in ("if", "ifdef", "ifndef"):
                if word == "ifdef":
                    kind = "positive"
                elif word == "ifndef":
                    kind = "negative"
                else:
                    kind = _condition_kind(condition)
                if kind == "positive":
                    true_guard, false_guard = True, parent
                elif kind == "negative":
                    true_guard, false_guard = parent, True
                else:
                    true_guard, false_guard = parent, parent
                stack.append(
                    {
                        "parent": parent,
                        "true": true_guard,
                        "false": false_guard,
                        "current": true_guard,
                    }
                )
                parent = true_guard
            elif word == "elif" and stack:
                frame = stack[-1]
                kind = _condition_kind(condition)
                if kind == "negative":
                    current = bool(frame["parent"])
                    frame["false"] = True
                elif kind == "positive":
                    current = True
                    frame["false"] = bool(frame["parent"])
                else:
                    current = bool(frame["parent"])
                frame["current"] = current
                parent = current
            elif word == "else" and stack:
                frame = stack[-1]
                current = bool(frame["false"])
                frame["current"] = current
                parent = current
            elif word == "endif" and stack:
                frame = stack.pop()
                parent = bool(frame["parent"])
        guards[index] = parent
    return guards


def _source_files(root: Path) -> list[tuple[Path, str]]:
    files: list[tuple[Path, str]] = []
    for source_root in SOURCE_ROOTS:
        base = root / source_root.relative_to(ROOT)
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relpath = path.relative_to(root).as_posix()
            parts = relpath.split("/")
            if any(part in SOURCE_EXCLUDED_PARTS for part in parts):
                continue
            files.append((path, relpath))
    return sorted(files, key=lambda item: item[1])


def _source_exception(
    relpath: str,
    line: str,
    exceptions: Sequence[tuple[str, str, str]],
) -> str | None:
    for path, needle, reason in exceptions:
        if relpath == path and needle in line:
            return reason
    return None


def source_audit(
    root: Path = ROOT,
    exceptions: Sequence[tuple[str, str, str]] = SOURCE_EXCEPTIONS,
) -> dict[str, object]:
    """Count direct stderr fprintf sites and prove their release guards."""
    site_re = re.compile(r"\bfprintf\s*\(\s*stderr\b")
    rows: list[dict[str, object]] = []
    errors: list[str] = []
    counts: dict[str, int] = {
        "debug-gated": 0,
        "product-error": 0,
        "debug-only-file": 0,
        "read-only-legacy": 0,
        "ungated": 0,
    }
    files = _source_files(root)

    for path, relpath in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        original = text.splitlines()
        masked = _mask_source(text).splitlines()
        guards = _debug_guards(original)
        for index, line in enumerate(masked):
            if not site_re.search(line):
                continue
            source_line = original[index] if index < len(original) else ""
            if relpath.startswith(SOURCE_READ_ONLY_PREFIXES):
                kind = "read-only-legacy"
                reason = "runtime/legacy is read-only (R15/M5)"
            elif relpath.startswith(SOURCE_NON_PRODUCT_PREFIXES):
                continue
            elif relpath.startswith(SOURCE_DEBUG_ONLY_PREFIXES):
                kind = "debug-only-file"
                reason = "file is compiled only in debug/headless"
            elif guards[index]:
                kind = "debug-gated"
                reason = "inside WC3_DEBUG guard"
            else:
                reason = _source_exception(relpath, source_line, exceptions)
                kind = "product-error" if reason else "ungated"
            counts[kind] += 1
            row = {
                "path": relpath,
                "line": index + 1,
                "kind": kind,
                "reason": reason,
                "text": source_line.strip(),
            }
            rows.append(row)
            if kind == "ungated":
                errors.append(
                    f"{relpath}:{index + 1}: ungated stderr fprintf: "
                    f"{source_line.strip()}"
                )

    return {
        "files": len(files),
        "counts": counts,
        "rows": rows,
        "errors": errors,
    }


def print_source_report(report: dict[str, object]) -> None:
    print(f"source files: {report['files']}")
    counts = report["counts"]
    for kind in (
        "debug-gated",
        "product-error",
        "debug-only-file",
        "read-only-legacy",
        "ungated",
    ):
        print(f"  {kind:18s} {counts[kind]}")  # type: ignore[index]
    errors = report["errors"]
    if errors:
        print("release source audit: FAILED")
        for error in errors:  # type: ignore[union-attr]
            print(f"  - {error}")
    else:
        print("release source audit: ok")


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

        source = root / "runtime" / "app" / "good.c"
        source.parent.mkdir(parents=True)
        source.write_text(
            "#if WC3_DEBUG && WC3_DEBUG_HLE\n"
            "fprintf(stderr, \"debug\");\n"
            "#endif\n"
            "fprintf(stderr, \"PRODUCT failure\");\n",
            encoding="utf-8",
        )
        legacy = root / "runtime" / "legacy" / "old.c"
        legacy.parent.mkdir(parents=True)
        legacy.write_text("fprintf(stderr, \"legacy\");\n", encoding="utf-8")
        debug_only = root / "runtime" / "hle" / "win32" / "winmm" / \
            "waveout_hle.c"
        debug_only.parent.mkdir(parents=True)
        debug_only.write_text("fprintf(stderr, \"v2\");\n", encoding="utf-8")
        exceptions = (("runtime/app/good.c", "PRODUCT", "test product error"),)
        source_report = source_audit(root, exceptions)
        counts = source_report["counts"]
        if (
            source_report["errors"]
            or counts["debug-gated"] != 1
            or counts["product-error"] != 1
            or counts["read-only-legacy"] != 1
            or counts["debug-only-file"] != 1
        ):
            print("release source self-test: false failure")
            return 1

        source.write_text(
            "#if !WC3_DEBUG\n"
            "fprintf(stderr, \"release-only diagnostic\");\n"
            "#endif\n"
            "fprintf(stderr, \"leak\");\n",
            encoding="utf-8",
        )
        source_report = source_audit(root, exceptions)
        if not source_report["errors"]:
            print("release source self-test: false pass")
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
        help="check the checker on synthetic marker/source files, no ELF needed",
    )
    parser.add_argument(
        "--source-audit",
        action="store_true",
        help="count direct stderr fprintf sites and prove their release guards",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        return run_self_test()

    if args.source_audit:
        report = source_audit()
        print_source_report(report)
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"report: {args.json}")
        return 1 if report["errors"] else 0

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
