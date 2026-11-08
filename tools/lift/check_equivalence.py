#!/usr/bin/env python3
"""Compare the compiled C lifter with the canonical Python reference (ABI-11).

This is a working *prototype* of G-15, not the enforced gate.  It lifts the
same catalog with build/lift-tools/lift32.exe and with
`python -m tools.lift.lift_one`, normalises both outputs to function bodies,
and publishes a function-level change set.  G-15 stays `missing` until a
golden set exists for every corpus module (V10, Step 5b).

Normalisation follows ABI-11.1: strip comments, collapse whitespace, ignore
formatting and generator banners.  A deliberately corrupted body must produce
`fail` (V5.4) -- use --self-test to prove the checker is not lying.

Exit codes: 0 pass, 1 fail, 2 unavailable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C_TOOL = ROOT / "build" / "lift-tools" / "lift32.exe"
BODY_RE = re.compile(r"void\s+L_([0-9A-Fa-f]{8})\s*\(CPU\s*\*c\)\s*\{", re.MULTILINE)
COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def normalize(text: str) -> dict[str, str]:
    """va -> normalised body text (ABI-11.1)."""
    plain = COMMENT_RE.sub(" ", text)
    matches = list(BODY_RE.finditer(plain))
    out: dict[str, str] = {}
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(plain)
        body = plain[m.end():end]
        # A trailing '}' belongs to this function; keep brace balance visible.
        body = " ".join(body.split())
        out[m.group(1).upper()] = body
    return out


def sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def run(cmd: list[str], label: str) -> tuple[int, str]:
    try:
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    except OSError as exc:
        print(f"{label}: unavailable ({exc})", file=sys.stderr)
        return 2, ""
    if proc.returncode != 0:
        print(f"{label}: exit {proc.returncode}: {proc.stderr.strip()[:300]}", file=sys.stderr)
    return proc.returncode, (proc.stdout + proc.stderr)


def compare(c_text: str, py_text: str) -> dict:
    c_bodies, py_bodies = normalize(c_text), normalize(py_text)
    all_va = sorted(set(c_bodies) | set(py_bodies))
    changes = []
    for va in all_va:
        a, b = c_bodies.get(va), py_bodies.get(va)
        verdict = "equal" if a == b else ("missing_in_c" if a is None else
                  "missing_in_python" if b is None else "different")
        changes.append({
            "va": va,
            "pass_id": "c_lifter_port",
            "before_sha256": sha(b or ""),
            "after_sha256": sha(a or ""),
            "reason": verdict,
        })
    bad = [c for c in changes if c["reason"] != "equal"]
    return {
        "schema": 1,
        "functions": len(changes),
        "mismatches": len(bad),
        "change_set": changes,
        "verdict": "pass" if changes and not bad else "fail",
    }


def checker_self_test() -> int:
    """Prove the normalizer/comparator detects a corrupted body without assets.

    This is the executable part of G-15 that does not need a licensed image or
    the C lifter.  It is not a substitute for the per-module golden set; it
    only proves the checker fails closed when a body changes.
    """
    reference = (
        'void L_00401000(CPU *c) { c->eax = 1u; return; }\n'
        'void L_00401010(CPU *c) { dispatch(c, 0x00401000u); return; }\n'
    )
    identical = compare(reference, reference)
    if identical["verdict"] != "pass" or identical["mismatches"] != 0:
        print("equivalence checker self-test: FAIL identical input",
              file=sys.stderr)
        return 1
    corrupted = reference.replace("c->eax = 1u", "c->eax = 2u", 1)
    poisoned = compare(corrupted, reference)
    if poisoned["verdict"] != "fail" or poisoned["mismatches"] != 1:
        print("equivalence checker self-test: FAIL corruption not detected",
              file=sys.stderr)
        return 1
    missing = compare(reference.splitlines()[0] + "\n", reference)
    if not any(c["reason"] == "missing_in_c" for c in missing["change_set"]):
        print("equivalence checker self-test: FAIL missing function not detected",
              file=sys.stderr)
        return 1
    print("equivalence checker self-test: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", help="licensed PE32 module (local only)")
    ap.add_argument("--funcs", help="catalog: 0xADDR size name per line")
    ap.add_argument("--json", type=Path, help="report path")
    ap.add_argument("--self-test", action="store_true",
                    help="prove fail on a deliberately corrupted body (V5.4)")
    ap.add_argument("--checker-self-test", action="store_true",
                    help="asset-free normalizer/comparator self-test")
    args = ap.parse_args(argv)

    if args.checker_self_test:
        return checker_self_test()
    if not args.image or not args.funcs:
        ap.error("--image and --funcs are required unless --checker-self-test")
    if not C_TOOL.is_file():
        print("equivalence: unavailable (build/lift-tools/lift32.exe missing; run build_tools.ps1)",
              file=sys.stderr)
        return 2
    if not Path(args.image).is_file() or not Path(args.funcs).is_file():
        print("equivalence: unavailable (image or catalog missing)", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        c_out, py_out = tmpdir / "c.c", tmpdir / "py.c"
        rc, _ = run([str(C_TOOL), args.image, args.funcs, str(c_out)], "c-lifter")
        if rc != 0:
            return 2 if rc == 2 else 1
        rc, _ = run([sys.executable, "-m", "tools.lift.lift_one",
                     "--image", args.image, "--funcs", args.funcs,
                     "--out", str(py_out)], "python-reference")
        if rc != 0:
            return 2 if rc == 2 else 1
        c_text = c_out.read_text(encoding="utf-8", errors="replace")
        py_text = py_out.read_text(encoding="utf-8", errors="replace")
        report = compare(c_text, py_text)

    if args.self_test:
        corrupted = py_text
        # Inject a wrong immediate into the first emitted instruction.
        broken = re.sub(r"GVA\(0x([0-9A-Fa-f]{8})\)",
                        lambda m: f"GVA(0x{int(m.group(1), 16) + 4:08X})",
                        corrupted, count=1)
        poisoned = compare(broken, corrupted)
        report["self_test"] = {
            "corrupted_body_detected": poisoned["mismatches"] > 0,
            "verdict": "pass" if poisoned["mismatches"] > 0 else "fail",
        }
        if poisoned["mismatches"] == 0:
            report["verdict"] = "fail"

    report["status"] = "provisional (G-15 = missing; golden set per module required)"
    report["c_tool_sha256"] = hashlib.sha256(C_TOOL.read_bytes()).hexdigest()
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"equivalence: {report['verdict']} "
          f"({report['functions']} functions, {report['mismatches']} mismatches)")
    if report.get("self_test"):
        print(f"equivalence: self-test {report['self_test']['verdict']}")
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
