#!/usr/bin/env python3
"""PASS14 evidence: AArch64 call-site profile of one product .so.

Streams `llvm-objdump -d` and counts direct calls to the emulated-TLS helper,
splitting them by the containing symbol (lifted `L_<va>` vs host code).  Static
site counts only: no dynamic frequency, no frame time, no speed claim.

    python tools/pass14_binary_profile.py --so <path> --json <path>
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import shutil
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SO = ROOT / "build/android_app/debug/cmake/libwc3gameactivity.so"
DEFAULT_OUT = ROOT / "reports/generated/pass14_binary_profile.json"

FUNC_RE = re.compile(r"^[0-9a-fA-F]+\s+<([^>]+)>:\s*$")
CALL_RE = re.compile(r"\bbl\b.*__emutls_get_address")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_objdump(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for name in ("llvm-objdump", "llvm-objdump.exe"):
        found = shutil.which(name)
        if found:
            return found
    for candidate in (
        Path("C:/msys64/clang64/bin/llvm-objdump.exe"),
        Path("C:/msys64/ucrt64/bin/llvm-objdump.exe"),
    ):
        if candidate.is_file():
            return str(candidate)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", default=str(DEFAULT_SO))
    ap.add_argument("--json", default=str(DEFAULT_OUT))
    ap.add_argument("--llvm-objdump", default=None)
    args = ap.parse_args()

    so = Path(args.so)
    if not so.is_absolute():
        so = ROOT / so
    out = Path(args.json)
    if not out.is_absolute():
        out = ROOT / out

    report = {
        "schema": "wc3.pass14_binary_profile.v1",
        "status": "report-only",
        "captured_at_local": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "so": {
            "path": str(so.relative_to(ROOT)).replace("\\", "/") if so.is_relative_to(ROOT) else str(so),
            "status": "present" if so.is_file() else "absent",
        },
        "limits": [
            "Static call-site count, not a dynamic profile (PM-17).",
            "Profiled artifact may be a debug build; release may differ.",
            "llvm-objdump resolves direct call targets by symbol name; inlined TLS accesses are invisible.",
        ],
    }
    tool = find_objdump(args.llvm_objdump)
    if not so.is_file() or not tool:
        report["error"] = "missing .so or llvm-objdump"
        report["available_objdump"] = bool(tool)
    else:
        report["so"].update(
            {
                "bytes": so.stat().st_size,
                "sha256": sha256(so),
                "mtime_local": dt.datetime.fromtimestamp(so.stat().st_mtime)
                .astimezone()
                .isoformat(timespec="seconds"),
            }
        )
        report["tool"] = tool
        cmd = [tool, "-d", "--no-show-raw-insn", str(so)]
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1 << 20
        )
        current, total, lifted, host, insn_lines = "<unknown>", 0, 0, 0, 0
        per_func: Counter[str] = Counter()
        assert proc.stdout is not None
        for line in proc.stdout:
            m = FUNC_RE.match(line)
            if m:
                current = m.group(1)
                continue
            if "\t" in line:
                insn_lines += 1
            if CALL_RE.search(line):
                total += 1
                per_func[current] += 1
                if current.startswith("L_"):
                    lifted += 1
                else:
                    host += 1
        proc.wait()
        report["emutls_calls"] = {
            "total_bl_sites": total,
            "in_lifted_L_symbols": lifted,
            "in_host_symbols": host,
            "disassembled_instruction_lines": insn_lines,
            "top_symbols": [
                {"symbol": sym, "sites": n} for sym, n in per_func.most_common(15)
            ],
        }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    print(f"sha256={sha256(out)}")
    print(json.dumps(report.get("emutls_calls", report.get("error")), ensure_ascii=False)[:400])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
