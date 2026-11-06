#!/usr/bin/env python3
"""Static performance probe for the WC3 1.27b lift corpus.

This is a one-shot analysis tool, not part of the build loop.  It reads the
generated dispatcher and one lifted translation unit and reports the
measurable upper bounds of the lift-time optimizations described in
``docs/ARCHITECTURE_1.27B.md`` section 3.7.  It never writes into the game
install and never copies game code into the repository.

The dispatcher file is small enough to read at once; the lift unit can be
hundreds of megabytes, so it is always streamed line by line.

Usage:
    python tools/perf_probe.py
    python tools/perf_probe.py --dispatch <file> --lift <file> --json out.json
"""
from __future__ import annotations

import argparse
import json
import random
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT.parent / "warcraft3_recomp"
DEFAULT_DISPATCH = OLD / "build27" / "wc3_dispatch_all.c"
DEFAULT_LIFT = OLD / "reports" / "lift27" / "war3_27_lifted.c"

HLE_BASE = 0x7F000000
HLE_SIZE = 0x00100000
LIFT_ROW = re.compile(r"\{ 0x([0-9A-Fa-f]{8})u, L_")
CONST_CALL = re.compile(r"dispatch\(c, 0x([0-9A-Fa-f]{8})u\)")
CONST_JMP = re.compile(r"dispatch_jmp\(c, 0x([0-9A-Fa-f]{8})u\)")
INDIRECT = re.compile(r"dispatch(?:_jmp)?\(c, (?!0x)")
FLAG_WRITE = re.compile(r"\bflags_\w+\(")
FLAG_READ = re.compile(r"c->(?:cf|zf|sf|of|af|pf)\b")
GVA = re.compile(r"\bGVA\(")
PUSH_PAIR = re.compile(r"push32\(c, 0x[0-9A-Fa-f]+u\); dispatch\(c, 0x")
IAT_LIKE = re.compile(r"dispatch(?:_jmp)?\(c, rd(?:32|16|8)\(")
FPU_CW = re.compile(r"c->fpu_cw\s*=")
FUNC_START = re.compile(r"void L_[0-9A-Fa-f]{8}\(CPU \*c\)")


def knuth(va: int, bits: int) -> int:
    """The hash used by the generated dispatcher today."""
    return ((va * 2654435761) & 0xFFFFFFFF) >> (32 - bits)


def hash_stats(vas: list[int], bits: int, samples: int = 2000) -> dict:
    size = 1 << bits
    mask = size - 1
    table: list[int | None] = [None] * size
    for va in vas:
        h = knuth(va, bits)
        while table[h] is not None:
            h = (h + 1) & mask
        table[h] = va
    rng = random.Random(7)
    hits = []
    for va in rng.sample(vas, min(samples, len(vas))):
        h, n = knuth(va, bits), 1
        while table[h] != va:
            h, n = (h + 1) & mask, n + 1
        hits.append(n)
    misses = []
    for _ in range(samples):
        va = rng.randrange(0, 1 << 32)
        h, n = knuth(va, bits), 1
        while table[h] is not None:
            h, n = (h + 1) & mask, n + 1
        misses.append(n)
    return {
        "bits": bits,
        "table_mib": round(size * 4 / 1048576, 2),
        "load": round(len(vas) / size, 4),
        "hit_avg": round(sum(hits) / len(hits), 3),
        "hit_max": max(hits),
        "miss_avg": round(sum(misses) / len(misses), 3),
        "miss_max": max(misses),
    }


def scan_lift(path: Path, lift_set: set[int]) -> dict:
    const_calls = const_jmp = indirect = covered = in_hle = unknown = 0
    flag_writes = flag_reads = dead_writes = funcs = gva = push_pairs = 0
    iat_like = fpu_cw = 0
    events: list[str] = []
    in_func = False
    func_open = False

    def flush() -> None:
        nonlocal funcs, flag_writes, flag_reads, dead_writes, func_open
        if not func_open:
            events.clear()
            return
        funcs += 1
        func_open = False
        for i, e in enumerate(events):
            if e == "W":
                flag_writes += 1
                nxt = next((x for x in events[i + 1:] if x in "WR"), None)
                if nxt == "W":
                    dead_writes += 1
            else:
                flag_reads += 1
        events.clear()

    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            for m in CONST_CALL.finditer(line):
                const_calls += 1
                va = int(m.group(1), 16)
                if va in lift_set:
                    covered += 1
                elif HLE_BASE <= va < HLE_BASE + HLE_SIZE:
                    in_hle += 1
                else:
                    unknown += 1
            for m in CONST_JMP.finditer(line):
                const_jmp += 1
                va = int(m.group(1), 16)
                if va in lift_set:
                    covered += 1
                elif HLE_BASE <= va < HLE_BASE + HLE_SIZE:
                    in_hle += 1
                else:
                    unknown += 1
            indirect += len(INDIRECT.findall(line))
            iat_like += len(IAT_LIKE.findall(line))
            gva += len(GVA.findall(line))
            push_pairs += len(PUSH_PAIR.findall(line))
            fpu_cw += len(FPU_CW.findall(line))

            if FUNC_START.match(line):
                flush()
                in_func = True
                func_open = True
            if in_func:
                events.extend("W" for _ in FLAG_WRITE.findall(line))
                events.extend("R" for _ in FLAG_READ.findall(line))
                if line.startswith("}"):
                    flush()
                    in_func = False
        flush()

    total = const_calls + const_jmp
    dispatch_all = total + indirect
    return {
        "lift_file": path.as_posix(),
        "lift_bytes": path.stat().st_size,
        "functions": funcs,
        "constant_dispatch": const_calls,
        "constant_dispatch_jmp": const_jmp,
        "constant_total": total,
        "devirtualizable": covered,
        "target_in_hle_window": in_hle,
        "target_unknown": unknown,
        "devirtualizable_pct": round(100 * covered / total, 2) if total else 0.0,
        "indirect_dispatch": indirect,
        "dispatch_traffic_removed_pct": round(100 * covered / dispatch_all, 2)
        if dispatch_all else 0.0,
        "iat_like_indirect": iat_like,
        "return_address_push_pairs": push_pairs,
        "flag_writes": flag_writes,
        "flag_reads": flag_reads,
        "dead_flag_writes": dead_writes,
        "dead_flag_pct": round(100 * dead_writes / flag_writes, 1)
        if flag_writes else 0.0,
        "gva_expansions": gva,
        "fpu_control_word_sites": fpu_cw,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dispatch", type=Path, default=DEFAULT_DISPATCH)
    ap.add_argument("--lift", type=Path, default=DEFAULT_LIFT)
    ap.add_argument("--json", type=Path, help="write the report as JSON")
    ap.add_argument("--no-hash", action="store_true", help="skip hash sweep")
    args = ap.parse_args(argv)

    for p in (args.dispatch, args.lift):
        if not p.is_file():
            print(f"error: missing artifact {p}", file=sys.stderr)
            return 2

    vas = [int(m, 16) for m in LIFT_ROW.findall(
        args.dispatch.read_text(encoding="utf-8", errors="replace"))]
    report: dict = {"dispatch_entries": len(vas), "unique_entries": len(set(vas))}
    if not args.no_hash:
        report["hash_sweep"] = [hash_stats(vas, b) for b in (18, 19, 20)]
    report["lift"] = scan_lift(args.lift, set(vas))

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
