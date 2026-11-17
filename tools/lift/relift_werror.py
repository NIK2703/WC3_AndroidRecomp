#!/usr/bin/env python3
"""Relift the corpus and build the release target with -Werror.

Steps:
  1. rebuild the lift toolchain (tools/lift/build_tools.ps1 -Force);
  2. refresh reports/generated/corpus.sha256 for the configured lift_rev;
  3. relift build/corpus-full;
  4. run tools/android/build_app.ps1 -Variant release with
     -CMakeArgs -DWC3_CORPUS_WERROR=ON and -NativeBuildArgs "-k 0".  The script
     configures the release tree, builds the release product and enables the
     IDEAPAD distcc worker automatically.

Use --skip-tools/--skip-relift to rerun only part of the pipeline.  Exit code
is non-zero when the release build fails or emits an error.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "build" / "lift-tools"


def run(cmd, *, env=None, cwd=ROOT, quiet=False):
    shown = " ".join(str(x) for x in cmd)
    print(f"+ {shown}")
    p = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(cwd),
        env=env,
        text=True,
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    out = p.stdout or ""
    if not quiet or p.returncode != 0:
        tail = out[-8000:]
        print(tail)
    if p.returncode != 0:
        raise SystemExit(f"command failed rc={p.returncode}: {shown}")
    return out


def lift_rev() -> int:
    cfg = json.loads((ROOT / "config" / "layout.1.27b.json").read_text())
    return int(cfg["corpus_manifest"]["lift_rev"])


def build_tools():
    ps = shutil.which("powershell") or "powershell.exe"
    run([ps, "-ExecutionPolicy", "Bypass", "-File",
         str(ROOT / "tools" / "lift" / "build_tools.ps1"), "-Force"],
        quiet=True)


def refresh_hash(rev: int):
    out = ROOT / "reports" / "generated" / "corpus.sha256"
    layout = ROOT / "config" / "layout.1.27b.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    run([TOOLS / "corpus.exe", "hash",
         "--inputs", ROOT / "build" / "corpus-inputs" / "inputs.txt",
         "--root", ROOT,
         "--generator", "wc3lift",
         "--lift-rev", rev,
         "--layout-file", ROOT / "runtime" / "guest" / "guest_layout.h",
         "--out", out], quiet=True)
    cfg = json.loads(layout.read_text())
    inputs = int(cfg["corpus_manifest"]["inputs"])
    run([TOOLS / "corpusinput.exe", "seal",
         "--layout", layout,
         "--hash", out,
         "--inputs", inputs,
         "--force"], quiet=True)


def relift():
    env = os.environ.copy()
    run([TOOLS / "corpusclosure.exe",
         "--input-list", ROOT / "build" / "corpus-inputs" / "inputs.txt",
         "--out-root", ROOT / "build" / "corpus-full",
         "--work-root", ROOT / "build" / "corpus-full-work",
         "--corpus-hash", ROOT / "reports" / "generated" / "corpus.sha256",
         "--layout-file", ROOT / "runtime" / "guest" / "guest_layout.h",
         "--report", ROOT / "reports" / "generated" / "corpus_closure.json",
         "--tools", TOOLS], env=env)


def werror_build(variant: str, jobs: int):
    ps = shutil.which("powershell") or "powershell.exe"
    cmd = [ps, "-ExecutionPolicy", "Bypass", "-File",
           str(ROOT / "tools" / "android" / "build_app.ps1"),
           "-Variant", variant,
           "-CMakeArgs", "-DWC3_CORPUS_WERROR=ON",
           "-NativeBuildArgs", "-k 0"]
    env = os.environ.copy()
    if jobs > 0:
        env["WC3_DIST_JOBS"] = str(jobs)
    print("+ " + " ".join(str(x) for x in cmd))
    p = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(ROOT),
        env=env,
        text=True,
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return p.returncode, p.stdout or ""


def report(rc: int, out: str) -> int:
    lines = out.splitlines()
    for line in lines:
        if line.startswith("[dist]") or line.startswith("[distcc]"):
            print(line)
    diags = [line.strip() for line in lines
             if ": error:" in line or ": warning:" in line]
    errors = [line for line in diags if ": error:" in line]
    kinds = {}
    for line in diags:
        m = re.search(r": (error|warning): (.*?) \[-W([^,\]]+)", line)
        key = m.group(3) if m else line.split(":", 3)[-1].strip()
        kinds.setdefault(key, []).append(line)

    for key, items in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print(f"\n## {key}: {len(items)}")
        seen = set()
        for line in items[:12]:
            short = line.replace(str(ROOT) + os.sep, "")
            if short not in seen:
                print(short)
                seen.add(short)

    log = ROOT / "build" / "tmp" / "werror_scan.txt"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(out, encoding="utf-8")
    print(f"\nerrors: {len(errors)} / build rc={rc}")
    print(f"full log: {log}")
    return 1 if rc != 0 or errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--variant", choices=["debug", "release"],
                    default="release")
    ap.add_argument("--skip-tools", action="store_true")
    ap.add_argument("--skip-relift", action="store_true")
    args = ap.parse_args()

    if not args.skip_tools:
        build_tools()
    if not args.skip_relift:
        rev = lift_rev()
        refresh_hash(rev)
        relift()

    rc, out = werror_build(args.variant, args.jobs)
    return report(rc, out)


if __name__ == "__main__":
    sys.exit(main())
