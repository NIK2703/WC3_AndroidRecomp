#!/usr/bin/env python3
"""Audit shared mutable globals on the as-built HLE call path.

One-shot analysis tool for the multithreading review in
``docs/ARCHITECTURE_1.27B.md`` section 3.1 / ``docs/THREAD_AUDIT.md``.  It
reads one C source file, finds **file-scope** globals that are written outside
their declaration, and produces the shortlist a human must classify with the
ownership rules.  It does not prove races; it finds candidates.

File-scope declarations are recognised by indentation (no leading whitespace),
a terminating semicolon and no parentheses.  Indented ``static`` locals are
deliberately ignored.

Usage:
    python tools/concurrency_audit.py
    python tools/concurrency_audit.py --source <file> --json out.json --strict
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT.parent / "warcraft3_recomp"
DEFAULT_SOURCE = OLD / "runtime" / "android" / "hle_android.c"

DECL_LINE = re.compile(
    r"^(?P<prefix>(?:static\s+)?(?:const\s+|volatile\s+|THREAD_LOCAL\s+|"
    r"__thread\s+|_Thread_local\s+|unsigned\s+|signed\s+)*)"
    r"(?P<type>[A-Za-z_]\w*(?:\s*\*+\s*|\s+))"
    r"(?P<names>[^;()]+);\s*(?:/\*.*)?$"
)
ANON_STRUCT_DECL = re.compile(
    r"^static\s+(?:const\s+)?struct\s*\{.*\}\s*(?P<names>[^;]+);\s*$"
)
FUNC_DEF = re.compile(
    r"^(?:static\s+)?[A-Za-z_][\w *]*\s+"
    r"(?P<name>hle_call|h_[A-Za-z0-9_]+|cr_[A-Za-z0-9_]+)\s*\("
)
WRITE = re.compile(
    r"\b(?P<n>[A-Za-z_]\w*)\b\s*(?:\+\+|--|\+=[^=]|-[^=>]?=[^=]|"
    r"\*=[^=]|/=[^=]|\|=[^=]|&=[^=]|=[^=])"
    r"|\b(?P<arr>[A-Za-z_]\w*)\s*\[[^\]]*\]\s*(?:\.[A-Za-z_]\w*\s*)*"
    r"(?:\+\+|--|\+=[^=]|-[^=>]?=[^=]|=[^=])"
)

# Findings verified by hand against the as-built source; the tool reports
# whether they are still present and how often they are written.
KNOWN = {
    "g_sync_next": ("functional race", "non-atomic ++ in gh_new_sync()"),
    "g_hle_count": ("late-bind race", "written by reg() after bootstrap"),
    "g_last_cpu": ("hot-path race", "overwritten by every hle_call()"),
    "g_ring_pos": ("hot-path race", "HLE ring index on every call"),
    "g_ring_len": ("hot-path race", "HLE ring length on every call"),
    "g_ring": ("hot-path race", "HLE ring buffer on every call"),
    "g_miss_count": ("diagnostic race", "note_missing() without a lock"),
    "g_miss_calls": ("diagnostic race", "note_missing() without a lock"),
    "g_disp_ring": ("diagnostic race", "dispatch trace ring"),
    "g_trace_budget": ("diagnostic race", "trace budget decremented globally"),
    "g_handles": ("unsynchronized table", "gh_new() linear scan without a lock"),
}
PROTECTED = {
    "g_threads": "g_thread_lock",
    "g_cs": "g_cs_lock",
    "g_cs_count": "g_cs_lock",
    "g_heap_lock": "mutex itself",
    "g_cs_lock": "mutex itself",
    "g_thread_lock": "mutex itself",
}
TLS_QUALIFIERS = ("THREAD_LOCAL", "__thread", "_Thread_local")


def audit(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    decls: dict[str, dict] = {}
    current_fn = ""
    writes: dict[str, list[int]] = {}
    write_fns: dict[str, set[str]] = {}

    for no, raw in enumerate(lines, 1):
        line = raw.rstrip()
        if line != line.lstrip() or line.startswith(("#", "/*", "*", "//")):
            # still allow indented code below for write detection
            pass
        if line == line.lstrip() and line.endswith(";") and "(" not in line:
            m = ANON_STRUCT_DECL.match(line) or DECL_LINE.match(line)
            if m:
                prefix = m.groupdict().get("prefix") or ""
                for part in m.group("names").split(","):
                    nm = re.match(r"\s*\*?\s*([A-Za-z_]\w*)", part)
                    if not nm:
                        continue
                    name = nm.group(1)
                    cls = ("thread-private" if any(q in prefix for q in TLS_QUALIFIERS)
                           else "immutable" if "const" in prefix else "shared")
                    decls[name] = {"line": no, "class": cls}
            continue

        fm = FUNC_DEF.match(line)
        if fm:
            current_fn = fm.group("name")

        for m in WRITE.finditer(line):
            name = m.group("n") or m.group("arr")
            if name not in decls or decls[name]["line"] == no:
                continue
            writes.setdefault(name, []).append(no)
            write_fns.setdefault(name, set()).add(current_fn or "<top-level>")

    rows = []
    for name, meta in sorted(decls.items()):
        if meta["class"] != "shared":
            continue
        w = writes.get(name, [])
        if not w:
            continue
        finding = KNOWN.get(name)
        rows.append({
            "name": name,
            "declared": meta["line"],
            "writes": len(w),
            "first_write": w[0],
            "written_from": sorted(write_fns.get(name, set()))[:6],
            "classification": finding[0] if finding else (
                "protected" if name in PROTECTED else "unclassified shared"),
            "note": finding[1] if finding else PROTECTED.get(name, "needs owner"),
        })
    known = [r for r in rows if r["classification"] in
             ("functional race", "late-bind race", "hot-path race",
              "diagnostic race", "unsynchronized table", "per-entry counter")]
    return {
        "source": path.as_posix(),
        "file_scope_globals": len(decls),
        "shared_written_globals": len(rows),
        "verified_findings": len(known),
        "verified_findings_present": sorted(r["name"] for r in known),
        "rows": rows,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when verified findings remain")
    args = ap.parse_args(argv)
    if not args.source.is_file():
        print(f"error: missing source {args.source}", file=sys.stderr)
        return 2
    report = audit(args.source)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if (args.strict and report["verified_findings"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
