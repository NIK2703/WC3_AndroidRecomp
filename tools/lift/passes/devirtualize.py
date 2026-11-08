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
import hashlib
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
# One pass so a match position stays valid; CALL_RE/TAIL_RE document the two
# accepted rewrites and stay useful for focused checks.
SITE_RE = re.compile(
    r"\b(dispatch_jmp|dispatch)\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})u?\s*\)")
FUNC_START_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)\s*\{",
    re.MULTILINE)
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
    # ABI-11.2: legal, body-free review artifact.  Only changed functions are
    # listed; every entry is checked by reversing the two allowed macros.
    change_set: list[dict] = field(default_factory=list)
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


COMMENT_BLOCK_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
COMMENT_LINE_RE = re.compile(r"//[^\n]*")
REVERSE_CALL_RE = re.compile(
    r"WC3_DIRECT_CALL\s*\(\s*L_([0-9A-Fa-f]{8})\s*,\s*c\s*\)")
REVERSE_TAIL_RE = re.compile(
    r"WC3_DIRECT_TAIL\s*\(\s*L_([0-9A-Fa-f]{8})\s*,\s*c\s*\)")


def function_bodies(text: str) -> dict[str, str]:
    """VA -> raw body text (everything after the opening brace)."""
    matches = list(FUNC_START_RE.finditer(text))
    out: dict[str, str] = {}
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        out[m.group(1).upper()] = text[m.end():end]
    return out


def normalize_body(body: str) -> str:
    plain = COMMENT_BLOCK_RE.sub(" ", body)
    plain = COMMENT_LINE_RE.sub(" ", plain)
    return " ".join(plain.split())


def body_sha(body: str) -> str:
    return hashlib.sha256(normalize_body(body).encode("utf-8")).hexdigest()


def reverse_allowed_macros(body: str) -> str:
    body = REVERSE_CALL_RE.sub(
        lambda m: f"dispatch(c, 0x{m.group(1)}u)", body)
    return REVERSE_TAIL_RE.sub(
        lambda m: f"dispatch_jmp(c, 0x{m.group(1)}u)", body)


def build_change_set(before: str, after: str, report: FileReport) -> None:
    """Publish ABI-11.2 entries and reject anything but the two rewrites."""
    before_bodies = function_bodies(before)
    after_bodies = function_bodies(after)
    for va in sorted(set(before_bodies) | set(after_bodies)):
        b = before_bodies.get(va)
        a = after_bodies.get(va)
        if b is None or a is None:
            report.errors.append(f"function set changed for L_{va}")
            continue
        b_norm = normalize_body(b)
        a_norm = normalize_body(a)
        if b_norm == a_norm:
            continue
        if normalize_body(reverse_allowed_macros(a)) != b_norm:
            report.errors.append(f"unexpected body change in L_{va}")
            continue
        report.change_set.append({
            "va": va,
            "pass_id": "devirtualize",
            "before_sha256": body_sha(b),
            "after_sha256": body_sha(a),
            "reason": "constant dispatch -> direct call/tail",
        })


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


def function_body_mask(text: str) -> bytearray:
    """Mark only bytes inside a lifted function definition (not prototypes)."""
    mask = bytearray(len(text))
    matches = list(FUNC_START_RE.finditer(text))
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        mask[m.end():end] = b"\x01" * (end - m.end())
    return mask


def code_mask(text: str) -> bytearray:
    """Mask function-body bytes that are code, not comments or literals.

    Generated C is data as well as code: a guest string may legitimately
    contain the text ``dispatch(c, 0x...)``.  Rewriting such a literal would
    silently change guest data, so the pass only touches matches whose whole
    span is inside a lifted body and outside comments/strings/chars.
    """
    mask = function_body_mask(text)
    i = 0
    n = len(text)
    while i < n:
        if not mask[i]:
            i += 1
            continue
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            mask[i:end] = b"\x00" * (end - i)
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i + 2)
            end = n if end < 0 else end
            mask[i:end] = b"\x00" * (end - i)
            i = end
        elif c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            mask[i:j] = b"\x00" * (j - i)
            i = j
        else:
            i += 1
    return mask


def is_code_match(m: re.Match[str], text: str, mask: bytearray) -> bool:
    for i in range(m.start(), m.end()):
        if not mask[i] and not text[i].isspace():
            return False
    return True


def transform_text(text: str, lift_set: set[str],
                   index: dict[str, dict[str, bool]],
                   report: FileReport) -> str:
    converted_targets: set[str] = set()
    mask = code_mask(text)

    def repl_site(m: re.Match[str]) -> str:
        if not is_code_match(m, text, mask):
            return m.group(0)
        kind = m.group(1)
        key = m.group(2).upper()
        va = int(key, 16)
        if kind == "dispatch":
            report.constant_calls += 1
        else:
            report.constant_tail_calls += 1
        if HLE_LO <= va < HLE_HI:
            report.skipped_hle_window += 1
            return m.group(0)
        if key not in lift_set:
            report.skipped_unknown += 1
            return m.group(0)
        if kind == "dispatch":
            info = index.get(key, {})
            if info.get("tail") or info.get("indirect"):
                report.call_sites_needing_depth_accounting += 1
            if info.get("tail"):
                report.call_sites_whose_callee_tail_jumps += 1
            report.converted_calls += 1
            converted_targets.add(key)
            return f"WC3_DIRECT_CALL(L_{m.group(2)}, c)"
        report.converted_tails += 1
        converted_targets.add(key)
        return f"WC3_DIRECT_TAIL(L_{m.group(2)}, c)"

    out = SITE_RE.sub(repl_site, text)
    report.converted_targets = sorted(converted_targets,
                                      key=lambda v: int(v, 16))
    if converted_targets:
        out = ensure_include(out)
        out = insert_prototypes(out, converted_targets)
    build_change_set(text, out, report)
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
        if not report.change_set:
            errors.append("converted sites but no function-level change set")
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
    report.errors.extend(verify_text(transformed, report))
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
void L_00401040(CPU *c)
{
    /* dispatch(c, 0x00401010u); */
    const char *s = "dispatch(c, 0x00401010u)";
    (void)s;
}
"""
    lift_set = {"00401000", "00401010", "00401020", "00401030", "00401040"}
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
            # A comment and a string literal must keep the original text.
            "literal untouched": text.count("dispatch(c, 0x00401010u)") == 2,
        }
        bad = [name for name, ok in checks.items() if not ok]
        if bad:
            print(f"devirtualize self-test: FAIL {bad}", file=sys.stderr)
            return 1
        if report.converted_calls != 1 or report.converted_tails != 1:
            print("devirtualize self-test: FAIL conversion counts", file=sys.stderr)
            return 1
        changed = {entry["va"] for entry in report.change_set}
        if changed != {"00401000", "00401010"}:
            print("devirtualize self-test: FAIL change set", file=sys.stderr)
            return 1
        required = {"va", "pass_id", "before_sha256", "after_sha256", "reason"}
        if any(not required.issubset(entry) for entry in report.change_set):
            print("devirtualize self-test: FAIL change set schema",
                  file=sys.stderr)
            return 1
        if any(entry["before_sha256"] == entry["after_sha256"]
               for entry in report.change_set):
            print("devirtualize self-test: FAIL unchanged body hash",
                  file=sys.stderr)
            return 1
        if report.errors:
            print("devirtualize self-test: FAIL errors", report.errors,
                  file=sys.stderr)
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
    changes = [
        {"file": r.lift_file, **entry}
        for r in reports for entry in r.change_set
    ]
    summary = {
        "schema": 1,
        "pass_id": "devirtualize",
        "files": len(reports),
        "converted": sum(r.converted for r in reports),
        "skipped_unknown": sum(r.skipped_unknown for r in reports),
        "skipped_hle_window": sum(r.skipped_hle_window for r in reports),
        "changed_functions": len(changes),
        "change_set": changes,
        "errors": errors,
        "files_detail": [
            {k: v for k, v in r.__dict__.items() if k != "change_set"}
            for r in reports
        ],
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
