#!/usr/bin/env python3
"""Relift the corpus, regenerate chunks and audit them with -Werror.

One-shot developer gate for generated-code warnings:

    python tools/lift/relift_werror.py

Steps:
  1. rebuild the lift toolchain (tools/lift/build_tools.ps1 -Force);
  2. refresh reports/generated/corpus.sha256 for the configured lift_rev;
  3. relift build/corpus-full;
  4. re-run CMake configure so splitcorpus and the LiftPasses see the new
     corpus and tool timestamps;
  5. compile every final generated chunk (leaf_call output when present,
     otherwise w3l_compat) with -Wall -Wextra -Werror and report every
     diagnostic.

Use --skip-tools/--skip-relift/--skip-configure to rerun only part of the
pipeline. Exit code is non-zero when any chunk fails.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "build" / "lift-tools"
CHUNK_ROOT = ROOT / "build" / "android_app" / "debug" / "cmake" / "w3l_compat"
LEAF_ROOT = ROOT / "build" / "android_app" / "debug" / "cmake" / "leaf_call"

DEFAULT_CMAKE = r"E:\Android\Sdk\cmake\3.22.1\bin\cmake.exe"
DEFAULT_CLANG = (
    r"E:\Android\Sdk\ndk\29.0.14206865\toolchains\llvm\prebuilt"
    r"\windows-x86_64\bin\clang.exe"
)

CFLAGS = [
    "--target=aarch64-linux-android26",
    "-std=c11",
    "-O2",
    "-g0",
    "-fno-stack-protector",
    "-U_FORTIFY_SOURCE",
    "-D_FORTIFY_SOURCE=0",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-ffp-contract=off",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-Wno-parentheses-equality",
    "-DGVA_DELTA=0",
    "-DWC3_FLAGS_FAST_PF_AF=0",
]


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


def find_tool(env_name, which_name, explicit_default):
    cand = os.environ.get(env_name)
    if cand and Path(cand).exists():
        return Path(cand)
    found = shutil.which(which_name)
    if found:
        return Path(found)
    if Path(explicit_default).exists():
        return Path(explicit_default)
    raise SystemExit(f"cannot find {which_name}; set {env_name}")


def cache_value(cache: Path, key: str, default: str) -> str:
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return default
    m = re.search(rf"^{re.escape(key)}:[^=]*=(.*)$", text, re.MULTILINE)
    return m.group(1).strip() if m else default


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
    # Flag fusion, flagless ALU, D1 and D2 are unconditional in the generator.
    run([TOOLS / "corpusclosure.exe",
         "--input-list", ROOT / "build" / "corpus-inputs" / "inputs.txt",
         "--out-root", ROOT / "build" / "corpus-full",
         "--work-root", ROOT / "build" / "corpus-full-work",
         "--corpus-hash", ROOT / "reports" / "generated" / "corpus.sha256",
         "--layout-file", ROOT / "runtime" / "guest" / "guest_layout.h",
         "--report", ROOT / "reports" / "generated" / "corpus_closure.json",
         "--tools", TOOLS], env=env)


def configure(build_dir: Path):
    cmake = find_tool("CMAKE", "cmake", DEFAULT_CMAKE)
    run([cmake, "-S", ROOT, "-B", build_dir], quiet=True)


def compile_chunk(path: Path, clang: Path, debug_value: str,
                  latency: str):
    cmd = [clang] + CFLAGS + [
        f"-DWC3_DEBUG={debug_value}",
        f"-DWC3_W3L_LATENCY={latency}",
        "-I", ROOT / "runtime" / "cpu",
        "-I", ROOT / "runtime" / "guest",
        "-I", ROOT / "runtime" / "compat",
        "-include", ROOT / "runtime" / "compat" / "w3l_compat.h",
        "-fsyntax-only", path,
    ]
    p = subprocess.run(
        [str(x) for x in cmd], cwd=str(ROOT),
        text=True, errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return path, p.returncode, p.stdout or ""


def scan(clang: Path, debug_value: str, latency: str, jobs: int):
    chunks = sorted(glob.glob(str(LEAF_ROOT / "**" / "*.c"), recursive=True))
    source = "leaf_call"
    if not chunks:
        chunks = sorted(glob.glob(str(CHUNK_ROOT / "**" / "*.c"),
                                  recursive=True))
        source = "w3l_compat"
    print(f"scanning {len(chunks)} {source} chunks with -Werror")
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = [ex.submit(compile_chunk, Path(c), clang, debug_value,
                             latency) for c in chunks]
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            results.append(fut.result())
            if i % 25 == 0:
                print(f"  {i}/{len(chunks)}")
    fails = [(p, rc, err) for p, rc, err in results if rc != 0]
    print(f"\nfailures: {len(fails)} / {len(results)}")

    kinds = {}
    for p, rc, err in fails:
        lines = [ln for ln in err.splitlines()
                 if ": error:" in ln or ": warning:" in ln]
        if not lines:
            kinds.setdefault("no-diagnostic", []).append(
                (p, err.strip().splitlines()[:3]))
            continue
        for ln in lines:
            m = re.search(r": (error|warning): (.*) \[-W([^,\]]+)", ln)
            key = m.group(3) if m else ln.split(":", 3)[-1].strip()
            kinds.setdefault(key, []).append((p, ln.strip()))

    for key, items in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print(f"\n## {key}: {len(items)}")
        seen = set()
        for p, line in items[:12]:
            short = str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p)
            text = f"{short}: {line}"
            if text not in seen:
                print(text)
                seen.add(text)

    log = ROOT / "build" / "tmp" / "werror_scan.txt"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8") as f:
        for p, rc, err in fails:
            f.write(f"=== {p} (rc={rc})\n{err}\n")
    print(f"\nfull log: {log}")
    return len(fails)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--variant", choices=["debug", "release"], default="debug")
    ap.add_argument("--skip-tools", action="store_true")
    ap.add_argument("--skip-relift", action="store_true")
    ap.add_argument("--skip-configure", action="store_true")
    args = ap.parse_args()

    build_dir = ROOT / "build" / "android_app" / args.variant / "cmake"
    cache = build_dir / "CMakeCache.txt"
    debug_value = cache_value(cache, "WC3_DEBUG", "1")
    latency = cache_value(cache, "WC3_W3L_LATENCY", "100")

    if not args.skip_tools:
        build_tools()
    if not args.skip_relift:
        rev = lift_rev()
        refresh_hash(rev)
        relift()
    if not args.skip_configure:
        configure(build_dir)

    clang = find_tool("CLANG", "clang", DEFAULT_CLANG)
    failures = scan(clang, debug_value, latency, args.jobs)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
