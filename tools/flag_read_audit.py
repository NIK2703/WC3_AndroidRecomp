#!/usr/bin/env python3
"""Audit lifted C for live PF/AF reads before P0-10 is enabled.

P0-10 lets the flag helpers skip `parity8()` and the AF arithmetic.  That is
safe only for a corpus where no instruction observes PF or AF.  This tool is
the missing proof step: it scans generated C for direct `c->pf`/`c->af` reads,
`eflags_pack()` (pushf), `lahf`, and SETcc forms that use PF.

The audit is intentionally textual and conservative.  It does not prove the
absence of a read hidden behind a macro; it reports every such construct as
unavailable, and the build must keep WC3_FLAGS_FAST_PF_AF=0.

Usage:
    python tools/flag_read_audit.py --lift build/.../game/closed.c
    python tools/flag_read_audit.py --in-dir build/.../game --json report.json
    python tools/flag_read_audit.py --self-test
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

FUNC_START_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)\s*\{",
    re.MULTILINE)
HEADER_RE = re.compile(
    r"^void\s+L_([0-9A-Fa-f]{8})\s*\(\s*CPU\s*\*\s*c\s*\)")
PF_AF_RE = re.compile(r"\bc->(pf|af)\b")
# A bare `pf`/`af` token or an access through another CPU pointer is also a
# read risk: it can hide behind a macro or a helper that the C text does not
# expand.  `c->pf` is handled above so this pattern excludes only that exact
# form to avoid duplicate findings.
BARE_PF_AF_RE = re.compile(r"(?<!c->)\b(?:pf|af)\b")
PACK_RE = re.compile(r"\b(?:eflags_pack|lahf)\s*\(")
SETP_RE = re.compile(r"\bSET(?:P|PE|NP|NPE)\b")


@dataclass
class Finding:
    file: str
    va: str | None
    line: int
    kind: str
    text: str


@dataclass
class Report:
    file: str
    functions: int = 0
    findings: list[Finding] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def mask_code(line: str) -> str:
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


def scan_text(path: str, text: str) -> Report:
    report = Report(path)
    matches = list(FUNC_START_RE.finditer(text))
    report.functions = len(matches)
    current_va: str | None = None
    for line_no, line in enumerate(text.splitlines(), 1):
        m = HEADER_RE.match(line)
        if m:
            current_va = m.group(1).upper()
        plain = mask_code(line)
        for m in PF_AF_RE.finditer(plain):
            # An assignment target is a write, not a live read.  A compound
            # assignment (+= etc.) is both and is flagged.
            tail = plain[m.start():]
            if re.match(r"c->(?:pf|af)\s*=(?!=)", tail):
                continue
            report.findings.append(
                Finding(path, current_va, line_no, m.group(0).strip(),
                        line.rstrip()))
        for m in BARE_PF_AF_RE.finditer(plain):
            report.findings.append(
                Finding(path, current_va, line_no, m.group(0), line.rstrip()))
        for m in PACK_RE.finditer(plain):
            report.findings.append(
                Finding(path, current_va, line_no, m.group(0).rstrip("("),
                        line.rstrip()))
        for m in SETP_RE.finditer(plain):
            report.findings.append(
                Finding(path, current_va, line_no, m.group(0), line.rstrip()))
    return report


def self_test() -> int:
    clean = """void L_00000001(CPU *c)
{
    c->eax = flags_add(c, c->eax, 1u, 4);
    c->pf = 0; c->af = 1;
    return;
}
"""
    dirty = """void L_00000002(CPU *c)
{
    if (c->pf) return;
    if (c->af) return;
    c->eax = eflags_pack(c);
    return;
}
"""
    r1 = scan_text("clean.c", clean)
    r2 = scan_text("dirty.c", dirty)
    if r1.findings:
        print("flag_read_audit self-test: clean fixture flagged", file=sys.stderr)
        return 1
    if len(r2.findings) < 3:
        print(f"flag_read_audit self-test: dirty findings={len(r2.findings)}, want >=3",
              file=sys.stderr)
        return 1
    print("flag_read_audit self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lift")
    ap.add_argument("--in-dir")
    ap.add_argument("--json")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.lift and not args.in_dir:
        ap.error("--lift or --in-dir is required")

    reports: list[Report] = []
    if args.lift:
        p = Path(args.lift)
        reports.append(scan_text(str(p), p.read_text(encoding="utf-8",
                                                      errors="replace")))
    else:
        files = sorted(Path(args.in_dir).glob("*.c"))
        if not files:
            print(f"flag_read_audit: no .c files in {args.in_dir}",
                  file=sys.stderr)
            return 2
        for p in files:
            reports.append(scan_text(str(p), p.read_text(encoding="utf-8",
                                                         errors="replace")))

    total = sum(len(r.findings) for r in reports)
    data = {
        "schema": 1,
        "status": "pass" if total == 0 else "fail",
        "files": [
            {"file": r.file, "functions": r.functions,
             "findings": len(r.findings), "errors": r.errors}
            for r in reports
        ],
        "findings": [
            {"file": f.file, "va": f.va, "line": f.line,
             "kind": f.kind, "text": f.text}
            for r in reports for f in r.findings
        ],
        "total": total,
    }
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(data, indent=2) + "\n",
                                   encoding="utf-8", newline="")
    if total:
        print(f"flag_read_audit: {total} live PF/AF read(s); "
              f"WC3_FLAGS_FAST_PF_AF must stay 0", file=sys.stderr)
        for r in reports:
            for f in r.findings[:20]:
                print(f"{f.file}:{f.line}: {f.kind}: {f.text}",
                      file=sys.stderr)
        return 1
    print(f"flag_read_audit: pass ({sum(r.functions for r in reports)} functions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
