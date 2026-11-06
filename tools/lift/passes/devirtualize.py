#!/usr/bin/env python3
"""LiftPass prototype: devirtualization of constant dispatch targets.

IMPORTANT: this is a prototype, not part of the build loop.  It never writes
generated C into the repository; ``--out`` is meant to point at a build dir.

Why a plain text substitution is WRONG
--------------------------------------
``dispatch_common`` increments ``g_disp_depth`` and drains
``g_tail_pending`` in a loop.  ``dispatch_jmp`` relies on ``g_disp_depth > 0``
to set a pending tail target instead of recursing on the host stack.  Replacing
``dispatch(c, 0xVA)`` with a bare ``L_<va>(c)`` removes that accounting, so a
``dispatch_jmp`` inside the callee can either recurse without bound or hand its
pending target to the wrong trampoline loop.  Guest state (esp/eip) stays
correct while the *host* call mechanism breaks.

Therefore this pass does not invent a runtime API.  It emits calls through two
declared macros whose contract is specified in ARCHITECTURE section 3.7:

    WC3_DIRECT_CALL(fn, c)   - normal call, keeps guest-depth accounting
    WC3_DIRECT_TAIL(fn, c)   - tail call, drains/propagates pending target

and it reports how many call sites actually need that accounting.

Usage:
    python tools/lift/passes/devirtualize.py                 # dry run / plan
    python tools/lift/passes/devirtualize.py --out build/lift/war3_devirt.c
    python tools/lift/passes/devirtualize.py --json build/lift/devirt_plan.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OLD = ROOT.parent / "warcraft3_recomp"
DEFAULT_DISPATCH = OLD / "build27" / "wc3_dispatch_all.c"
DEFAULT_LIFT = OLD / "reports" / "lift27" / "war3_27_lifted.c"

LIFT_ROW = re.compile(r"\{ 0x([0-9A-Fa-f]{8})u, L_")
FUNC_START = re.compile(r"^void L_([0-9A-Fa-f]{8})\(CPU \*c\)")
CALL = re.compile(r"dispatch\(c, 0x([0-9A-Fa-f]{8})u\)")
TAIL = re.compile(r"dispatch_jmp\(c, 0x([0-9A-Fa-f]{8})u\)")
ANY_JMP = re.compile(r"dispatch_jmp\(")
ANY_INDIRECT = re.compile(r"dispatch(?:_jmp)?\(c, (?!0x)")
HLE_LO, HLE_HI = 0x7F000000, 0x7F100000


def index_functions(path: Path) -> dict[str, dict]:
    """Pass 1: record which lifted bodies need tail/depth accounting."""
    out: dict[str, dict] = {}
    cur: str | None = None
    has_tail = has_indirect = False
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = FUNC_START.match(line)
            if m:
                if cur is not None:
                    out[cur] = {"tail": has_tail, "indirect": has_indirect}
                cur = m.group(1).upper()
                has_tail = has_indirect = False
                continue
            if cur is None:
                continue
            if ANY_JMP.search(line):
                has_tail = True
            if ANY_INDIRECT.search(line):
                has_indirect = True
            if line.startswith("}"):
                out[cur] = {"tail": has_tail, "indirect": has_indirect}
                cur = None
    if cur is not None:
        out[cur] = {"tail": has_tail, "indirect": has_indirect}
    return out


def transform(path: Path, out_path: Path | None, lift_set: set[int],
              index: dict[str, dict]) -> dict:
    calls = tails = converted = skipped_unknown = skipped_hle = 0
    needs_depth = 0
    tail_in_callee = 0
    out = out_path.open("w", encoding="utf-8", newline="") if out_path else None
    try:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                def repl_call(m: re.Match) -> str:
                    nonlocal calls, converted, skipped_unknown, skipped_hle, \
                        needs_depth, tail_in_callee
                    calls += 1
                    va = int(m.group(1), 16)
                    if va not in lift_set:
                        if HLE_LO <= va < HLE_HI:
                            skipped_hle += 1
                        else:
                            skipped_unknown += 1
                        return m.group(0)
                    key = m.group(1).upper()
                    info = index.get(key, {})
                    if info.get("tail") or info.get("indirect"):
                        needs_depth += 1
                    if info.get("tail"):
                        tail_in_callee += 1
                    converted += 1
                    return f"WC3_DIRECT_CALL(L_{m.group(1)}, c)"

                def repl_tail(m: re.Match) -> str:
                    nonlocal tails, converted, skipped_unknown, skipped_hle
                    tails += 1
                    va = int(m.group(1), 16)
                    if va not in lift_set:
                        if HLE_LO <= va < HLE_HI:
                            skipped_hle += 1
                        else:
                            skipped_unknown += 1
                        return m.group(0)
                    converted += 1
                    return f"WC3_DIRECT_TAIL(L_{m.group(1)}, c)"

                line = CALL.sub(repl_call, line)
                line = TAIL.sub(repl_tail, line)
                if out:
                    out.write(line)
    finally:
        if out:
            out.close()
    return {
        "lift_file": path.as_posix(),
        "constant_calls": calls,
        "constant_tail_calls": tails,
        "converted": converted,
        "skipped_unknown": skipped_unknown,
        "skipped_hle_window": skipped_hle,
        "call_sites_needing_depth_accounting": needs_depth,
        "call_sites_whose_callee_tail_jumps": tail_in_callee,
        "functions_indexed": len(index),
        "output": out_path.as_posix() if out_path else None,
        "macros_required": ["WC3_DIRECT_CALL", "WC3_DIRECT_TAIL"],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dispatch", type=Path, default=DEFAULT_DISPATCH)
    ap.add_argument("--lift", type=Path, default=DEFAULT_LIFT)
    ap.add_argument("--out", type=Path, help="write transformed C (build dir only)")
    ap.add_argument("--json", type=Path, help="write the plan as JSON")
    args = ap.parse_args(argv)
    for p in (args.dispatch, args.lift):
        if not p.is_file():
            print(f"error: missing artifact {p}", file=sys.stderr)
            return 2
    vas = [int(m, 16) for m in LIFT_ROW.findall(
        args.dispatch.read_text(encoding="utf-8", errors="replace"))]
    index = index_functions(args.lift)
    report = transform(args.lift, args.out, set(vas), index)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
