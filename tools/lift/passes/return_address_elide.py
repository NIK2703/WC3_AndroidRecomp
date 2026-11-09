#!/usr/bin/env python3
"""P0-6: elide the guest return-address store before provably safe direct calls.

The lifted corpus keeps a Windows-x86-shaped stack: a call site executes
`push32(c, return_va)` and a lifted callee returns with `c->esp += 4; return;`.
The callee does **not** need the value in memory to return; it needs only the
`esp` slot.  The store matters only when the callee (or a function reached by a
direct tail jump) may read that slot.

This pass indexes every lifted function in the supplied files, proves a
conservative `reads_return_address` summary, and rewrites

    push32(c, 0x...u); dispatch(c, 0xTARGETu);
    push32(c, 0x...u); WC3_DIRECT_CALL(L_TARGET, c);

to

    c->esp -= 4; dispatch(c, 0xTARGETu);
    c->esp -= 4; WC3_DIRECT_CALL(L_TARGET, c);

only when TARGET's summary is false.

Safety boundary
---------------
The summary is deliberately textual and conservative:

  * every direct `[esp+N]` / `rd*(c->esp + N)` read is checked against the
    tracked stack delta; a read of the entry slot is unsafe;
  * `pop32` at the entry slot is unsafe; a balanced pop of a value pushed by
    the function itself is safe;
  * a standard `push ebp; mov ebp, esp` frame is tracked, so `[ebp+8]` is an
    argument while `[ebp+4]` is the return address; a function that uses ebp as
    a general register is treated as unsafe as soon as ebp memory is read;
  * any residual `c->esp` use other than push/pop/stack adjustment is unsafe
    (this catches `push esp`, `memcpy(..., esp, ...)` and similar aliases);
  * direct tail jumps are followed; an indirect or unknown tail, or a tail
    into the HLE window, makes callers unsafe;
  * calls and unknown stack changes reset the stack delta to "unknown"; this
    loses opportunities, never invents a proof;
  * HLE calls are never rewritten.

The pass is wired as ``WC3_RETURN_ADDRESS_ELIDE_P0_6`` (default OFF) until
G-15 carries its function-level change set and a whole-corpus run is accepted.

Usage:
    python tools/lift/passes/return_address_elide.py --lift corpus.c --out out.c
    python tools/lift/passes/return_address_elide.py --in-dir chunks --out-dir out
    python tools/lift/passes/return_address_elide.py --self-test

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
FUNC_START_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)\s*\{",
    re.MULTILINE)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')
LABEL_RE = re.compile(r"^\s*(L_[0-9A-Fa-f]+):")
GOTO_RE = re.compile(r"\bgoto\s+(L_[0-9A-Fa-f]+)\s*;")
RETURN_RE = re.compile(r"\breturn\s*;")
ESP_READ_RE = re.compile(
    r"\brd(?:8|16|32|64)\s*\(\s*\(?\s*c->esp\s*"
    r"(?:\+\s*(0x[0-9A-Fa-f]+|\d+)u?)?\s*\)?\s*\)")
EBP_READ_RE = re.compile(
    r"\brd(?:8|16|32|64)\s*\(\s*\(?\s*c->ebp\s*"
    r"(?:\+\s*(0x[0-9A-Fa-f]+|\d+)u?)?\s*\)?\s*\)")
CALL_RE = re.compile(
    r"\b(?:dispatch|dispatch_jmp|WC3_DIRECT_CALL|WC3_DIRECT_TAIL|"
    r"wc3_legacy_hle_call|hle_[A-Za-z0-9_]*|hle_call)\s*\(")
DIRECT_TAIL_RE = re.compile(
    r"\bWC3_DIRECT_TAIL\s*\(\s*L_([0-9A-Fa-f]{8})\s*,\s*c\s*\)")
DISPATCH_TAIL_RE = re.compile(
    r"\bdispatch_jmp\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})u?\s*\)")
HLE_LO, HLE_HI = 0x7F000000, 0x7F100000

CALL_SITE_RE = re.compile(
    r"push32\s*\(\s*c\s*,\s*0x(?P<ret>[0-9A-Fa-f]{8})u?\s*\)\s*;\s*"
    r"(?P<call>"
    r"WC3_DIRECT_CALL\s*\(\s*L_(?P<direct>[0-9A-Fa-f]{8})\s*,\s*c\s*\)"
    r"|"
    r"dispatch\s*\(\s*c\s*,\s*0x(?P<dispatch>[0-9A-Fa-f]{8})u?\s*\)"
    r")")

REVERSE_PUSH_RE = re.compile(
    r"(?m)^(\s*)c->esp -= 4;(\s*(?:/\*.*?\*/\s*)?)$")
PUSH_CALL_RE = re.compile(
    r"push32\s*\(\s*c\s*,\s*0x[0-9A-Fa-f]{8}u?\s*\)\s*;")
ESP_DEC_RE = re.compile(r"c->esp\s*-=\s*4\s*;")
HLE_AFTER_ESP_RE = re.compile(
    r"c->esp\s*-=\s*4\s*;\s*"
    r"(?:dispatch\s*\(\s*c\s*,\s*"
    r"(?:0x(?P<dispatch>[0-9A-Fa-f]{8})u?|L_(?P<dispatch_l>[0-9A-Fa-f]{8}))"
    r"|WC3_DIRECT_CALL\s*\(\s*L_(?P<direct>[0-9A-Fa-f]{8})\s*,\s*c\s*\))")


@dataclass
class FunctionInfo:
    va: str
    body: str
    reads_return_address: bool = False
    tail_targets: set[str] = field(default_factory=set)
    tail_unknown: bool = False


@dataclass
class Report:
    file: str
    output: str | None
    functions: int = 0
    call_sites: int = 0
    elided: int = 0
    skipped_reads_ra: int = 0
    skipped_unknown: int = 0
    skipped_hle: int = 0
    changed_functions: int = 0
    change_set: list[dict] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def clean_line(line: str) -> str:
    line = BLOCK_COMMENT_RE.sub(" ", line)
    line = LINE_COMMENT_RE.sub(" ", line)
    return STRING_RE.sub('""', line)


def parse_number(text: str) -> int | None:
    text = text.strip()
    m = re.fullmatch(r"0x([0-9A-Fa-f]+)u?", text)
    if m:
        return int(m.group(1), 16)
    m = re.fullmatch(r"(\d+)u?", text)
    if m:
        return int(m.group(1))
    return None


def normalize_body(body: str) -> str:
    plain = BLOCK_COMMENT_RE.sub(" ", body)
    plain = LINE_COMMENT_RE.sub(" ", plain)
    # Literal contents are not executable stack/call sites.  Masking them
    # keeps the structural ABI check from rewriting text inside a string while
    # comparing before/after bodies.
    plain = STRING_RE.sub('""', plain)
    return " ".join(plain.split())


def body_sha(body: str) -> str:
    return hashlib.sha256(normalize_body(body).encode("utf-8")).hexdigest()


def structural_body(body: str) -> str:
    """Canonicalize the only two forms the pass may exchange."""
    plain = normalize_body(body)
    plain = PUSH_CALL_RE.sub("SITE;", plain)
    return ESP_DEC_RE.sub("SITE;", plain)


def function_bodies(text: str) -> dict[str, str]:
    matches = list(FUNC_START_RE.finditer(text))
    out: dict[str, str] = {}
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        out[m.group(1).upper()] = text[m.end():end]
    return out


def code_mask(text: str) -> bytearray:
    """Mark bytes inside lifted bodies that are code, not comments/strings."""
    matches = list(FUNC_START_RE.finditer(text))
    mask = bytearray(len(text))
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        mask[m.end():end] = b"\x01" * (end - m.end())
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
        elif c in ('"', "'"):
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


def analyze_body(body: str) -> tuple[bool, set[str], bool]:
    """Return (reads_ra, direct_tail_targets, tail_unknown)."""
    lines = [clean_line(x) for x in body.splitlines()]
    n = len(lines)
    labels = {m.group(1): i for i, line in enumerate(lines)
              if (m := LABEL_RE.match(line))}

    depth_before: list[int] = []
    depth_after: list[int] = []
    depth = 0
    for line in lines:
        depth_before.append(depth)
        depth += line.count("{") - line.count("}")
        depth_after.append(depth)

    def match_end(i: int) -> int:
        base = depth_before[i]
        if depth_after[i] == base:
            return i  # single-line block
        for j in range(i + 1, n):
            if depth_after[j] == base and "}" in lines[j]:
                return j
        return n - 1

    ebp_writes = [i for i, line in enumerate(lines)
                  if re.search(r"\bc->ebp\s*=", line)]
    prologue = None
    for i, line in enumerate(lines):
        if (re.search(r"\bc->ebp\s*=\s*\(?\s*uint32_t\s*\)?\s*"
                      r"\(?\s*c->esp\b", line)
                or re.search(r"\bc->ebp\s*=\s*c->esp\b", line)):
            prologue = i
            break

    frame_rel: int | None = None
    if prologue is not None:
        others = [
            i for i in ebp_writes
            if i != prologue and not re.search(
                r"\bc->ebp\s*=\s*\(?\s*uint32_t\s*\)?\s*"
                r"\(?\s*pop32\s*\(", lines[i])
        ]
        if not others:
            rel = 0
            for j in range(prologue):
                if re.search(r"\bpush32\s*\(", lines[j]):
                    rel -= 4
            frame_rel = rel

    states: list[set[int | None]] = [set() for _ in range(n)]
    if n:
        states[0].add(0)
    work = [0]
    unsafe = False
    tail_targets: set[str] = set()
    tail_unknown = False

    def mark_unsafe() -> None:
        nonlocal unsafe
        unsafe = True

    while work:
        i = work.pop()
        if i >= n:
            continue
        for incoming in list(states[i]):
            line = lines[i]
            for m in EBP_READ_RE.finditer(line):
                off = parse_number(m.group(1) or "0")
                if off is None or frame_rel is None or frame_rel + off == 0:
                    mark_unsafe()
            delta = incoming
            for m in ESP_READ_RE.finditer(line):
                off = parse_number(m.group(1) or "0")
                if off is None or delta is None or delta + off == 0:
                    mark_unsafe()
            rest = ESP_READ_RE.sub(" ", line)
            # A proven frame read is consumed here; only a residual c->ebp
            # use (an alias or an untracked frame) is unsafe below.
            rest = EBP_READ_RE.sub(" ", rest)

            push = re.search(r"\bpush32\s*\(\s*c\s*,\s*(.*?)\)\s*;", rest)
            if push:
                arg = push.group(1)
                if "c->esp" in arg:
                    mark_unsafe()
                if "c->ebp" in arg and CALL_RE.search(line):
                    mark_unsafe()
                if delta is not None:
                    delta -= 4
                rest = rest[:push.start()] + " " + rest[push.end():]

            pop = re.search(r"\bpop32\s*\(\s*c\s*\)", rest)
            if pop:
                if delta is None or delta == 0:
                    mark_unsafe()
                elif delta is not None:
                    delta += 4

            m = re.search(r"\bc->esp\s*\+=\s*(0x[0-9A-Fa-f]+|\d+)u?",
                          rest)
            if m:
                add = parse_number(m.group(1))
                delta = None if (delta is None or add is None) else delta + add
                rest = re.sub(r"\bc->esp\s*\+=\s*[^;]+;?", " ", rest)
            m = re.search(r"\bc->esp\s*-=\s*(0x[0-9A-Fa-f]+|\d+)u?",
                          rest)
            if m:
                sub = parse_number(m.group(1))
                delta = None if (delta is None or sub is None) else delta - sub
                rest = re.sub(r"\bc->esp\s*-=\s*[^;]+;?", " ", rest)

            if re.search(
                    r"\bc->esp\s*=\s*[^;]*\bflags_add\s*\(\s*c\s*,"
                    r"\s*c->esp\s*,\s*(0x[0-9A-Fa-f]+|\d+)u?", rest):
                m = re.search(
                    r"flags_add\s*\(\s*c\s*,\s*c->esp\s*,\s*"
                    r"(0x[0-9A-Fa-f]+|\d+)u?", rest)
                add = parse_number(m.group(1)) if m else None
                delta = None if (delta is None or add is None) else delta + add
                rest = re.sub(r"\bc->esp\s*=\s*[^;]+;?", " ", rest)
            elif re.search(
                    r"\bc->esp\s*=\s*\(?\s*uint32_t\s*\)?\s*"
                    r"\(?\s*c->ebp\s*\)?", rest):
                delta = frame_rel
                rest = re.sub(r"\bc->esp\s*=\s*[^;]+;?", " ", rest)
            elif re.search(r"\bc->esp\s*=", rest):
                delta = None
                rest = re.sub(r"\bc->esp\s*=\s*[^;]+;?", " ", rest)

            if i == prologue:
                rest = re.sub(r"c->esp", " ", rest)
            if "c->esp" in rest:
                mark_unsafe()

            if CALL_RE.search(line):
                delta = None

            if re.search(r"\bc->ebp\s*=", rest):
                if i == prologue:
                    rest = re.sub(r"c->ebp", " ", rest)
                elif re.search(
                        r"\bc->ebp\s*=\s*\(?\s*uint32_t\s*\)?\s*"
                        r"\(?\s*pop32\s*\(", rest):
                    frame_rel = None
                    rest = re.sub(r"c->ebp", " ", rest)
                else:
                    mark_unsafe()
            if "c->ebp" in rest:
                mark_unsafe()

            for m in DIRECT_TAIL_RE.finditer(line):
                tail_targets.add(m.group(1).upper())
            for m in DISPATCH_TAIL_RE.finditer(line):
                target = int(m.group(1), 16)
                if HLE_LO <= target < HLE_HI:
                    tail_unknown = True
                else:
                    tail_targets.add(m.group(1).upper())
            if re.search(r"\bdispatch_jmp\s*\(", line):
                if not DISPATCH_TAIL_RE.search(line) and \
                        not DIRECT_TAIL_RE.search(line):
                    tail_unknown = True

            stripped = line.strip()
            if RETURN_RE.search(line) and not re.search(r"^\s*if\b", stripped):
                continue

            goto = GOTO_RE.search(line)
            if goto:
                target = labels.get(goto.group(1))
                if target is not None and delta not in states[target]:
                    states[target].add(delta)
                    work.append(target)
                if not re.search(r"^\s*if\b", stripped):
                    continue
                if i + 1 < n and incoming not in states[i + 1]:
                    states[i + 1].add(incoming)
                    work.append(i + 1)
                continue

            if re.search(r"^\s*if\b", stripped):
                if "{" in stripped:
                    end = match_end(i)
                    if end + 1 < n and incoming not in states[end + 1]:
                        states[end + 1].add(incoming)
                        work.append(end + 1)
                else:
                    mark_unsafe()

            if re.search(r"^\s*(?:while|for|switch|do)\b", stripped):
                if ("push32" in stripped or "pop32" in stripped
                        or "c->esp" in stripped):
                    mark_unsafe()
                if i + 1 < n and incoming not in states[i + 1]:
                    states[i + 1].add(incoming)
                    work.append(i + 1)

            if i + 1 < n and delta not in states[i + 1]:
                states[i + 1].add(delta)
                work.append(i + 1)

    return unsafe, tail_targets, tail_unknown


def index_functions(text: str) -> dict[str, FunctionInfo]:
    infos: dict[str, FunctionInfo] = {}
    for va, body in function_bodies(text).items():
        unsafe, tails, tail_unknown = analyze_body(body)
        infos[va] = FunctionInfo(
            va=va, body=body, reads_return_address=unsafe,
            tail_targets=tails, tail_unknown=tail_unknown)
    return infos


def compute_tail_closures(infos: dict[str, FunctionInfo]) -> None:
    """Propagate reads_ra through direct tail jumps to a fixed point."""
    changed = True
    while changed:
        changed = False
        for info in infos.values():
            if info.reads_return_address:
                continue
            if info.tail_unknown:
                info.reads_return_address = True
                changed = True
                continue
            for target in info.tail_targets:
                target_info = infos.get(target)
                if target_info is None or target_info.reads_return_address:
                    info.reads_return_address = True
                    changed = True
                    break


def transform_text(text: str, infos: dict[str, FunctionInfo],
                   report: Report) -> str:
    mask = code_mask(text)
    pieces: list[str] = []
    last = 0
    for m in CALL_SITE_RE.finditer(text):
        report.call_sites += 1
        if not is_code_match(m, text, mask):
            continue
        target = (m.group("direct") or m.group("dispatch") or "").upper()
        if not target:
            continue
        target_va = int(target, 16)
        if HLE_LO <= target_va < HLE_HI:
            report.skipped_hle += 1
            continue
        info = infos.get(target)
        if info is None:
            report.skipped_unknown += 1
            continue
        if info.reads_return_address:
            report.skipped_reads_ra += 1
            continue
        if int(m.group("ret"), 16) == 0:
            report.skipped_unknown += 1
            continue

        # Replace the push32 token, keep the call and any spacing/comments.
        push_start = m.start()
        call_start = m.start("call")
        prefix = text[push_start:call_start]
        rewritten = re.sub(
            r"push32\s*\(\s*c\s*,\s*0x[0-9A-Fa-f]{8}u?\s*\)\s*;",
            "c->esp -= 4;", prefix, count=1)
        if rewritten == prefix:
            report.errors.append(f"could not rewrite push at {m.start()}")
            continue
        pieces.append(text[last:push_start])
        pieces.append(rewritten)
        last = call_start
        report.elided += 1
    pieces.append(text[last:])
    out = "".join(pieces)

    # ABI-11.2: only the exact push -> esp rewrite is allowed.  Re-running the
    # deterministic transform per changed function is the structural check.
    before_bodies = function_bodies(text)
    after_bodies = function_bodies(out)
    for va in sorted(set(before_bodies) | set(after_bodies)):
        before = before_bodies.get(va)
        after = after_bodies.get(va)
        if before is None or after is None:
            report.errors.append(f"function set changed for L_{va}")
            continue
        # Unchanged functions do not belong in the function-level change set.
        if normalize_body(before) == normalize_body(after):
            continue
        # The transform above only replaces `push32(...);` with `c->esp -= 4;`.
        # Canonicalize just those two forms; any other body edit still differs.
        if structural_body(before) != structural_body(after):
            report.errors.append(f"unexpected body change in L_{va}")
            continue
        report.changed_functions += 1
        report.change_set.append({
            "va": va,
            "pass_id": "return_address_elide",
            "before_sha256": body_sha(before),
            "after_sha256": body_sha(after),
            "reason": "return-address store elided before a direct call whose "
                      "target does not read [esp]",
        })
    return out


def verify_text(text: str, report: Report) -> list[str]:
    errors: list[str] = []
    if report.elided and not report.change_set:
        errors.append("elided sites but no function-level change set")
    if report.elided < 0:
        errors.append("negative elision count")
    # The call itself must remain; only the push becomes an esp adjustment.
    # A kept HLE site still starts with push32, so detect the actual forbidden
    # shape instead of treating every HLE call site as rewritten.
    for m in HLE_AFTER_ESP_RE.finditer(text):
        target = int(
            m.group("dispatch") or m.group("dispatch_l") or m.group("direct"),
            16,
        )
        if HLE_LO <= target < HLE_HI:
            errors.append("HLE call was rewritten")
            break
    return errors


def process_files(sources: list[Path], outputs: list[Path | None],
                  reports: list[Report]) -> None:
    texts = [p.read_text(encoding="utf-8", errors="replace") for p in sources]
    infos: dict[str, FunctionInfo] = {}
    for text in texts:
        infos.update(index_functions(text))
    compute_tail_closures(infos)
    for text, out, report in zip(texts, outputs, reports):
        report.functions = len(function_bodies(text))
        transformed = transform_text(text, infos, report)
        report.errors.extend(verify_text(transformed, report))
        if out is not None and not report.errors:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(transformed, encoding="utf-8", newline="")


def self_test() -> int:
    fixture = """#include "cpu.h"
void L_00401000(CPU *c)
{
    /* safe: no stack read */
    c->eax = 1u;
    c->esp += 4; return;
}
void L_00401010(CPU *c)
{
    /* unsafe: reads the entry return-address slot */
    c->eax = (uint32_t)(rd32((c->esp)));
    c->esp += 4; return;
}
void L_00401020(CPU *c)
{
    /* safe: reads a value it pushed itself */
    push32(c, 7u);
    c->eax = (uint32_t)(pop32(c));
    c->esp += 4; return;
}
void L_00401030(CPU *c)
{
    /* unsafe: pops the entry return-address slot */
    c->eax = (uint32_t)(pop32(c));
    c->esp += 4; return;
}
void L_00401040(CPU *c)
{
    /* unsafe: tails to a reader */
    WC3_DIRECT_TAIL(L_00401010, c);
    return;
}
void L_00401050(CPU *c)
{
    /* safe standard frame: [ebp+8] is an argument, not the return address */
    push32(c, c->ebp);
    c->ebp = (uint32_t)(c->esp);
    c->eax = (uint32_t)(rd32((c->ebp + 8u)));
    c->ebp = (uint32_t)(pop32(c));
    c->esp += 4; return;
}
void L_00401060(CPU *c)
{
    /* unsafe standard frame: [ebp+4] is the return address */
    push32(c, c->ebp);
    c->ebp = (uint32_t)(c->esp);
    c->eax = (uint32_t)(rd32((c->ebp + 4u)));
    c->ebp = (uint32_t)(pop32(c));
    c->esp += 4; return;
}
void L_00402000(CPU *c)
{
    push32(c, 0x00402010u); dispatch(c, 0x00401000u);
    push32(c, 0x00402020u); dispatch(c, 0x00401010u);
    push32(c, 0x00402030u); dispatch(c, 0x00401020u);
    push32(c, 0x00402040u); dispatch(c, 0x00401030u);
    push32(c, 0x00402050u); dispatch(c, 0x00401040u);
    push32(c, 0x00402060u); dispatch(c, 0x00401050u);
    push32(c, 0x00402070u); dispatch(c, 0x00401060u);
    push32(c, 0x00402080u); dispatch(c, 0x7F000010u);
    /* push32(c, 0x00402090u); dispatch(c, 0x00401000u); */
    const char *s = "push32(c, 0x004020A0u); dispatch(c, 0x00401000u);";
    (void)s;
    c->esp += 4; return;
}
"""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fixture.c"
        out = Path(td) / "out.c"
        src.write_text(fixture, encoding="utf-8")
        report = Report(file=src.as_posix(), output=out.as_posix())
        process_files([src], [out], [report])
        if report.errors:
            print("return_address_elide self-test: FAIL errors", report.errors,
                  file=sys.stderr)
            return 1
        text = out.read_text(encoding="utf-8")
        checks = {
            "safe call rewritten":
                "c->esp -= 4; dispatch(c, 0x00401000u);" in text,
            "ra reader kept":
                "push32(c, 0x00402020u); dispatch(c, 0x00401010u);" in text,
            "balanced pop rewritten":
                "c->esp -= 4; dispatch(c, 0x00401020u);" in text,
            "entry pop kept":
                "push32(c, 0x00402040u); dispatch(c, 0x00401030u);" in text,
            "tail reader kept":
                "push32(c, 0x00402050u); dispatch(c, 0x00401040u);" in text,
            "frame safe rewritten":
                "c->esp -= 4; dispatch(c, 0x00401050u);" in text,
            "frame reader kept":
                "push32(c, 0x00402070u); dispatch(c, 0x00401060u);" in text,
            "hle kept":
                "push32(c, 0x00402080u); dispatch(c, 0x7F000010u);" in text,
            "comment untouched":
                "/* push32(c, 0x00402090u); dispatch(c, 0x00401000u); */"
                in text,
            "literal untouched":
                '"push32(c, 0x004020A0u); dispatch(c, 0x00401000u);"' in text,
        }
        bad = [name for name, ok in checks.items() if not ok]
        if bad:
            print(f"return_address_elide self-test: FAIL {bad}",
                  file=sys.stderr)
            return 1
        if report.elided != 3:
            print(f"return_address_elide self-test: FAIL elided={report.elided}",
                  file=sys.stderr)
            return 1
        if not report.change_set:
            print("return_address_elide self-test: FAIL empty change set",
                  file=sys.stderr)
            return 1
        if report.changed_functions != 1:
            print("return_address_elide self-test: FAIL changed_functions="
                  f"{report.changed_functions}, want 1", file=sys.stderr)
            return 1
        if any(e["before_sha256"] == e["after_sha256"]
               for e in report.change_set):
            print("return_address_elide self-test: FAIL unchanged body in "
                  "change set", file=sys.stderr)
            return 1
        required = {"va", "pass_id", "before_sha256", "after_sha256", "reason"}
        if any(not required.issubset(e) for e in report.change_set):
            print("return_address_elide self-test: FAIL change set schema",
                  file=sys.stderr)
            return 1
        forbidden = (
            "void L_00403000(CPU *c)\n{\n"
            "    c->esp -= 4; WC3_DIRECT_CALL(L_7F000010, c);\n"
            "    c->esp += 4; return;\n}\n"
        )
        probe = Report(file="<hle-probe>", output=None)
        probe.elided = 1
        probe.change_set = [{"va": "00403000"}]
        if "HLE call was rewritten" not in verify_text(forbidden, probe):
            print("return_address_elide self-test: FAIL HLE direct-call guard",
                  file=sys.stderr)
            return 1
    print("return_address_elide self-test: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lift", type=Path, help="generated C file")
    ap.add_argument("--out", type=Path, help="output file (single input)")
    ap.add_argument("--in-dir", type=Path, help="directory of generated chunks")
    ap.add_argument("--out-dir", type=Path, help="output directory")
    ap.add_argument("--json", type=Path, help="report path")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--dry-run", action="store_true",
                    help="transform and verify without writing C")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    jobs: list[tuple[Path, Path | None]] = []
    if args.in_dir:
        if not args.in_dir.is_dir():
            print(f"error: missing input directory {args.in_dir}",
                  file=sys.stderr)
            return 2
        for src in sorted(args.in_dir.glob("*.c")):
            out = None if (args.dry_run or args.out_dir is None) \
                else args.out_dir / src.name
            jobs.append((src, out))
    elif args.lift:
        if not args.lift.is_file():
            print(f"error: missing generated C {args.lift}", file=sys.stderr)
            return 2
        jobs.append((args.lift, None if args.dry_run else args.out))
    else:
        print("error: pass --lift/--out, --in-dir/--out-dir, or --self-test",
              file=sys.stderr)
        return 2

    reports = [
        Report(file=src.as_posix(),
               output=out.as_posix() if out else None)
        for src, out in jobs
    ]
    process_files([src for src, _ in jobs], [out for _, out in jobs], reports)
    errors = [f"{r.file}: {e}" for r in reports for e in r.errors]
    changes = [
        {"file": r.file, **entry}
        for r in reports for entry in r.change_set
    ]
    summary = {
        "schema": 1,
        "pass_id": "return_address_elide",
        "files": len(reports),
        "functions": sum(r.functions for r in reports),
        "call_sites": sum(r.call_sites for r in reports),
        "elided": sum(r.elided for r in reports),
        "skipped_reads_ra": sum(r.skipped_reads_ra for r in reports),
        "skipped_unknown": sum(r.skipped_unknown for r in reports),
        "skipped_hle": sum(r.skipped_hle for r in reports),
        "changed_functions": len(changes),
        "change_set": changes,
        "errors": errors,
        "verdict": "pass" if not errors else "fail",
    }
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(summary, indent=2) + "\n",
                             encoding="utf-8")
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"return_address_elide: {summary['elided']} site(s) elided, "
          f"{summary['skipped_reads_ra']} kept (target reads [esp]), "
          f"{summary['skipped_unknown']} kept (unknown target)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
