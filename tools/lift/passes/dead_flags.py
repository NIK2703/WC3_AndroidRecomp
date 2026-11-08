#!/usr/bin/env python3
"""P0-2: conservative dead-flag elimination on generated C.

The pass removes the *flag side effects* of a flag-defining instruction only
when a later flag-defining instruction in the same straight-line basic block
overwrites every flag that could still be observed.  It never removes the
arithmetic/logic result itself.

This is intentionally not a general lazy-flags engine.  Control flow, labels,
guest calls, dispatcher boundaries and unknown helper calls are barriers: a
write that can be reached by another path, or whose flags can be read by a
callee, is left alone.  The resulting pass is a lower bound on P0-2, not a
replacement for the full dataflow analysis described in docs/PERFORMANCE.md.

The output is valid only when the input is a lifted corpus chunk and the
caller has a per-module golden equivalence report (G-15).  The pass is exposed
behind ``WC3_DEAD_FLAGS_P0_2`` (default OFF) for that reason.

Usage:
    python tools/lift/passes/dead_flags.py --lift chunk.c --out chunk.dead.c
    python tools/lift/passes/dead_flags.py --in-dir chunks --out-dir dead
    python tools/lift/passes/dead_flags.py --self-test

Exit codes: 0 pass, 1 verification failure, 2 invalid input.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FUNC_START_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)\s*\{",
    re.MULTILINE)
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
FLAG_READ_RE = re.compile(r"\bc->(cf|zf|sf|of|af|pf)\b")
LABEL_RE = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*:")

ALL_FLAGS = frozenset(("cf", "zf", "sf", "of", "af", "pf"))
LOGIC_FLAGS = frozenset(("cf", "zf", "sf", "of", "pf"))
INC_FLAGS = frozenset(("zf", "sf", "of", "af", "pf"))

# Helper -> flags it writes.  inc/dec preserve CF, so they write five flags and
# read CF; adc/sbb also read CF.  flags_logic* leave AF alone.
FLAG_WRITES = {
    "flags_add": ALL_FLAGS,
    "flags_sub": ALL_FLAGS,
    "flags_adc": ALL_FLAGS,
    "flags_sbb": ALL_FLAGS,
    "flags_logicz": LOGIC_FLAGS,
    "flags_logic32": LOGIC_FLAGS,
    "flags_logic8": LOGIC_FLAGS,
    "flags_incs": INC_FLAGS,
    "flags_decs": INC_FLAGS,
}
FLAG_READS = {
    "flags_adc": frozenset(("cf",)),
    "flags_sbb": frozenset(("cf",)),
    "flags_incs": frozenset(("cf",)),
    "flags_decs": frozenset(("cf",)),
}
FLAG_HELPERS = frozenset(FLAG_WRITES)

# Calls that cannot read or write the x86 flags.  This is a proof obligation:
# keep it narrow.  Anything not listed here is a barrier, which is the safe
# direction (it only loses an optimization, never changes semantics).
NEUTRAL_CALLS = frozenset((
    "rd8", "rd16", "rd32", "rd64", "rdss", "rdsd", "rdxm",
    "wr8", "wr16", "wr32", "wr64", "wrss", "wrsd", "wrxm",
    "push32", "pop32", "GVA", "R8L", "R8H", "R16", "R16L", "R32L",
    "mask_sz", "sign_sz", "parity8",
    "FS_RD32", "FS_WR32",
    "fst", "fpop", "fpush", "x87_ld_f32", "x87_ld_f64",
    "x87_st_f32", "x87_st_f64", "x87_add", "x87_mul",
    "sse_load_ss", "sse_load_sd", "sse_compare", "sse_cvtt_i32",
    "atomic_load32", "atomic_store32", "atomic_xchg32", "atomic_xadd32",
    "atomic_cmpxchg32", "atomic_cmpxchg64",
    "memcpy", "memmove", "memset", "strlen", "sizeof",
    "__builtin_expect", "__builtin_unreachable",
))

CONTROL_PREFIXES = (
    "if", "else", "while", "for", "switch", "case", "default",
    "goto", "return", "break", "continue", "do",
)
CONTROL_WORD_RE = re.compile(
    r"\b(?:if|else|while|for|switch|goto|return|break|continue|do)\b")
CALL_BARRIERS = (
    "dispatch", "dispatch_jmp", "WC3_DIRECT_CALL", "WC3_DIRECT_TAIL",
    "WC3_GUEST_DRAIN", "hle_", "L_", "RECOMP_TODO",
)


@dataclass
class WriteRecord:
    line: int
    start: int
    end: int
    text: str
    source_text: str
    flags: set[str]
    helper: str
    repl_text: str
    field_count: int
    live: bool = False


@dataclass
class FileReport:
    lift_file: str
    output: str | None = None
    functions: int = 0
    flag_writes: int = 0
    dead_writes: int = 0
    dead_flag_fields: int = 0
    changed_functions: int = 0
    change_set: list[dict] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def mask_code(line: str) -> str:
    """Return a same-length copy with comments and literals blanked."""
    out = list(line)
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            end = line.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for j in range(i, end):
                if out[j] != "\n":
                    out[j] = " "
            i = end
        elif c == "/" and i + 1 < n and line[i + 1] == "/":
            end = line.find("\n", i + 2)
            end = n if end < 0 else end
            for j in range(i, end):
                out[j] = " "
            i = end
        elif c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if line[j] == "\\":
                    j += 2
                    continue
                if line[j] == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


@dataclass
class Call:
    name: str
    start: int
    open: int
    close: int  # index one past the closing ')'
    args: list[str]


def find_calls(masked: str) -> list[Call]:
    calls: list[Call] = []
    for m in CALL_RE.finditer(masked):
        name = m.group(1)
        open_idx = masked.find("(", m.start())
        if open_idx < 0:
            continue
        depth = 1
        j = open_idx + 1
        while j < len(masked) and depth:
            if masked[j] == "(":
                depth += 1
            elif masked[j] == ")":
                depth -= 1
            j += 1
        if depth:
            continue
        close = j
        raw = masked[open_idx + 1:close - 1]
        args: list[str] = []
        part_start = 0
        d = 0
        for k, ch in enumerate(raw):
            if ch == "(":
                d += 1
            elif ch == ")":
                d -= 1
            elif ch == "," and d == 0:
                args.append(raw[part_start:k].strip())
                part_start = k + 1
        args.append(raw[part_start:].strip())
        calls.append(Call(name, m.start(), open_idx, close, args))
    return calls


def parse_size(text: str) -> int | None:
    t = text.strip().rstrip("uUlL")
    try:
        value = int(t, 0)
    except ValueError:
        return None
    return value if value in (1, 2, 4) else None


def mask_expr(expr: str, size: int) -> str:
    if size == 4:
        return f"(uint32_t)({expr})"
    bits = size * 8
    return f"(uint32_t)(({expr}) & 0x{(1 << bits) - 1:0{bits // 4}X}u)"


def make_replacement(call: Call) -> tuple[str, frozenset[str]] | None:
    """Return (raw expression, flags written) or None if unsupported."""
    name = call.name
    flags = FLAG_WRITES.get(name)
    if flags is None:
        return None
    args = call.args
    if name in ("flags_add", "flags_sub", "flags_adc", "flags_sbb"):
        if len(args) != 4:
            return None
        a, b, sz_text = args[1], args[2], args[3]
        sz = parse_size(sz_text)
        if sz is None:
            return None
        if name == "flags_add":
            return mask_expr(f"({a}) + ({b})", sz), flags
        if name == "flags_sub":
            return mask_expr(f"({a}) - ({b})", sz), flags
        if name == "flags_adc":
            return mask_expr(f"({a}) + ({b}) + (c->cf & 1u)", sz), flags
        return mask_expr(f"({a}) - ({b}) - (c->cf & 1u)", sz), flags
    if name == "flags_logicz":
        if len(args) != 3:
            return None
        value, sz_text = args[1], args[2]
        sz = parse_size(sz_text)
        if sz is None:
            return None
        return mask_expr(f"({value})", sz), flags
    if name in ("flags_incs", "flags_decs"):
        if len(args) != 3:
            return None
        value, sz_text = args[1], args[2]
        sz = parse_size(sz_text)
        if sz is None:
            return None
        op = "+" if name == "flags_incs" else "-"
        return mask_expr(f"({value}) {op} 1u", sz), flags
    if name in ("flags_logic32", "flags_logic8"):
        if len(args) != 2:
            return None
        # Keep evaluation of the value even though the flags are dead: the
        # argument may contain a side effect.  The compiler removes the cast
        # for pure register expressions.
        return f"(void)({args[1]})", flags
    return None


def is_barrier(masked: str, calls: list[Call]) -> bool:
    stripped = masked.strip()
    if not stripped:
        return False
    if stripped.startswith("#"):
        return True
    if stripped[0] in "{}":
        return True
    if LABEL_RE.match(stripped):
        return True
    low = stripped
    for word in CONTROL_PREFIXES:
        if low == word or low.startswith(word + " ") or low.startswith(word + "("):
            return True
    # A semicolon-separated control transfer later on the line (for example
    # `c->eax = flags_add(...); return;`) is still a boundary.
    if CONTROL_WORD_RE.search(masked):
        return True
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\s*=", low):
        # Direct assignment to a flag is a partial write.  Handling it would
        # need per-flag overwrite semantics, so it is a barrier.
        lhs = low.split("=", 1)[0]
        if re.search(r"\bc->(cf|zf|sf|of|af|pf)\b", lhs):
            return True
    if "WC3_DIRECT" in masked or "WC3_GUEST_DRAIN" in masked:
        return True
    if "dispatch" in masked or "dispatch_jmp" in masked:
        return True
    if "__asm" in masked or "asm volatile" in masked:
        return True
    for call in calls:
        if call.name in FLAG_HELPERS or call.name in NEUTRAL_CALLS:
            continue
        if call.name in CONTROL_PREFIXES:
            continue
        if any(call.name.startswith(prefix) for prefix in CALL_BARRIERS):
            return True
        return True
    return False


def helper_calls(calls: list[Call]) -> list[Call]:
    return [c for c in calls if c.name in FLAG_HELPERS]


def process_body(body: str, va: str, report: FileReport) -> str:
    lines = body.splitlines(keepends=True)
    pending: dict[str, WriteRecord] = {}
    dead: list[tuple[int, int, int, str, WriteRecord]] = []

    def kill_record(record: WriteRecord) -> None:
        record.live = True
        for f in list(record.flags):
            if pending.get(f) is record:
                pending.pop(f, None)
        record.flags.clear()

    for line_no, line in enumerate(lines):
        masked = mask_code(line)
        calls = find_calls(masked)
        hc = helper_calls(calls)

        if is_barrier(masked, calls):
            for rec in list(pending.values()):
                rec.live = True
            pending.clear()
            # A lifted function ends with a column-zero brace.  Anything after
            # it belongs to the next chunk/declaration and must not be treated
            # as part of this body's straight-line block.
            if line.rstrip("\r\n") == "}":
                break
            continue

        if len(hc) > 1:
            for rec in list(pending.values()):
                rec.live = True
            pending.clear()
            continue

        reads = {m.group(1) for m in FLAG_READ_RE.finditer(masked)}
        # A read on the same line may happen after the flag write.  Treat the
        # whole line as read-live so the new write is never marked dead.
        same_line_read = bool(reads)
        for f in reads:
            rec = pending.get(f)
            if rec is not None:
                kill_record(rec)

        if not hc:
            # A neutral line can contain arbitrary c->xxx register traffic but
            # not a flag read (handled above).  Pending writes survive.
            continue

        call = hc[0]
        if call.name not in FLAG_WRITES:
            continue
        made = make_replacement(call)
        if made is None:
            for rec in list(pending.values()):
                rec.live = True
            pending.clear()
            continue
        repl_text, write_flags = made
        report.flag_writes += 1

        # adc/sbb/inc/dec read the old CF; that read is part of this
        # instruction, not a reason to keep the previous flag computation
        # alive as an observable value.  It is still a read for liveness: the
        # old CF is consumed by this instruction's result, so the preceding
        # write cannot be removed unless this instruction has no successor.
        for f in FLAG_READS.get(call.name, frozenset()):
            rec = pending.get(f)
            if rec is not None:
                kill_record(rec)

        # The call starts at call.start and ends at call.close.  Replace the
        # whole call expression, not just the name.
        record = WriteRecord(line_no, call.start, call.close, call.name,
                             lines[line_no][call.start:call.close],
                             set(write_flags), call.name, repl_text,
                             len(write_flags), live=same_line_read)
        for f in write_flags:
            old = pending.get(f)
            if old is not None:
                old.flags.discard(f)
                if not old.flags and not old.live:
                    dead.append((old.line, old.start, old.end,
                                 old.source_text, old.repl_text, old))
                if not old.flags:
                    for k in [k for k, v in pending.items() if v is old]:
                        pending.pop(k, None)
        if not same_line_read:
            for f in write_flags:
                pending[f] = record
        else:
            record.live = True

    # A pending write at function exit is observable by the caller (or by the
    # guest's next instruction), so it is live.
    for rec in list(pending.values()):
        rec.live = True

    # Apply replacements.  They are non-overlapping because one flag helper
    # per line is required above.
    out = list(lines)
    for line_no, start, end, source_text, repl_text, record in sorted(
            dead, key=lambda x: (x[0], -x[1])):
        line = out[line_no]
        if line[start:end] != source_text:
            report.errors.append(
                f"L_{va}: replacement span mismatch on line {line_no + 1}")
            continue
        out[line_no] = line[:start] + repl_text + line[end:]
        report.dead_writes += 1
        report.dead_flag_fields += record.field_count
    if dead:
        report.changed_functions += 1
    return "".join(out)


def body_sha(body: str) -> str:
    return hashlib.sha256(body.encode("utf-8", "surrogatepass")).hexdigest()


def transform_text(text: str, report: FileReport) -> str:
    matches = list(FUNC_START_RE.finditer(text))
    if not matches:
        report.errors.append("no lifted functions found")
        return text
    report.functions = len(matches)
    pieces: list[str] = []
    cursor = 0
    before_bodies: dict[str, str] = {}
    after_bodies: dict[str, str] = {}
    for i, m in enumerate(matches):
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end]
        va = m.group(1).upper()
        before_bodies[va] = body
        new_body = process_body(body, va, report)
        after_bodies[va] = new_body
        pieces.append(text[cursor:start])
        pieces.append(new_body)
        cursor = end
    pieces.append(text[cursor:])
    out = "".join(pieces)

    for va in sorted(before_bodies):
        b = before_bodies[va]
        a = after_bodies[va]
        if b == a:
            continue
        report.change_set.append({
            "va": va,
            "pass_id": "dead_flags",
            "before_sha256": body_sha(b),
            "after_sha256": body_sha(a),
            "reason": "dead flag write elided in a straight-line basic block",
        })
    return out


def verify(before: str, after: str, report: FileReport) -> bool:
    """Check that only complete flag-helper calls were replaced."""
    if report.errors:
        return False
    b_matches = list(FUNC_START_RE.finditer(before))
    a_matches = list(FUNC_START_RE.finditer(after))
    if len(b_matches) != len(a_matches):
        report.errors.append("function count changed")
        return False
    for idx, (bm, am) in enumerate(zip(b_matches, a_matches)):
        bva = bm.group(1).upper()
        ava = am.group(1).upper()
        if bva != ava:
            report.errors.append(f"function order changed: {bva} -> {ava}")
            return False
        bstart = bm.end()
        bend = b_matches[idx + 1].start() if idx + 1 < len(b_matches) else len(before)
        astart = am.end()
        aend = a_matches[idx + 1].start() if idx + 1 < len(a_matches) else len(after)
        b = before[bstart:bend]
        a = after[astart:aend]
        if b == a:
            continue
        # Every changed line must have a helper call in the before text and a
        # generated expression in the after text.  The pass itself records no
        # per-site side file; this check is intentionally structural and
        # conservative.  It catches comment/string edits and whole-body
        # copying, which are the failure modes a post-pass can introduce.
        bl = b.splitlines()
        al = a.splitlines()
        if len(bl) != len(al):
            report.errors.append(f"L_{bva}: line count changed")
            return False
        for n, (x, y) in enumerate(zip(bl, al), 1):
            if x == y:
                continue
            mx = mask_code(x)
            my = mask_code(y)
            if not re.search(r"\bflags_(add|sub|adc|sbb|logicz|logic32|logic8|incs|decs)\s*\(", mx):
                report.errors.append(f"L_{bva}:{n}: changed line has no flag helper")
                return False
            if "flags_" in my:
                report.errors.append(f"L_{bva}:{n}: flag helper survived rewrite")
                return False
    return True


def self_test() -> int:
    src = """/* auto-generated fixture */
void L_00000001(CPU *c)
{
    c->eax = flags_add(c, c->eax, 1u, 4);       /* 1: add eax, 1 */
    c->edx = flags_add(c, c->edx, 2u, 4);       /* 2: add edx, 2 */
    c->ecx = flags_logicz(c, c->ecx ^ c->edx, 4); /* 3: xor ecx, edx */
    if (c->zf) { dispatch_jmp(c, 0x00000002u); return; } /* 4: je */
    c->ebx = flags_add(c, c->ebx, 3u, 4);       /* 5: read below */
    c->esi = flags_add(c, c->esi, 4u, 4);       /* 6: dead */
    c->edi = flags_sub(c, c->edi, 5u, 4);       /* 7: dead before inc? */
    c->ebp = flags_incs(c, c->ebp, 4);          /* 8: reads old cf */
    c->eax = flags_add(c, c->eax, 6u, 4);       /* 9: live, checked below */
    if (c->cf) { return; }                       /* 10: read */
    flags_logic32(c, c->eax);                    /* 11: void, dead before add */
    c->edx = flags_add(c, c->edx, 7u, 4);       /* 12: kill 11 */
    c->ecx = rd32(c->esp);                       /* 13: neutral */
    c->ebx = flags_add(c, c->ebx, 8u, 4);       /* 14: dead before ret? ret barrier keeps */
    return;
}
void L_00000002(CPU *c)
{
    c->eax = flags_add(c, c->eax, 1u, 4);
    unknown_hle(c);
    c->edx = flags_add(c, c->edx, 2u, 4);
    return;
}
"""
    report = FileReport("self-test")
    out = transform_text(src, report)
    if not verify(src, out, report):
        print("dead_flags self-test: verification failed", file=sys.stderr)
        for e in report.errors:
            print("  " + e, file=sys.stderr)
        return 1
    # The first add is overwritten by the second; the second is not fully dead
    # because the following logic does not write AF.  The add before the sub is
    # dead; the sub's CF is consumed by inc and must stay.  The add before the
    # CF test is live.  The void logic is overwritten by the next add.  The
    # final add is live at the return boundary.
    expected = [
        "c->eax = (uint32_t)((c->eax) + (1u));",
        "c->edx = flags_add(c, c->edx, 2u, 4);",
        "flags_logicz(c, c->ecx ^ c->edx, 4)",
        "flags_sub(c, c->edi, 5u, 4)",
        "c->ebp = (uint32_t)((c->ebp) + 1u);",
        "flags_add(c, c->eax, 6u, 4)",
        "flags_add(c, c->ebx, 8u, 4)",
        "unknown_hle(c)",
    ]
    for needle in expected:
        if needle not in out:
            print(f"dead_flags self-test: missing {needle!r}", file=sys.stderr)
            return 1
    if "flags_logic32" in out:
        print("dead_flags self-test: void logic not elided", file=sys.stderr)
        return 1
    if "(void)(c->eax)" not in out:
        print("dead_flags self-test: logic value not preserved", file=sys.stderr)
        return 1
    if report.dead_writes != 6:
        print(f"dead_flags self-test: dead_writes={report.dead_writes}, want 6",
              file=sys.stderr)
        return 1
    print("dead_flags self-test: OK")
    return 0


def process_file(src: Path, out: Path, report: FileReport) -> int:
    text = src.read_text(encoding="utf-8", errors="replace")
    result = transform_text(text, report)
    if not verify(text, result, report):
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(result, encoding="utf-8", newline="")
    report.output = str(out)
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lift")
    ap.add_argument("--in-dir")
    ap.add_argument("--out")
    ap.add_argument("--out-dir")
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.lift and not args.in_dir:
        ap.error("--lift or --in-dir is required")
    if args.lift and (not args.out or args.in_dir):
        ap.error("--lift requires --out")
    if args.in_dir and (not args.out_dir or args.lift):
        ap.error("--in-dir requires --out-dir")

    reports: list[FileReport] = []
    rc = 0
    if args.lift:
        report = FileReport(args.lift)
        rc = process_file(Path(args.lift), Path(args.out), report)
        reports.append(report)
    else:
        src_dir = Path(args.in_dir)
        out_dir = Path(args.out_dir)
        files = sorted(src_dir.glob("*.c"))
        if not files:
            print(f"dead_flags: no .c files in {src_dir}", file=sys.stderr)
            return 2
        for src in files:
            report = FileReport(str(src))
            one = process_file(src, out_dir / src.name, report)
            reports.append(report)
            rc = max(rc, one)

    data = {
        "pass": "dead_flags",
        "files": [r.__dict__ for r in reports],
        "totals": {
            "functions": sum(r.functions for r in reports),
            "flag_writes": sum(r.flag_writes for r in reports),
            "dead_writes": sum(r.dead_writes for r in reports),
            "dead_flag_fields": sum(r.dead_flag_fields for r in reports),
            "changed_functions": sum(r.changed_functions for r in reports),
            "errors": sum(len(r.errors) for r in reports),
        },
    }
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(data, indent=2) + "\n",
                                   encoding="utf-8", newline="")
    if not args.quiet:
        t = data["totals"]
        print(f"dead_flags: functions={t['functions']} "
              f"writes={t['flag_writes']} dead={t['dead_writes']} "
              f"fields={t['dead_flag_fields']} "
              f"changed={t['changed_functions']} errors={t['errors']}")
    for r in reports:
        for e in r.errors:
            print(f"{r.lift_file}: {e}", file=sys.stderr)
    return 1 if data["totals"]["errors"] else rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
