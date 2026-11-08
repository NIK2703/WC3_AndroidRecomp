#!/usr/bin/env python3
"""Devirtualize constant dispatch targets into the P0-1 direct-call contract.

The pass is deliberately conservative.  It only rewrites a site when the
target has an `L_<va>` definition in the active corpus list and the target is
outside the HLE thunk window.  The replacement is not a bare function call:

    dispatch(c, 0xVA)              -> WC3_DIRECT_CALL(L_VA, c)
    dispatch_jmp(c, 0xVA); return; -> WC3_DIRECT_TAIL(L_VA, c); return;

Those macros preserve the dispatcher depth and the deferred tail target
(GUEST_ABI ABI-4/ABI-5).  A bare `L_VA(c)` would leave `dispatch_jmp` inside
the callee with depth 0 and break the host call chain.

The pass also inserts `#include "direct_call.h"` and prototypes for every
converted target.  The prototypes matter after `splitcorpus`: a chunk may call
an `L_<va>` defined in another translation unit, and C11 has no implicit
declarations.  They are emitted in every output file, so a chunk is complete on
its own.

Usage:
    python tools/lift/passes/devirtualize.py --list build/.../boot_dispatch_list.h \
        --lift build/corpus-full/game/closed.c --out build/devirt/game_closed.c
    python tools/lift/passes/devirtualize.py --list ... --in-dir chunks --out-dir devirt
    python tools/lift/passes/devirtualize.py --self-test

Exit codes: 0 pass, 1 verification failure, 2 unavailable/invalid input.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_LIST = ROOT / "build" / "corpus-full" / "boot_dispatch_list.h"

CALL_RE = re.compile(
    r"\bdispatch\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})u?\s*\)")
TAIL_RE = re.compile(
    r"\bdispatch_jmp\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})u?\s*\)")
FUNC_START_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)", re.MULTILINE)
LIST_X_RE = re.compile(r"\bX\(([0-9A-Fa-f]{8})\)")
LIST_ROW_RE = re.compile(r"\{\s*0x([0-9A-Fa-f]{8})u\s*,\s*L_")
INCLUDE_CPU_RE = re.compile(r'^[ \t]*#include[ \t]+"cpu\.h"[ \t]*$',
                            re.MULTILINE)
INCLUDE_DIRECT_RE = re.compile(
    r'^[ \t]*#include[ \t]+"direct_call\.h"[ \t]*$', re.MULTILINE)
HLE_LO, HLE_HI = 0x7F000000, 0x7F100000


@dataclass
class FileReport:
    lift_file: str
    output: str | None
    constant_calls: int = 0
    constant_tail_calls: int = 0
    converted_calls: int = 0
    converted_tails: int = 0
    skipped_unknown: int = 0
    skipped_hle_window: int = 0
    call_sites_needing_depth_accounting: int = 0
    call_sites_whose_callee_tail_jumps: int = 0
    functions_indexed: int = 0
    converted_targets: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    @property
    def converted(self) -> int:
        return self.converted_calls + self.converted_tails


def parse_va_list(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    values = {m.upper() for m in LIST_X_RE.findall(text)}
    values.update(m.upper() for m in LIST_ROW_RE.findall(text))
    if not values:
        raise ValueError(f"no VAs in {path}")
    return values


def index_functions(text: str) -> dict[str, dict[str, bool]]:
    """Record which lifted bodies contain tail or non-constant dispatch."""
    matches = list(FUNC_START_RE.finditer(text))
    out: dict[str, dict[str, bool]] = {}
    for i, m in enumerate(matches):
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end]
        out[m.group(1).upper()] = {
            "tail": bool(re.search(r"\bdispatch_jmp\s*\(", body)),
            "indirect": bool(re.search(r"\bdispatch\s*\(\s*c\s*,(?!\s*0x)", body)),
        }
    return out


def ensure_include(text: str) -> str:
    if INCLUDE_DIRECT_RE.search(text):
        return text
    m = INCLUDE_CPU_RE.search(text)
    if m:
        end = m.end()
        return text[:end] + '\n#include "direct_call.h"' + text[end:]
    return '#include "direct_call.h"\n' + text


def insert_prototypes(text: str, targets: set[str]) -> str:
    if not targets:
        return text
    prototypes = "\n".join(
        f"void L_{va}(CPU *c);" for va in sorted(targets, key=lambda v: int(v, 16))
    )
    # Place them immediately after the direct_call include so they precede all
    # function bodies and generated calls.
    m = INCLUDE_DIRECT_RE.search(text)
    if not m:
        return prototypes + "\n\n" + text
    end = m.end()
    return text[:end] + "\n\n/* P0-1: converted call targets (may be in another TU). */\n" \
        + prototypes + text[end:]


def transform_text(text: str, lift_set: set[str],
                   index: dict[str, dict[str, bool]],
                   report: FileReport) -> str:
    converted_targets: set[str] = set()

    def repl_call(m: re.Match[str]) -> str:
        report.constant_calls += 1
        va = int(m.group(1), 16)
        if HLE_LO <= va < HLE_HI:
            report.skipped_hle_window += 1
            return m.group(0)
        if m.group(1).upper() not in lift_set:
            report.skipped_unknown += 1
            return m.group(0)
        key = m.group(1).upper()
        info = index.get(key, {})
        if info.get("tail") or info.get("indirect"):
            report.call_sites_needing_depth_accounting += 1
        if info.get("tail"):
            report.call_sites_whose_callee_tail_jumps += 1
        report.converted_calls += 1
        converted_targets.add(key)
        return f"WC3_DIRECT_CALL(L_{m.group(1)}, c)"

    def repl_tail(m: re.Match[str]) -> str:
        report.constant_tail_calls += 1
        va = int(m.group(1), 16)
        if HLE_LO <= va < HLE_HI:
            report.skipped_hle_window += 1
            return m.group(0)
        if m.group(1).upper() not in lift_set:
            report.skipped_unknown += 1
            return m.group(0)
        report.converted_tails += 1
        converted_targets.add(m.group(1).upper())
        return f"WC3_DIRECT_TAIL(L_{m.group(1)}, c)"

    out = CALL_RE.sub(repl_call, text)
    out = TAIL_RE.sub(repl_tail, out)
    report.converted_targets = sorted(converted_targets,
                                      key=lambda v: int(v, 16))
    if converted_targets:
        out = ensure_include(out)
        out = insert_prototypes(out, converted_targets)
    return out


def verify_text(text: str, report: FileReport) -> list[str]:
    errors: list[str] = []
    if re.search(r"\bL_7F[0-9A-Fa-f]{6}\s*\(\s*c\s*\)", text):
        errors.append("direct call to an HLE-window address")
    if re.search(r"\breturn\s+L_[0-9A-Fa-f]{8}\s*\(", text):
        errors.append("return fn(c); in a void function")
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if "WC3_DIRECT_TAIL(" not in line:
            continue
        if "return;" in line:
            continue
        # ABI-5.6 permits the return on the next source line.  Blank/comment
        # lines do not count as the required terminal statement.
        following = ""
        for nxt in lines[i + 1:i + 4]:
            if nxt.strip() and not nxt.lstrip().startswith("/*"):
                following = nxt
                break
        if "return;" not in following:
            errors.append("WC3_DIRECT_TAIL without a following return;")
            break
    if report.converted:
        if not INCLUDE_DIRECT_RE.search(text):
            errors.append("converted file does not include direct_call.h")
        for va in report.converted_targets:
            if not re.search(rf"\bvoid\s+L_{va}\s*\(", text, re.IGNORECASE):
                errors.append(f"no declaration for L_{va}")
    return errors


def process_file(src: Path, out: Path | None, lift_set: set[str]) -> FileReport:
    text = src.read_text(encoding="utf-8", errors="replace")
    index = index_functions(text)
    report = FileReport(lift_file=src.as_posix(),
                        output=out.as_posix() if out else None,
                        functions_indexed=len(index))
    transformed = transform_text(text, lift_set, index, report)
    report.errors = verify_text(transformed, report)
    if out is not None and not report.errors:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(transformed, encoding="utf-8", newline="")
    return report


def self_test() -> int:
    fixture = """#include "cpu.h"
void L_00401000(CPU *c) { dispatch(c, 0x00401010u); return; }
void L_00401010(CPU *c) { dispatch_jmp(c, 0x00401020u); return; }
void L_00401020(CPU *c) { dispatch(c, 0x7F000010u); return; }
void L_00401030(CPU *c) { dispatch(c, 0xDEADBEEFu); return; }
"""
    lift_set = {"00401000", "00401010", "00401020", "00401030"}
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fixture.c"
        out = Path(td) / "out.c"
        src.write_text(fixture, encoding="utf-8")
        report = process_file(src, out, lift_set)
        if report.errors:
            print("devirtualize self-test: FAIL", report.errors, file=sys.stderr)
            return 1
        text = out.read_text(encoding="utf-8")
        checks = {
            "direct call": "WC3_DIRECT_CALL(L_00401010, c)" in text,
            "direct tail": "WC3_DIRECT_TAIL(L_00401020, c)" in text,
            "hle skipped": "dispatch(c, 0x7F000010u)" in text,
            "unknown skipped": "dispatch(c, 0xDEADBEEFu)" in text,
            "include": '#include "direct_call.h"' in text,
            "prototype": "void L_00401010(CPU *c);" in text,
        }
        bad = [name for name, ok in checks.items() if not ok]
        if bad:
            print(f"devirtualize self-test: FAIL {bad}", file=sys.stderr)
            return 1
        if report.converted_calls != 1 or report.converted_tails != 1:
            print("devirtualize self-test: FAIL conversion counts", file=sys.stderr)
            return 1
    print("devirtualize self-test: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", type=Path, default=DEFAULT_LIST,
                    help="boot_dispatch_list.h or old dispatch table")
    ap.add_argument("--lift", type=Path, help="generated C file or directory")
    ap.add_argument("--out", type=Path, help="output file (single input)")
    ap.add_argument("--in-dir", type=Path, help="directory of generated chunks")
    ap.add_argument("--out-dir", type=Path, help="output directory for --in-dir")
    ap.add_argument("--json", type=Path, help="report path")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true",
                    help="transform and verify without writing C")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.list.is_file():
        print(f"error: missing VA list {args.list}", file=sys.stderr)
        return 2
    try:
        lift_set = parse_va_list(args.list)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    jobs: list[tuple[Path, Path | None]] = []
    if args.in_dir:
        if not args.in_dir.is_dir():
            print(f"error: missing input directory {args.in_dir}", file=sys.stderr)
            return 2
        out_dir = args.out_dir
        for src in sorted(args.in_dir.glob("*.c")):
            jobs.append((src, None if args.dry_run or out_dir is None
                         else out_dir / src.name))
    elif args.lift:
        if not args.lift.is_file():
            print(f"error: missing generated C {args.lift}", file=sys.stderr)
            return 2
        jobs.append((args.lift, None if args.dry_run else args.out))
    else:
        print("error: pass --lift/--out, --in-dir/--out-dir, or --self-test",
              file=sys.stderr)
        return 2

    reports = [process_file(src, out, lift_set) for src, out in jobs]
    errors = [f"{r.lift_file}: {e}" for r in reports for e in r.errors]
    summary = {
        "files": len(reports),
        "converted": sum(r.converted for r in reports),
        "skipped_unknown": sum(r.skipped_unknown for r in reports),
        "skipped_hle_window": sum(r.skipped_hle_window for r in reports),
        "errors": errors,
        "files_detail": [r.__dict__ for r in reports],
        "verdict": "pass" if not errors else "fail",
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(summary, indent=2) + "\n",
                             encoding="utf-8")
    print(f"devirtualize: {summary['verdict']} ({summary['files']} files, "
          f"{summary['converted']} sites, {len(errors)} errors)")
    for error in errors[:20]:
        print(f"  {error}", file=sys.stderr)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
