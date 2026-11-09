#!/usr/bin/env python3
"""Single entry point for host-side checks.

Android device tests are unavailable in this workspace (no SDK/NDK/adb), so
they are listed as `unavailable` and never reported as passed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "host"
_CCACHE = None
_CCACHE_CANDIDATES = (
    "C:/msys64/usr/bin/ccache.exe",
    "C:/msys64/ucrt64/bin/ccache.exe",
    "C:/msys64/mingw64/bin/ccache.exe",
    "C:/msys64/clang64/bin/ccache.exe",
    "C:/ProgramData/chocolatey/bin/ccache.exe",
    "C:/Program Files/ccache/ccache.exe",
)


def find_ccache() -> str | None:
    """Return ccache using the same discovery order as the CMake module."""
    candidates = []
    if os.environ.get("CCACHE"):
        candidates.append(os.environ["CCACHE"])
    candidates.extend(("ccache", "ccache.exe"))
    if os.name == "nt":
        candidates.extend(_CCACHE_CANDIDATES)
    for candidate in candidates:
        resolved = shutil.which(candidate)
        if not resolved and Path(candidate).is_file():
            resolved = candidate
        if resolved:
            return str(Path(resolved)).replace("\\", "/")
    return None


_CCACHE = find_ccache()


def _is_compiler(path: object) -> bool:
    name = Path(str(path)).name.lower()
    return name in {"gcc", "gcc.exe", "g++", "g++.exe",
                    "clang", "clang.exe", "clang++", "clang++.exe",
                    "cc", "cc.exe", "c++", "c++.exe"}


def _with_ccache(cmd: list[object]) -> list[object]:
    actual = list(cmd)
    if _CCACHE and actual and _is_compiler(actual[0]):
        actual = [_CCACHE, str(actual[0]).replace("\\", "/"), *actual[1:]]
    return actual


def _execute(cmd: list[object], cwd: Path = ROOT) -> int:
    actual = _with_ccache(cmd)
    print("+ " + " ".join(str(x) for x in actual))
    return subprocess.run(actual, cwd=cwd).returncode


def _compile_and_link(cmd: list[str], cwd: Path) -> int | None:
    """Split a one-shot compile+link so ccache can cache the compile stage.

    ccache marks a compiler invocation with sources and ``-o executable`` as
    "called for linking" and cannot cache it.  Host tests and small tools are
    usually written that way, so compile each source with ``-c`` first and then
    link the objects.  Commands that already use ``-c``, preprocess, or dump
    assembly are left alone.
    """
    actual = [str(x) for x in cmd]
    if len(actual) < 2 or not _is_compiler(actual[0]):
        return None
    if any(flag in actual for flag in ("-c", "-E", "-S")):
        return None
    try:
        out_index = actual.index("-o")
        output = actual[out_index + 1]
    except (ValueError, IndexError):
        return None
    if Path(output).suffix.lower() in {".o", ".obj"}:
        return None

    source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".s", ".asm"}

    def is_source(arg: str) -> bool:
        path = Path(arg)
        if not path.is_absolute():
            path = cwd / path
        return path.suffix.lower() in source_suffixes and path.is_file()

    sources = [arg for arg in actual[1:] if is_source(arg)]
    if not sources:
        return None

    source_set = set(sources)
    base = [actual[0]]
    skip_output = False
    for arg in actual[1:]:
        if skip_output:
            skip_output = False
            continue
        if arg == "-o":
            skip_output = True
            continue
        if arg in source_set:
            continue
        base.append(arg)

    digest = hashlib.sha256(
        ("\0".join(base) + "\0" + "\0".join(sources)).encode()).hexdigest()[:16]
    objdir = BUILD / "ccache-objects" / digest
    objdir.mkdir(parents=True, exist_ok=True)

    objects: list[str] = []
    for index, source in enumerate(sources):
        obj = objdir / f"{index:02d}_{Path(source).stem}.o"
        objects.append(str(obj))
        rc = _execute([*base, "-c", source, "-o", str(obj)], cwd)
        if rc != 0:
            return rc
    return _execute([*base, *objects, "-o", output], cwd)


def run(cmd: list[str], cwd: Path = ROOT) -> int:
    split_rc = _compile_and_link(cmd, cwd)
    if split_rc is not None:
        return split_rc
    return _execute(cmd, cwd=cwd)


def gcc() -> str | None:
    return shutil.which("gcc") or shutil.which("clang")


def test_cpu_helpers() -> int:
    """T0: integer CPU helpers keep x86 flag and count semantics."""
    cc = gcc()
    if not cc:
        print("cpu helpers: unavailable (no host C compiler)")
        return 2
    archive = ROOT / "third_party" / "softfloat" / "build-host" / "softfloat.a"
    if not archive.is_file():
        print("cpu helpers: unavailable (host SoftFloat archive missing)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "cpu_helpers_test.exe"
    obj = BUILD / "cpu_helpers_test.o"
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    rc = run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              *include, "-c", str(ROOT / "tests" / "cpu_helpers_test.c"),
              "-o", str(obj)])
    if rc != 0:
        return rc
    rc = run([cc, str(obj), str(archive), "-lm", "-o", str(exe)])
    if rc != 0:
        return rc
    return run([str(exe)])


def test_hle_abi() -> int:
    cc = gcc()
    if not cc:
        print("hle ABI: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle" / "abi"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "abi" / "hle_abi.c"),
        str(ROOT / "tests" / "hle_abi_test.c"),
    ]
    for name, debug in (("hle_abi_test_debug.exe", 1), ("hle_abi_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_imports() -> int:
    kind = "table"
    fixture = ROOT / "config" / "imports.1.27b.json"
    try:
        if json.loads(fixture.read_text(encoding="utf-8")).get(
            "pe_fixture_status"
        ) == "available":
            kind = "pe"
    except (OSError, ValueError):
        pass
    return run([sys.executable, str(ROOT / "tools" / "hle_coverage.py"),
                "--corpus", "1.27b", "--fixture-kind", kind])


def test_thread_audit() -> int:
    # No weakening flag here.  Until the T2 evidence exists this test reports
    # unavailable (exit 2); the default report-only run may summarise it, but
    # the acceptance run may not call it with --allow-unavailable (V1.4).
    return run([sys.executable, str(ROOT / "tools" / "check_thread_audit.py"),
                "--corpus", "1.27b"])


def test_layout() -> int:
    # G-05: the canonical gate must refuse a corpus with no sealed identity.
    return run([sys.executable, str(ROOT / "tools" / "check_layout.py"),
                "--corpus", "1.27b", "--require-corpus"])


def test_config_schema() -> int:
    """R10: every config/*.json matches its config/schema/*.schema.json."""
    return run([sys.executable, str(ROOT / "tools" / "check_config_schema.py")])


def test_profile_corpus() -> int:
    """O-19/O-30: static profile counters and identity checks are sane."""
    return run([sys.executable, str(ROOT / "tools" / "profile_corpus.py"),
                "--self-test"])


def test_boot_dispatch_hash() -> int:
    """O-24/O-53: the generated merged table matches the runtime lookup."""
    return run([sys.executable, str(ROOT / "tools" / "gen_boot_dispatch.py"),
                "--self-test"])


def test_boot_dispatch_compile() -> int:
    """O-53: the generated LiftEnt table compiles in the product dispatcher."""
    cc = gcc()
    if not cc:
        print("boot dispatch compile: unavailable (no host C compiler)")
        return 2

    work = BUILD / "boot_dispatch_compile"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    funcs = work / "funcs.h"
    funcs.write_text(
        "#define LIFTED_FUNCS(X) X(00400000) X(00400004) X(35000000)\n",
        encoding="ascii")
    gen_list = work / "boot_dispatch_list.h"
    gen_hash = work / "boot_dispatch_hash.h"
    rc = run([sys.executable, str(ROOT / "tools" / "gen_boot_dispatch.py"),
              "--list", str(funcs), "--prefix", "004",
              "--out", str(gen_list), "--hash-out", str(gen_hash)])
    if rc != 0:
        return rc

    # dispatch_boot.c includes the bridge headers on purpose.  Build the
    # include set from the real tree so a moved HLE header fails this check.
    include_dirs = [
        ROOT / "runtime" / "cpu",
        ROOT / "runtime" / "guest",
        ROOT / "runtime" / "boot",
        ROOT / "runtime" / "hle",
        ROOT / "runtime" / "legacy",
        ROOT / "runtime" / "platform",
        ROOT / "runtime" / "platform" / "android",
        ROOT / "runtime" / "diag",
        ROOT / "third_party" / "softfloat" / "source" / "include",
        ROOT / "third_party" / "softfloat" / "source" / "8086-SSE",
    ]
    include_dirs.extend(
        path for path in (ROOT / "runtime" / "hle").rglob("*")
        if path.is_dir())
    include = ["-I" + str(path) for path in include_dirs]
    return run([cc, "-std=c11", "-fsyntax-only", "-DWC3_DEBUG=0",
                "-I" + str(work), *include,
                str(ROOT / "runtime" / "boot" / "dispatch_boot.c")])


def test_softfloat_manifest() -> int:
    """O-52: native TLS can replace the emulated-TLS SoftFloat archive."""
    return run([sys.executable, str(ROOT / "tools" /
                                    "check_softfloat_manifest.py")])


def test_x87_publication_guard() -> int:
    """O-22: SoftFloat publication keeps the reviewed exactness shape."""
    return run([sys.executable, str(ROOT / "tools" / "check_x87_guard.py")])


def test_splitcorpus_packing() -> int:
    """O-23: keep the component-aware planner in the splitter source."""
    return run([sys.executable, str(ROOT / "tools" / "check_splitcorpus_packing.py")])


def test_splitcorpus_order() -> int:
    """O-23: splitcorpus must emit caller/callee neighbours together."""
    exe = ROOT / "build" / "lift-tools" / "splitcorpus.exe"
    if not exe.is_file():
        print("splitcorpus_order: unavailable (prebuilt splitcorpus.exe missing)")
        return 2
    work = BUILD / "splitcorpus_order"
    if work.exists():
        shutil.rmtree(work)
    outdir = work / "out"
    outdir.mkdir(parents=True)
    src = work / "fixture.c"
    src.write_text(
        "void L_00401000(CPU *c) {\n"
        "    dispatch(c, 0x00404000u);\n"
        "}\n"
        "void L_00402000(CPU *c) {\n"
        "    dispatch(c, 0x00402010u);\n"
        "}\n"
        "void L_00404000(CPU *c) {\n"
        "    dispatch(c, 0x00401000u);\n"
        "}\n"
        "void L_00403000(CPU *c) {\n"
        "    dispatch(c, 0x00403010u);\n"
        "}\n",
        encoding="ascii")
    rc = run([str(exe), str(src), str(outdir), "o23", "500", "16"])
    if rc != 0:
        return rc
    chunks = sorted(outdir.glob("o23_*.c"))
    if len(chunks) != 1:
        print(f"splitcorpus_order: expected 1 chunk, got {len(chunks)}")
        return 1
    order = re.findall(r"^void L_([0-9A-Fa-f]+)\(",
                       chunks[0].read_text(encoding="ascii"), re.M)
    expected = ["00401000", "00404000", "00402000", "00403000"]
    if order != expected:
        print(f"splitcorpus_order: got {order}, expected {expected}")
        return 1
    print("splitcorpus_order: graph order verified")
    return 0


def test_splitcorpus_source_order() -> int:
    """O-23: rebuild splitcorpus from source and verify the graph order."""
    cc = gcc()
    if not cc:
        print("splitcorpus_source_order: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "splitcorpus_source.exe"
    source = ROOT / "tools" / "lift" / "csrc_local" / "splitcorpus.c"
    dirwalk = ROOT / "tools" / "lift" / "csrc_local" / "dirwalk.c"
    rc = run([
        cc, "-std=c11", "-O2", "-Wall", "-Wextra",
        "-I" + str(source.parent),
        str(source), str(dirwalk), "-o", str(exe),
    ])
    if rc != 0:
        return rc
    work = BUILD / "splitcorpus_source_order"
    if work.exists():
        shutil.rmtree(work)
    outdir = work / "out"
    outdir.mkdir(parents=True)
    src = work / "fixture.c"
    src.write_text(
        "void L_00401000(CPU *c) {\n"
        "    dispatch(c, 0x00404000u);\n"
        "}\n"
        "void L_00402000(CPU *c) {\n"
        "    dispatch(c, 0x00402010u);\n"
        "}\n"
        "void L_00404000(CPU *c) {\n"
        "    dispatch(c, 0x00401000u);\n"
        "}\n"
        "void L_00403000(CPU *c) {\n"
        "    dispatch(c, 0x00403010u);\n"
        "}\n",
        encoding="ascii")
    rc = run([str(exe), str(src), str(outdir), "o23src", "500", "16"])
    if rc != 0:
        return rc
    chunks = sorted(outdir.glob("o23src_*.c"))
    if len(chunks) != 1:
        print(f"splitcorpus_source_order: expected 1 chunk, got {len(chunks)}")
        return 1
    order = re.findall(r"^void L_([0-9A-Fa-f]+)\(",
                       chunks[0].read_text(encoding="ascii"), re.M)
    expected = ["00401000", "00404000", "00402000", "00403000"]
    if order != expected:
        print(f"splitcorpus_source_order: got {order}, expected {expected}")
        return 1
    print("splitcorpus_source_order: source graph order verified")
    return 0


def test_dxvk_probe_cache() -> int:
    """O-25: a second Vulkan probe is served from the process memo."""
    cc = gcc()
    if not cc:
        print("dxvk_probe_cache: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "dxvk_probe_cache_test.exe"
    rc = run([
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        str(ROOT / "runtime" / "gfx" / "dxvk_probe_cache.c"),
        str(ROOT / "tests" / "dxvk_probe_cache_test.c"),
        str(ROOT / "tests" / "dxvk_probe_cache_real_stub.c"),
        "-pthread", "-Wl,--wrap=wc3_vulkan_major_minor",
        "-o", str(exe),
    ])
    if rc != 0:
        return rc
    return run([str(exe)])


def test_kernel32_file() -> int:
    """O-26/O-27: leaf-case cache and live invalidation on the M5 seam."""
    cc = gcc()
    if not cc:
        print("kernel32_file: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "file"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "file"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "file" /
            "kernel32_file_hle.c"),
        str(ROOT / "tests" / "kernel32_file_test.c"),
        "-pthread",
        "-Wl,--wrap=access",
    ]
    for name, debug, tag in (
            ("kernel32_file_test_debug.exe", 1, 1),
            ("kernel32_file_test_release.exe", 0, 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}",
                  f"-DWC3_DEBUG_HLE={tag}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_corpus_identity() -> int:
    """G-05: the sealed manifest digest must match the real input hash.

    corpusinput.exe and corpus.sha256 are build artefacts, so a clean checkout
    honestly reports `unavailable` instead of a pass (V1.2).
    """
    tool = ROOT / "build" / "lift-tools" / "corpusinput.exe"
    digest = ROOT / "reports" / "generated" / "corpus.sha256"
    if not tool.is_file() or not digest.is_file():
        print("corpus identity: unavailable (run build_tools.ps1 and "
              "corpusinput prepare / corpus hash)")
        return 2
    return run([str(tool), "verify",
                "--layout", str(ROOT / "config" / "layout.1.27b.json"),
                "--hash", str(digest),
                "--evidence", str(ROOT / "reports" / "generated" / "gate_status.json")])


def test_atomic_lift() -> int:
    """T0/V5.9: relifted C uses atomic helpers for memory xchg and lock RMW."""
    tool = ROOT / "build" / "lift-tools" / "atomiccheck.exe"
    corpus = ROOT / "build" / "corpus-full"
    manifest = corpus / "corpus_manifest.json"
    if not tool.is_file() or not manifest.is_file():
        print("atomic lift: unavailable (run build_tools.ps1 and relift)")
        return 2
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"atomic lift: unavailable (manifest: {exc})")
        return 2
    files = []
    for module in data.get("modules", []):
        name = module.get("name")
        if not name:
            continue
        for kind in ("seed", "closed"):
            path = corpus / name / f"{kind}.c"
            if path.is_file():
                files.append(path)
    if not files:
        print("atomic lift: unavailable (manifest has no corpus sources)")
        return 2
    (ROOT / "reports" / "generated").mkdir(parents=True, exist_ok=True)
    args = [str(tool), "--require-any",
            "--report", str(ROOT / "reports" / "generated" / "corpus_atomic.json")]
    for path in files:
        args.extend(["--file", str(path)])
    return run(args)


def test_posix_shim() -> int:
    """BP-8.3: every compiled tool source builds in the POSIX branch.

    The pinned csrc/*.c files are byte-frozen, so portability comes from the
    include-path shim.  WC3_FORCE_POSIX selects that branch even on a Windows
    host; on real POSIX it is harmless because _WIN32 is undefined.
    """
    cc = gcc()
    if not cc:
        print("posix shim: unavailable (no host C compiler)")
        return 2
    shim = ROOT / "tools" / "lift" / "posix_shim"
    csrc = ROOT / "tools" / "lift" / "csrc"
    local = ROOT / "tools" / "lift" / "csrc_local"
    inc = ROOT / "third_party" / "capstone" / "include"
    if not (shim / "windows.h").is_file():
        print("posix shim: unavailable (shim header missing)")
        return 2
    files = sorted(csrc.glob("*.c")) + sorted(local.glob("*.c"))
    common = [cc, "-fsyntax-only", "-std=c11", "-Wall", "-Wextra",
              "-DWC3_FORCE_POSIX", "-I", str(shim), "-I", str(local),
              "-I", str(csrc), "-I", str(inc)]
    failed: list[str] = []
    warnings = 0
    for src in files:
        proc = subprocess.run(_with_ccache([*common, str(src)]), cwd=ROOT,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            failed.append(src.name)
            print(proc.stderr.strip()[:400], file=sys.stderr)
        elif proc.stderr.strip():
            warnings += 1
    print(f"posix shim: {len(files) - len(failed)}/{len(files)} sources, "
          f"{warnings} pre-existing warning(s)")
    if failed:
        print("posix shim FAILED: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


def test_guest_runner() -> int:
    cc = gcc()
    if not cc:
        print("guest runner: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "app"),
        "-I" + str(ROOT / "runtime" / "ipc"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "tests"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "app" / "guest_runner.c"),
        str(ROOT / "runtime" / "ipc" / "mailbox.c"),
        str(ROOT / "runtime" / "platform" / "platform.c"),
        str(ROOT / "tests" / "fake_platform.c"),
        str(ROOT / "tests" / "guest_runner_test.c"),
        "-pthread",
    ]
    for name, debug in (("guest_runner_test_debug.exe", 1),
                        ("guest_runner_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_asset_api() -> int:
    cc = gcc()
    if not cc:
        print("asset API: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "app"),
        "-I" + str(ROOT / "runtime" / "platform"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "app" / "asset_check.c"),
        str(ROOT / "tests" / "asset_api_test.c"),
    ]
    for name, debug in (("asset_api_test_debug.exe", 1),
                        ("asset_api_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_asset_snapshot() -> int:
    """T0: O-45 snapshot growth, truncation and open-failure state."""
    cc = gcc()
    if not cc:
        print("asset snapshot: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "asset_snapshot_test.exe"
    return run([
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(ROOT / "runtime" / "platform" / "android"),
        str(ROOT / "runtime" / "platform" / "android" / "asset_snapshot.c"),
        str(ROOT / "tests" / "asset_snapshot_test.c"),
        "-o", str(exe),
    ]) or run([str(exe)])


def test_platform_contract() -> int:
    return run([sys.executable, str(ROOT / "tools" / "check_arch.py"),
                "--platform-contract", "--concurrency", "--lifecycle", "--release"])


def test_platform_headless() -> int:
    """T0: the verification platform implements all Wc3Platform APIs.

    Real host primitives (memory, files, threads, sync) plus null
    window/audio/surface.  No corpus and no device are required.
    """
    cc = gcc()
    if not cc:
        print("platform headless: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "platform" / "headless"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    headless = ROOT / "runtime" / "platform" / "headless"
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "platform" / "platform.c"),
        str(headless / "headless_mem.c"),
        str(headless / "headless_file.c"),
        str(headless / "headless_time.c"),
        str(headless / "headless_schedule.c"),
        str(headless / "headless_sched.c"),
        str(headless / "headless_thread.c"),
        str(headless / "headless_sync.c"),
        str(headless / "headless_services.c"),
        str(headless / "headless_init.c"),
        str(ROOT / "runtime" / "guest" / "guest_memory.c"),
        str(ROOT / "tests" / "platform_headless_test.c"),
        "-pthread",
    ]
    for name, debug in (("platform_headless_test_debug.exe", 1),
                        ("platform_headless_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_guest_memory() -> int:
    """T0: dynamic range page hints stay hints and are updated on remove."""
    cc = gcc()
    if not cc:
        print("guest memory: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    exe = BUILD / "guest_memory_test.exe"
    rc = run([
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        *include,
        str(ROOT / "tests" / "guest_memory_test.c"),
        "-pthread",
        "-o", str(exe),
    ])
    if rc != 0:
        return rc
    return run([str(exe)])


def test_window_settle() -> int:
    """T0: cold-start surface settle rule shared with the Android shell."""
    cc = gcc()
    if not cc:
        print("window settle: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "apps" / "android_gameactivity" / "src" / "main" /
                    "cpp"),
    ]
    for name, debug in (("window_settle_test_debug.exe", 1),
                        ("window_settle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            *include, f"-DWC3_DEBUG={debug}",
            str(ROOT / "tests" / "window_settle_test.c"),
            "-o", str(exe),
        ])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_waveout_install() -> int:
    """T0: WINMM install selection, including both rollback triggers."""
    cc = gcc()
    if not cc:
        print("waveout install: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "winmm"),
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "8086-SSE"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "winmm" / "waveout_hle.c"),
        str(ROOT / "tests" / "waveout_install_test.c"),
    ]
    cases = (
        ("disabled", 0, "disabled", None),
        ("enabled", 1, "enabled", None),
        ("trigger_on", 0, "enabled", "wave2_v2_on"),
        ("trigger_off", 1, "disabled", "wave2_v2_off"),
    )
    saved_data_dir = os.environ.pop("WC3_DATA_DIR", None)
    saved_audio_v2 = os.environ.pop("WC3_AUDIO_V2", None)
    try:
        for mode, enabled, expected, trigger in cases:
            for debug in (1, 0):
                exe = BUILD / f"waveout_install_test_{mode}_{debug}.exe"
                rc = run([*common, f"-DWC3_AUDIO_V2_ENABLED={enabled}",
                          f"-DWC3_DEBUG={debug}",
                          f"-DWC3_DEBUG_AUDIO={debug}", "-o", str(exe)])
                if rc != 0:
                    return rc
                marker = BUILD / trigger if trigger else None
                if marker:
                    marker.write_text("", encoding="utf-8")
                    os.environ["WC3_DATA_DIR"] = str(BUILD)
                try:
                    rc = run([str(exe), expected])
                finally:
                    if marker:
                        marker.unlink(missing_ok=True)
                    os.environ.pop("WC3_DATA_DIR", None)
                if rc != 0:
                    return rc
    finally:
        os.environ.pop("WC3_DATA_DIR", None)
        if saved_data_dir is not None:
            os.environ["WC3_DATA_DIR"] = saved_data_dir
        if saved_audio_v2 is not None:
            os.environ["WC3_AUDIO_V2"] = saved_audio_v2
    return 0


def test_waveout_runtime() -> int:
    """T0: real WINMM handlers for WOM_DONE, WM_TIMER, spill, NOMEM."""
    cc = gcc()
    if not cc:
        print("waveout runtime: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "winmm"),
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "8086-SSE"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "winmm" / "waveout_hle.c"),
        str(ROOT / "tests" / "waveout_runtime_test.c"),
    ]
    saved_data_dir = os.environ.pop("WC3_DATA_DIR", None)
    saved_audio_v2 = os.environ.pop("WC3_AUDIO_V2", None)
    try:
        for name, debug in (("waveout_runtime_test_debug.exe", 1),
                            ("waveout_runtime_test_release.exe", 0)):
            exe = BUILD / name
            rc = run([*common, "-DWC3_AUDIO_V2_ENABLED=1",
                      f"-DWC3_DEBUG={debug}", f"-DWC3_DEBUG_AUDIO={debug}",
                      "-o", str(exe)])
            if rc != 0:
                return rc
            rc = run([str(exe)])
            if rc != 0:
                return rc
    finally:
        if saved_data_dir is not None:
            os.environ["WC3_DATA_DIR"] = saved_data_dir
        if saved_audio_v2 is not None:
            os.environ["WC3_AUDIO_V2"] = saved_audio_v2
    return 0


def test_waveout_headless() -> int:
    """T0: real WINMM handlers on the real headless Wc3AudioApi."""
    cc = gcc()
    if not cc:
        print("waveout headless: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "winmm"),
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "platform" / "headless"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "8086-SSE"),
    ]
    headless = ROOT / "runtime" / "platform" / "headless"
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "platform" / "platform.c"),
        str(headless / "headless_mem.c"),
        str(headless / "headless_file.c"),
        str(headless / "headless_time.c"),
        str(headless / "headless_schedule.c"),
        str(headless / "headless_sched.c"),
        str(headless / "headless_thread.c"),
        str(headless / "headless_sync.c"),
        str(headless / "headless_services.c"),
        str(headless / "headless_init.c"),
        str(ROOT / "runtime" / "hle" / "win32" / "winmm" / "waveout_hle.c"),
        str(ROOT / "tests" / "waveout_headless_test.c"),
        "-pthread",
    ]
    saved_data_dir = os.environ.pop("WC3_DATA_DIR", None)
    saved_audio_v2 = os.environ.pop("WC3_AUDIO_V2", None)
    try:
        for name, debug in (("waveout_headless_test_debug.exe", 1),
                            ("waveout_headless_test_release.exe", 0)):
            exe = BUILD / name
            rc = run([*common, "-DWC3_AUDIO_V2_ENABLED=1",
                      f"-DWC3_DEBUG={debug}", f"-DWC3_DEBUG_AUDIO={debug}",
                      "-o", str(exe)])
            if rc != 0:
                return rc
            rc = run([str(exe)])
            if rc != 0:
                return rc
    finally:
        if saved_data_dir is not None:
            os.environ["WC3_DATA_DIR"] = saved_data_dir
        if saved_audio_v2 is not None:
            os.environ["WC3_AUDIO_V2"] = saved_audio_v2
    return 0


def test_release_hygiene() -> int:
    """T0: BV-10 marker/source self-test and the live source audit."""
    rc = run([
        sys.executable,
        str(ROOT / "tools" / "check_release_hygiene.py"),
        "--self-test",
    ])
    if rc != 0:
        return rc
    return run([
        sys.executable,
        str(ROOT / "tools" / "check_release_hygiene.py"),
        "--source-audit",
    ])


def test_null_gfx() -> int:
    """T0: canonical D3D9 stream and the three V5.9 invariants."""
    cc = gcc()
    if not cc:
        print("null gfx: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(ROOT / "runtime" / "gfx" / "null"),
        str(ROOT / "runtime" / "gfx" / "null" / "null_stream.c"),
        str(ROOT / "runtime" / "gfx" / "null" / "null_invariants.c"),
        str(ROOT / "tests" / "null_gfx_test.c"),
    ]
    for name, debug in (("null_gfx_test_debug.exe", 1),
                        ("null_gfx_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_difftest() -> int:
    """T1 harness self-test (V5.4): corrupted stream/save/marker fail."""
    return run([sys.executable, str(ROOT / "tools" / "difftest.py"), "self-test"])


def test_dispatch() -> int:
    cc = gcc()
    if not cc:
        print("dispatch: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    generated = BUILD / "wc3_dispatch.c"
    rc = run([sys.executable, str(ROOT / "tools" / "gen_dispatch.py"),
              str(ROOT / "tests" / "fixtures" / "lifted_list.h"), str(generated)])
    if rc != 0:
        return rc
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle" / "abi"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
        "-I" + str(BUILD),
        "-I" + str(ROOT / "tests" / "fixtures"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(generated),
        str(ROOT / "runtime" / "hle" / "abi" / "hle_abi.c"),
        str(ROOT / "tests" / "dispatch_test.c"),
    ]
    for name, debug in (("dispatch_test_debug.exe", 1),
                        ("dispatch_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_direct_call() -> int:
    """P0-1 contract: devirtualized calls must drain deferred tail targets."""
    cc = gcc()
    if not cc:
        print("direct_call: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "direct_call_test.exe"
    rc = run([
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "runtime" / "guest"),
        str(ROOT / "tests" / "direct_call_test.c"),
        "-o", str(exe),
    ])
    if rc != 0:
        return rc
    return run([str(exe)])


def test_devirtualize_pass() -> int:
    """P0-1/G-15 source check: only constant dispatch sites are rewritten."""
    return run([sys.executable,
                str(ROOT / "tools" / "lift" / "passes" / "devirtualize.py"),
                "--self-test"])


def test_dead_flags_pass() -> int:
    """P0-2 source check: only fully overwritten flag writes are elided."""
    return run([sys.executable,
                str(ROOT / "tools" / "lift" / "passes" / "dead_flags.py"),
                "--self-test"])


def test_return_address_elide_pass() -> int:
    """P0-6 source check: only proven non-readers lose the return-address store."""
    return run([sys.executable,
                str(ROOT / "tools" / "lift" / "passes" /
                    "return_address_elide.py"),
                "--self-test"])


def test_flag_read_audit() -> int:
    """P0-10 proof step: a live PF/AF read must fail the audit."""
    return run([sys.executable,
                str(ROOT / "tools" / "flag_read_audit.py"),
                "--self-test"])


def test_corpus_flag_policy() -> int:
    """O-61: the relifted corpus is built at -O2 with -g0 last."""
    source = (ROOT / "cmake" / "Wc3ReliftedCorpus.cmake").read_text(
        encoding="utf-8", errors="replace")
    block_start = source.find("target_compile_options(wc3_corpus_relifted")
    block_end = source.find(")", block_start)
    block = source[block_start:block_end] if block_start >= 0 else ""
    if "-O2" not in block or "-g0" not in block:
        print("corpus_flag_policy: -O2/-g0 missing from corpus flags",
              file=sys.stderr)
        return 1
    if block.find("-O2") > block.find("-g0"):
        print("corpus_flag_policy: -g0 is not after -O2 in the block",
              file=sys.stderr)
        return 1

    build_ninja = (ROOT / "build" / "android_app" / "debug" / "cmake" /
                   "build.ninja")
    if not build_ninja.is_file():
        print("corpus_flag_policy: source policy OK (no build.ninja)")
        return 0
    text = build_ninja.read_text(encoding="utf-8", errors="replace")
    marker = "CMakeFiles/wc3_corpus_relifted.dir/"
    checked = 0
    for block_text in text.split("\nbuild "):
        if marker not in block_text:
            continue
        for line in block_text.splitlines():
            if not line.startswith("  FLAGS = "):
                continue
            flags = line[len("  FLAGS = "):].split()
            if "-g0" not in flags:
                print("corpus_flag_policy: corpus object lacks -g0",
                      file=sys.stderr)
                return 1
            optimizations = [flag for flag in flags
                             if re.fullmatch(r"-O[0-3sg]", flag)]
            if not optimizations or optimizations[-1] != "-O2":
                print(f"corpus_flag_policy: last -O flag is "
                      f"{optimizations[-1] if optimizations else 'missing'}",
                      file=sys.stderr)
                return 1
            debug_flags = [flag for flag in flags
                           if re.fullmatch(r"-g[0-3]?", flag)]
            if not debug_flags or debug_flags[-1] != "-g0":
                print(f"corpus_flag_policy: last -g flag is "
                      f"{debug_flags[-1] if debug_flags else 'missing'}",
                      file=sys.stderr)
                return 1
            checked += 1
            break
    if checked == 0:
        print("corpus_flag_policy: build.ninja has no corpus object flags",
              file=sys.stderr)
        return 1
    print(f"corpus_flag_policy: {checked} corpus object(s) use -O2 -g0")
    return 0


def test_pass6_build_guards() -> int:
    """O-64/O-65: ICF and -fno-plt stay opt-in experiments, OFF by default."""
    cmake = (ROOT / "CMakeLists.txt").read_text(
        encoding="utf-8", errors="replace")
    build_app = (ROOT / "tools" / "android" / "build_app.ps1").read_text(
        encoding="utf-8", errors="replace")
    build_apk = (ROOT / "tools" / "android" / "build_apk.ps1").read_text(
        encoding="utf-8", errors="replace")
    problems: list[str] = []

    def option_default(name: str, expected: str) -> None:
        match = re.search(
            rf"option\({re.escape(name)}\s+\"[^\"]*\"\s+(ON|OFF)\)",
            cmake, re.S)
        if not match or match.group(1) != expected:
            found = match.group(1) if match else "missing"
            problems.append(f"CMake option {name} default is {found}, "
                            f"expected {expected}")

    option_default("WC3_ICF", "OFF")
    option_default("WC3_FNO_PLT", "OFF")
    option_default("WC3_ICF_PRINT_SECTIONS", "ON")
    mode = re.search(
        r'set\(WC3_ICF_MODE\s+"([^"]+)"\s+CACHE STRING', cmake, re.S)
    if not mode or mode.group(1) != "safe":
        problems.append("WC3_ICF_MODE default is not safe")
    if '"-Wl,--icf=${WC3_ICF_MODE}"' not in cmake:
        problems.append("CMake does not pass -Wl,--icf=... to the link")
    if "add_compile_options(-fno-plt)" not in cmake:
        problems.append("CMake does not wire -fno-plt")

    for needle in (
        "$icfValue = if ($Icf) { 'ON' } else { 'OFF' }",
        '"-DWC3_ICF=$icfValue"',
        '"-DWC3_ICF_MODE=$IcfMode"',
        '"-DWC3_ICF_PRINT_SECTIONS=$icfValue"',
        "$fnoPltValue = if ($FnoPlt) { 'ON' } else { 'OFF' }",
        '"-DWC3_FNO_PLT=$fnoPltValue"',
    ):
        if needle not in build_app:
            problems.append(f"build_app.ps1 missing {needle}")
    for needle in ("$nativeParams['Icf'] = $true",
                   "$nativeParams['FnoPlt'] = $true"):
        if needle not in build_apk:
            problems.append(f"build_apk.ps1 missing {needle}")

    if problems:
        for problem in problems:
            print(f"pass6_build_guards: {problem}", file=sys.stderr)
        return 1
    print("pass6_build_guards: WC3_ICF/WC3_FNO_PLT are explicit OFF defaults")
    return 0


def test_pass7_build_guards() -> int:
    """O-70/O-74: RELR and P0-6 stay explicit opt-ins, OFF by default."""
    cmake = (ROOT / "CMakeLists.txt").read_text(
        encoding="utf-8", errors="replace")
    corpus = (ROOT / "cmake" / "Wc3ReliftedCorpus.cmake").read_text(
        encoding="utf-8", errors="replace")
    build_app = (ROOT / "tools" / "android" / "build_app.ps1").read_text(
        encoding="utf-8", errors="replace")
    build_apk = (ROOT / "tools" / "android" / "build_apk.ps1").read_text(
        encoding="utf-8", errors="replace")
    problems: list[str] = []

    def option_default(name: str, expected: str) -> None:
        match = re.search(
            rf"option\({re.escape(name)}\s+\"[^\"]*\"\s+(ON|OFF)\)",
            cmake, re.S)
        if not match or match.group(1) != expected:
            found = match.group(1) if match else "missing"
            problems.append(f"CMake option {name} default is {found}, "
                            f"expected {expected}")

    option_default("WC3_PACK_DYN_RELOCS", "OFF")
    option_default("WC3_RETURN_ADDRESS_ELIDE_P0_6", "OFF")
    mode = re.search(
        r'set\(WC3_PACK_DYN_RELOCS_MODE\s+"([^"]+)"\s+CACHE STRING',
        cmake, re.S)
    if not mode or mode.group(1) != "android":
        problems.append("WC3_PACK_DYN_RELOCS_MODE default is not android")
    if '"-Wl,--pack-dyn-relocs=${WC3_PACK_DYN_RELOCS_MODE}"' not in cmake:
        problems.append("CMake does not pass the selected relocation mode")
    if "if(WC3_RETURN_ADDRESS_ELIDE_P0_6)" not in corpus:
        problems.append("relifted corpus does not gate P0-6")
    if "return_address_elide.py" not in corpus:
        problems.append("relifted corpus does not invoke return_address_elide")
    if '"-DWC3_PACK_DYN_RELOCS_MODE=$PackDynRelocsMode"' not in build_app:
        problems.append("build_app.ps1 does not pass the relocation mode")
    if "$nativeParams['PackDynRelocsMode'] = $PackDynRelocsMode" not in build_apk:
        problems.append("build_apk.ps1 does not pass the relocation mode")

    if problems:
        for problem in problems:
            print(f"pass7_build_guards: {problem}", file=sys.stderr)
        return 1
    print("pass7_build_guards: RELR/P0-6 are explicit OFF opt-ins")
    return 0


def test_icf_audit() -> int:
    """O-64 source check: duplicate grouping folds aliases by VA."""
    return run([sys.executable,
                str(ROOT / "tools" / "icf_audit.py"),
                "--self-test"])


def test_plt_audit() -> int:
    """O-65 source check: AArch64 BL and PLT-slot mapping."""
    return run([sys.executable,
                str(ROOT / "tools" / "plt_audit.py"),
                "--self-test"])


def test_wait_clock_audit() -> int:
    """O-66 guard: legacy wall-clock waits stay read-only and visible."""
    rc = run([sys.executable,
              str(ROOT / "tools" / "wait_clock_audit.py"),
              "--self-test"])
    if rc != 0:
        return rc
    return run([sys.executable,
                str(ROOT / "tools" / "wait_clock_audit.py"),
                "--strict"])


def test_gva_fold() -> int:
    """P0-3 source invariant: the default path must not load g_image_delta."""
    cpu_h = (ROOT / "runtime" / "cpu" / "cpu.h").read_text(
        encoding="utf-8", errors="replace")
    missing = []
    if "#define GVA_DELTA 0" not in cpu_h:
        missing.append("cpu.h: GVA_DELTA default is not 0")
    if "#define GVA(abs) ((uint32_t)(abs))" not in cpu_h:
        missing.append("cpu.h: GVA does not fold the delta")
    for rel in ("cmake/Wc3ReliftedCorpus.cmake", "cmake/Wc3LegacyBoot.cmake"):
        text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        if "GVA_DELTA=0" not in text:
            missing.append(f"{rel}: no explicit GVA_DELTA=0")
    if missing:
        for item in missing:
            print(f"gva_fold: {item}", file=sys.stderr)
        return 1
    print("gva_fold: OK")
    return 0


def test_lift_equivalence_checker() -> int:
    """G-15 checker self-test: a corrupted body must fail closed."""
    return run([sys.executable,
                str(ROOT / "tools" / "lift_equivalence.py"),
                "--checker-self-test"])


def test_direct_call_multi_tu() -> int:
    """G-14: direct/tail state must span TUs and survive mixed dispatch."""
    cc = gcc()
    if not cc:
        print("direct_call_multi_tu: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    generated = BUILD / "wc3_dispatch_multi_tu.c"
    rc = run([sys.executable, str(ROOT / "tools" / "gen_dispatch.py"),
              str(ROOT / "tests" / "fixtures" / "direct_call_list.h"),
              str(generated)])
    if rc != 0:
        return rc
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle" / "abi"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "8086-SSE"),
        "-I" + str(ROOT / "tests"),
        "-I" + str(ROOT / "tests" / "fixtures"),
        "-I" + str(BUILD),
    ]
    common = [
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        *include,
        str(generated),
        str(ROOT / "runtime" / "hle" / "abi" / "hle_abi.c"),
        str(ROOT / "tests" / "direct_call_multi_tu_a.c"),
        str(ROOT / "tests" / "direct_call_multi_tu_b.c"),
        str(ROOT / "tests" / "direct_call_multi_tu_test.c"),
        "-pthread",
    ]
    for name, debug in (("direct_call_multi_tu_debug.exe", 1),
                        ("direct_call_multi_tu_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_file_layout() -> int:
    """R1-R14 from docs/FILE_LAYOUT.md; warnings are known M2-M6 debt."""
    return run([sys.executable, str(ROOT / "tools" / "check_file_layout.py"),
                "--strict"])


def test_docs() -> int:
    """R11: index/link/policy checks; full path inventory arrives with M6."""
    return run([sys.executable, str(ROOT / "tools" / "check_docs.py")])


def test_pass4_build() -> int:
    """T0: PASS4 build inputs without linking the product.

    The real lld smoke is deliberately outside the default gate: it needs the
    Android NDK.  This check keeps the O-42 version-script surface and CMake
    wiring, the O-43 deterministic pack-relocation switches, the O-45 asset
    snapshot fallbacks, and the O-46 release dispatch path from silently
    drifting.
    """
    failures: list[str] = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    map_path = ROOT / "cmake" / "wc3_exports.map"
    if not map_path.is_file():
        print(f"FAIL missing {map_path}", file=sys.stderr)
        return 1
    version_script = map_path.read_text(encoding="utf-8")
    global_match = re.search(r"global:(.*?)(?:local:|$)", version_script, re.S)
    check(global_match is not None, "version script has no global block")
    if global_match:
        exported = {item.rstrip(";") for item in global_match.group(1).split()}
        for symbol in ("JNI_OnLoad", "android_main", "Java_*"):
            check(symbol in exported, f"version script missing {symbol}")
    local_match = re.search(r"local:(.*?)(?:};|$)", version_script, re.S)
    check(local_match is not None, "version script has no local block")
    if local_match:
        locals_ = {item.rstrip(";") for item in local_match.group(1).split()}
        check("*" in locals_, "version script does not localize the rest")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    check("option(WC3_HIDE_DYNSYM" in cmake, "O-42 option missing")
    check("--version-script=${WC3_ROOT}/cmake/wc3_exports.map" in cmake,
          "O-42 version script not linked")
    check("LINK_DEPENDS" in cmake, "O-42 map is not a link dependency")
    check("--pack-dyn-relocs=${WC3_PACK_DYN_RELOCS_MODE}" in cmake,
          "O-43 pack mode not wired")

    build_app = (ROOT / "tools" / "android" / "build_app.ps1").read_text(
        encoding="utf-8")
    check(re.search(
        r"\$packDynRelocs\w*\s*=\s*if \(\$PackDynRelocs\)\s*\{\s*'ON'\s*\}\s*"
        r"else\s*\{\s*'OFF'\s*\}", build_app) is not None,
          "O-43 OFF is not passed on every configure")
    check(re.search(r'"-DWC3_PACK_DYN_RELOCS=\$packDynRelocs\w*"',
                    build_app) is not None,
          "O-43 pack option is not deterministic")
    check("WC3_HIDE_DYNSYM=ON" in build_app and "WC3_HIDE_DYNSYM=OFF" in build_app,
          "O-42 hide option is not explicit per variant")
    check("$hideDynsym = ($Variant -eq 'release') -and (-not $KeepDynsym)" in build_app,
          "O-42 hide policy is not release-only with a KeepDynsym escape")

    build_apk = (ROOT / "tools" / "android" / "build_apk.ps1").read_text(
        encoding="utf-8")
    check("$KeepDynsym" in build_apk, "O-42 KeepDynsym escape hatch missing")
    check("L_[0-9A-Fa-f]{8}" in build_apk, "O-42 L_* leak check missing")
    check(r"\bwc3_" in build_apk, "O-42 wc3_* leak check missing")

    snapshot = (ROOT / "runtime" / "platform" / "android" /
                "asset_snapshot.c").read_text(encoding="utf-8")
    snapshot_h = (ROOT / "runtime" / "platform" / "android" /
                  "asset_snapshot.h").read_text(encoding="utf-8")
    platform = (ROOT / "runtime" / "platform" / "android" /
                "platform_android.c").read_text(encoding="utf-8")
    check("char (*names)[WC3_ASSET_NAME_MAX];" in snapshot_h,
          "O-45 asset snapshot is not count-allocated")
    check("realloc(snapshot->names" in snapshot,
          "O-45 asset snapshot does not grow its name table")
    check("wc3_asset_snapshot_build(&g_asset_snapshot, g_asset_root);" in platform,
          "O-45 platform does not use the count-allocated snapshot")
    check("asset_snapshot.c" in cmake,
          "O-45 snapshot module is not in the platform target")
    check("if (!g_asset_snapshot.truncated)" in platform and
          "asset_scan_enumerate_locked(index, name, cap)" in platform,
          "O-45 truncated snapshot indexing fallback missing")

    dispatch_boot = (ROOT / "runtime" / "boot" / "dispatch_boot.c").read_text(
        encoding="utf-8")
    check("wc3_legacy_hle_call(target, c);" in dispatch_boot,
          "O-46 product dispatcher bypasses the legacy shim")
    legacy_shim = (ROOT / "runtime" / "hle" / "legacy_shim.c").read_text(
        encoding="utf-8")
    direct_start = legacy_shim.find("static void legacy_call_direct")
    direct_end = legacy_shim.find("\n}\n", direct_start)
    direct_body = (legacy_shim[direct_start:direct_end + 3]
                   if direct_start >= 0 and direct_end >= 0 else "")
    check(bool(direct_body) and "e->calls++" not in direct_body,
          "O-46 release direct path still runs the diagnostic counter")

    if failures:
        for message in failures:
            print(f"FAIL {message}", file=sys.stderr)
        return 1
    print("PASS4 build contract OK")
    return 0


def test_upstream_pin() -> int:
    """BP-4.2: vendored C lift sources must match config/build_upstream.json."""
    return run([sys.executable, str(ROOT / "tools" / "lift" / "check_upstream.py")])


def test_t1() -> int:
    """T1: execute the headless DUT and compare it with the oracle.

    V5.7 requires the harness to execute H (`wc3_headless`).  A report-only
    comparison is useful while the DUT is being built, but it is not this gate;
    the historical Windows host is not a substitute (ADR-026/027).
    """
    print("T1: unavailable (wc3_headless is not built; no Windows oracle host)")
    return 2


def test_t2() -> int:
    """Run the device end-to-end gate.  No device is never a pass."""
    script = ROOT / "tools" / "device_test.py"
    if not script.exists():
        print("T2: unavailable (tools/device_test.py is not implemented)")
        return 2
    return run([sys.executable, str(script), "--corpus", "1.27b"])


def test_module_registry() -> int:
    """Cross-module binding: lifted export, HLE thunk, unresolved import."""
    cc = gcc()
    if not cc:
        print("module registry: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "guest" / "guest_modules.c"),
        str(ROOT / "runtime" / "guest" / "pe.c"),
        str(ROOT / "runtime" / "guest" / "guest_memory.c"),
        str(ROOT / "tests" / "module_registry_test.c"),
    ]
    for name, debug in (("module_registry_test_debug.exe", 1),
                        ("module_registry_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_corpus_binding() -> int:
    """Real 1.27b corpus through wc3_pe_load and the module registry.

    Asset-gated: build/corpus-inputs comes from `corpusinput prepare`.  When it
    is absent the test reports `unavailable`, never a pass.
    """
    cc = gcc()
    if not cc:
        print("corpus binding: unavailable (no host C compiler)")
        return 2
    corpus = ROOT / "build" / "corpus-inputs"
    if not (corpus / "war3.exe").is_file():
        print("corpus binding: unavailable (build/corpus-inputs missing)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "guest" / "guest_modules.c"),
        str(ROOT / "runtime" / "guest" / "pe.c"),
        str(ROOT / "runtime" / "guest" / "guest_memory.c"),
        str(ROOT / "tests" / "corpus_binding_test.c"),
    ]
    for name, debug in (("corpus_binding_test_debug.exe", 1),
                        ("corpus_binding_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe), str(corpus)])
        if rc != 0:
            return rc
    return 0


def test_hle_process() -> int:
    """T0: kernel32/process identity handlers against a fake platform."""
    cc = gcc()
    if not cc:
        print("hle process: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "process"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "process" / "process_hle.c"),
        str(ROOT / "tests" / "process_hle_test.c"),
    ]
    for name, debug in (("process_hle_test_debug.exe", 1),
                        ("process_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_user32_window() -> int:
    """T0: USER32 display metrics, desktop window and cursor counter."""
    cc = gcc()
    if not cc:
        print("user32 window: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "window"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "window" / "window_hle.c"),
        str(ROOT / "tests" / "user32_window_test.c"),
    ]
    for name, debug in (("user32_window_test_debug.exe", 1),
                        ("user32_window_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_user32_display() -> int:
    """T0: USER32 display enumeration and mode-change contracts."""
    cc = gcc()
    if not cc:
        print("user32 display: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "display"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "display" / "display_hle.c"),
        str(ROOT / "tests" / "user32_display_test.c"),
    ]
    for name, debug in (("user32_display_test_debug.exe", 1),
                        ("user32_display_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_hle_system() -> int:
    """T0: kernel32 version and machine-information handlers."""
    cc = gcc()
    if not cc:
        print("hle system: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "process"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "process" / "system_hle.c"),
        str(ROOT / "tests" / "system_hle_test.c"),
    ]
    for name, debug in (("system_hle_test_debug.exe", 1),
                        ("system_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_hle_memory() -> int:
    """T0: kernel32/memory heap handlers against a fake guest heap."""
    cc = gcc()
    if not cc:
        print("hle memory: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "memory"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "memory" / "memory_hle.c"),
        str(ROOT / "tests" / "memory_hle_test.c"),
    ]
    for name, debug in (("memory_hle_test_debug.exe", 1),
                        ("memory_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_hle_string() -> int:
    """T0: kernel32 lstrlenA/lstrcpynA against a fake guest arena."""
    cc = gcc()
    if not cc:
        print("hle string: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "string"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "string" / "string_hle.c"),
        str(ROOT / "tests" / "string_hle_test.c"),
    ]
    for name, debug in (("string_hle_test_debug.exe", 1),
                        ("string_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_hle_resource() -> int:
    """T0: kernel32 PE resource lookup against a synthetic guest image."""
    cc = gcc()
    if not cc:
        print("hle resource: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "resource"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "resource" / "resource_hle.c"),
        str(ROOT / "tests" / "kernel32_resource_test.c"),
    ]
    for name, debug in (("kernel32_resource_test_debug.exe", 1),
                        ("kernel32_resource_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_comctl32_imagelist() -> int:
    """T0: COMCTL32 image-list handles and drag state."""
    cc = gcc()
    if not cc:
        print("comctl32 imagelist: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "comctl32" / "imagelist"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "comctl32" / "imagelist" / "imagelist_hle.c"),
        str(ROOT / "tests" / "comctl32_imagelist_test.c"),
        "-pthread",
    ]
    for name, debug in (("comctl32_imagelist_test_debug.exe", 1),
                        ("comctl32_imagelist_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_gdi_dc() -> int:
    """T0: GDI32 per-DC color, mode and alignment state."""
    cc = gcc()
    if not cc:
        print("gdi dc: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "dc"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "dc" / "gdi_dc_hle.c"),
        str(ROOT / "tests" / "gdi_dc_test.c"),
        "-pthread",
    ]
    for name, debug in (("gdi_dc_test_debug.exe", 1),
                        ("gdi_dc_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_user32_menu() -> int:
    """T0: USER32 menu objects and MENUITEMINFO accessors."""
    cc = gcc()
    if not cc:
        print("user32 menu: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "menu"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "menu" / "menu_hle.c"),
        str(ROOT / "tests" / "user32_menu_test.c"),
        "-pthread",
    ]
    for name, debug in (("user32_menu_test_debug.exe", 1),
                        ("user32_menu_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_scroll_hle() -> int:
    """T0: USER32 scroll-bar state and SCROLLINFO accessors."""
    cc = gcc()
    if not cc:
        print("scroll: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "scroll"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "scroll" / "scroll_hle.c"),
        str(ROOT / "tests" / "scroll_hle_test.c"),
        "-pthread",
    ]
    for name, debug in (("scroll_hle_test_debug.exe", 1),
                        ("scroll_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_gdi_objects() -> int:
    """T0: GDI32 regions, pens and per-DC line state."""
    cc = gcc()
    if not cc:
        print("gdi objects: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "objects"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "objects" / "gdi_obj_hle.c"),
        str(ROOT / "tests" / "gdi_obj_test.c"),
        "-pthread",
    ]
    for name, debug in (("gdi_obj_test_debug.exe", 1),
                        ("gdi_obj_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_window_facts() -> int:
    """T0: USER32 window facts recorded through the chained as-built handlers."""
    cc = gcc()
    if not cc:
        print("window facts: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "window_facts"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "window_facts" / "window_facts_hle.c"),
        str(ROOT / "tests" / "user32_window_facts_test.c"),
        "-pthread",
    ]
    for name, debug in (("user32_window_facts_test_debug.exe", 1),
                        ("user32_window_facts_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_user_object() -> int:
    """T0: USER32 process window station and object information."""
    cc = gcc()
    if not cc:
        print("user object: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "user32" / "user_object"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "user32" / "user_object" / "user_object_hle.c"),
        str(ROOT / "tests" / "user_object_test.c"),
        "-pthread",
    ]
    for name, debug in (("user_object_test_debug.exe", 1),
                        ("user_object_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_gdi_device() -> int:
    """T0: GDI32 gamma-ramp state and display device caps."""
    cc = gcc()
    if not cc:
        print("gdi device: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "device"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "device" / "gdi_device_hle.c"),
        str(ROOT / "tests" / "gdi_device_test.c"),
        "-pthread",
    ]
    for name, debug in (("gdi_device_test_debug.exe", 1),
                        ("gdi_device_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_dragdrop() -> int:
    """T0: SHELL32 drag/drop reports the empty Android drop list."""
    cc = gcc()
    if not cc:
        print("drag/drop: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "shell32" / "dragdrop"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "shell32" / "dragdrop" / "dragdrop_hle.c"),
        str(ROOT / "tests" / "dragdrop_test.c"),
        "-pthread",
    ]
    for name, debug in (("dragdrop_test_debug.exe", 1),
                        ("dragdrop_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_gdi_palette() -> int:
    """T0: GDI32 palette handles, selection and system-palette entries."""
    cc = gcc()
    if not cc:
        print("gdi palette: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "palette"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "gdi32" / "palette" / "gdi_palette_hle.c"),
        str(ROOT / "tests" / "gdi_palette_test.c"),
        "-pthread",
    ]
    for name, debug in (("gdi_palette_test_debug.exe", 1),
                        ("gdi_palette_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_registry_hle() -> int:
    """T0: ADVAPI32 registry store, handles and enumeration."""
    cc = gcc()
    if not cc:
        print("registry: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "advapi32" / "registry"),
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "advapi32" / "registry" / "registry_hle.c"),
        str(ROOT / "tests" / "registry_hle_test.c"),
        "-pthread",
    ]
    for name, debug in (("registry_hle_test_debug.exe", 1),
                        ("registry_hle_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_string() -> int:
    """T0: MSVCR80 string/memory handlers over a fake guest arena."""
    cc = gcc()
    if not cc:
        print("msvcr80 string: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "string"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "string" / "string_hle.c"),
        str(ROOT / "tests" / "msvcr80_string_test.c"),
    ]
    for name, debug in (("msvcr80_string_test_debug.exe", 1),
                        ("msvcr80_string_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_rand() -> int:
    """T0: MSVCR80 srand/rand must match the MSVC LCG sequence."""
    cc = gcc()
    if not cc:
        print("msvcr80 rand: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "rand"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "rand" / "rand_hle.c"),
        str(ROOT / "tests" / "msvcr80_rand_test.c"),
    ]
    for name, debug in (("msvcr80_rand_test_debug.exe", 1),
                        ("msvcr80_rand_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_math() -> int:
    """T0: MSVCR80 math ABIs (x87 stack, cdecl doubles, SSE2)."""
    cc = gcc()
    if not cc:
        print("msvcr80 math: unavailable (no host C compiler)")
        return 2
    archive = ROOT / "third_party" / "softfloat" / "build-host" / "softfloat.a"
    if not archive.is_file():
        print("msvcr80 math: unavailable (host SoftFloat archive missing)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "math"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    sources = [
        ROOT / "runtime" / "hle" / "msvcr80" / "math" / "math_hle.c",
        ROOT / "tests" / "msvcr80_math_test.c",
    ]
    flags = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *include]
    for name, debug in (("msvcr80_math_test_debug.exe", 1),
                        ("msvcr80_math_test_release.exe", 0)):
        objdir = BUILD / f"{name}.objects"
        objdir.mkdir(parents=True, exist_ok=True)
        objects: list[str] = []
        for source in sources:
            obj = objdir / f"{source.stem}.o"
            rc = run([*flags, f"-DWC3_DEBUG={debug}", "-c",
                      str(source), "-o", str(obj)])
            if rc != 0:
                return rc
            objects.append(str(obj))
        exe = BUILD / name
        rc = run([cc, *objects, str(archive), "-lm", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_convert() -> int:
    """T0: MSVCR80 numeric conversion and CP1251 predicates."""
    cc = gcc()
    if not cc:
        print("msvcr80 convert: unavailable (no host C compiler)")
        return 2
    archive = ROOT / "third_party" / "softfloat" / "build-host" / "softfloat.a"
    if not archive.is_file():
        print("msvcr80 convert: unavailable (host SoftFloat archive missing)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "convert"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    sources = [
        ROOT / "runtime" / "hle" / "msvcr80" / "convert" / "convert_hle.c",
        ROOT / "tests" / "msvcr80_convert_test.c",
    ]
    flags = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *include]
    for name, debug in (("msvcr80_convert_test_debug.exe", 1),
                        ("msvcr80_convert_test_release.exe", 0)):
        objdir = BUILD / f"{name}.objects"
        objdir.mkdir(parents=True, exist_ok=True)
        objects: list[str] = []
        for source in sources:
            obj = objdir / f"{source.stem}.o"
            rc = run([*flags, f"-DWC3_DEBUG={debug}", "-c",
                      str(source), "-o", str(obj)])
            if rc != 0:
                return rc
            objects.append(str(obj))
        exe = BUILD / name
        rc = run([cc, *objects, str(archive), "-lm", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_crt() -> int:
    """T0: MSVCR80 CRT va_list copy and fatal runtime helpers."""
    cc = gcc()
    if not cc:
        print("msvcr80 crt: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "crt"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "crt" / "crt_hle.c"),
        str(ROOT / "tests" / "msvcr80_crt_test.c"),
    ]
    for name, debug in (("msvcr80_crt_test_debug.exe", 1),
                        ("msvcr80_crt_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}",
                  f"-DWC3_DEBUG_HLE={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_file() -> int:
    """T0: MSVCR80 low-level file descriptors over Wc3FileApi."""
    cc = gcc()
    if not cc:
        print("msvcr80 file: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "file"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "file" / "file_hle.c"),
        str(ROOT / "tests" / "msvcr80_file_test.c"),
        "-pthread",
    ]
    for name, debug in (("msvcr80_file_test_debug.exe", 1),
                        ("msvcr80_file_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_stdio() -> int:
    """T0: MSVCR80 stdio streams over Wc3FileApi and shared descriptors."""
    cc = gcc()
    if not cc:
        print("msvcr80 stdio: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "diag"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "file"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "stdio"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "file" / "file_hle.c"),
        str(ROOT / "runtime" / "hle" / "msvcr80" / "stdio" / "stdio_hle.c"),
        str(ROOT / "tests" / "msvcr80_stdio_test.c"),
        "-pthread",
    ]
    for name, debug in (("msvcr80_stdio_test_debug.exe", 1),
                        ("msvcr80_stdio_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_sort() -> int:
    """T0: MSVCR80 qsort over a fake guest dispatcher."""
    cc = gcc()
    if not cc:
        print("msvcr80 sort: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "sort"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "sort" / "sort_hle.c"),
        str(ROOT / "tests" / "msvcr80_sort_test.c"),
    ]
    for name, debug in (("msvcr80_sort_test_debug.exe", 1),
                        ("msvcr80_sort_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_secure() -> int:
    """T0: MSVCR80 strcpy_s over a fake guest range."""
    cc = gcc()
    if not cc:
        print("msvcr80 secure: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "secure"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "secure" / "secure_hle.c"),
        str(ROOT / "tests" / "msvcr80_secure_test.c"),
    ]
    for name, debug in (("msvcr80_secure_test_debug.exe", 1),
                        ("msvcr80_secure_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_error() -> int:
    """T0: MSVCR80 strerror messages and per-thread buffer reuse."""
    cc = gcc()
    if not cc:
        print("msvcr80 error: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "error"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "error" / "strerror_hle.c"),
        str(ROOT / "tests" / "msvcr80_error_test.c"),
    ]
    for name, debug in (("msvcr80_error_test_debug.exe", 1),
                        ("msvcr80_error_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_time() -> int:
    """T0: MSVCR80 _time64/_ctime64 over fake platform and shim."""
    cc = gcc()
    if not cc:
        print("msvcr80 time: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "time"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "time" / "crt_time_hle.c"),
        str(ROOT / "tests" / "msvcr80_time_test.c"),
    ]
    for name, debug in (("msvcr80_time_test_debug.exe", 1),
                        ("msvcr80_time_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_msvcr80_heap() -> int:
    """T0: MSVCR80 malloc/free/realloc/calloc over the guest heap shim."""
    cc = gcc()
    if not cc:
        print("msvcr80 heap: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "msvcr80" / "heap"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "msvcr80" / "heap" / "heap_hle.c"),
        str(ROOT / "tests" / "msvcr80_heap_test.c"),
    ]
    for name, debug in (("msvcr80_heap_test_debug.exe", 1),
                        ("msvcr80_heap_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_legacy_shim() -> int:
    """T0: real H6.3 shim against a fake registry and low guest mapping."""
    cc = gcc()
    if not cc:
        print("legacy shim: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "abi"),
        "-I" + str(ROOT / "runtime" / "legacy"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        *include,
        str(ROOT / "runtime" / "hle" / "legacy_shim.c"),
        str(ROOT / "runtime" / "hle" / "abi" / "hle_abi.c"),
        str(ROOT / "runtime" / "guest" / "guest_memory.c"),
        str(ROOT / "tests" / "legacy_shim_test.c"),
    ]
    for name, debug in (("legacy_shim_test_debug.exe", 1),
                        ("legacy_shim_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_mailbox_set() -> int:
    """P0-8: four concurrent producers, one consumer, zero loss, FIFO."""
    cc = gcc()
    if not cc:
        print("mailbox_set: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "mailbox_set_test.exe"
    rc = run([
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
        "-I" + str(ROOT / "runtime" / "ipc"),
        str(ROOT / "tests" / "mailbox_set_test.c"),
        str(ROOT / "runtime" / "ipc" / "mailbox.c"),
        str(ROOT / "runtime" / "ipc" / "mailbox_set.c"),
        "-o", str(exe),
    ])
    if rc != 0:
        return rc
    return run([str(exe)])


def test_hle_time() -> int:
    """T0: kernel32/time handlers against fake platform and shim."""
    cc = gcc()
    if not cc:
        print("hle time: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    include = [
        "-I" + str(ROOT / "runtime" / "cpu"),
        "-I" + str(ROOT / "runtime" / "guest"),
        "-I" + str(ROOT / "runtime" / "hle"),
        "-I" + str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "time"),
        "-I" + str(ROOT / "runtime" / "platform"),
        "-I" + str(ROOT / "third_party" / "softfloat" / "source" / "include"),
    ]
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        *include,
        str(ROOT / "runtime" / "hle" / "win32" / "kernel32" / "time" / "time_hle.c"),
        str(ROOT / "tests" / "hle_time_test.c"),
    ]
    for name, debug in (("hle_time_test_debug.exe", 1),
                        ("hle_time_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


def test_overlay() -> int:
    """T0: cached overlay compositor has no software-rasteriser fallback."""
    cc = gcc()
    if not cc:
        print("overlay: unavailable (no host C compiler)")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [
        cc,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(ROOT / "runtime" / "platform" / "android" / "controls"),
        str(ROOT / "runtime" / "platform" / "android" / "controls" / "overlay.c"),
        str(ROOT / "tests" / "overlay_test.c"),
    ]
    for name, debug in (("overlay_test_debug.exe", 1),
                        ("overlay_test_release.exe", 0)):
        exe = BUILD / name
        rc = run([*common, f"-DWC3_DEBUG={debug}", "-o", str(exe)])
        if rc != 0:
            return rc
        rc = run([str(exe)])
        if rc != 0:
            return rc
    return 0


TESTS = {
    "cpu_helpers": test_cpu_helpers,
    "hle_abi": test_hle_abi,
    "direct_call": test_direct_call,
    "direct_call_multi_tu": test_direct_call_multi_tu,
    "devirtualize_pass": test_devirtualize_pass,
    "dead_flags_pass": test_dead_flags_pass,
    "return_address_elide_pass": test_return_address_elide_pass,
    "flag_read_audit": test_flag_read_audit,
    "corpus_flag_policy": test_corpus_flag_policy,
    "pass6_build_guards": test_pass6_build_guards,
    "pass7_build_guards": test_pass7_build_guards,
    "icf_audit": test_icf_audit,
    "plt_audit": test_plt_audit,
    "wait_clock_audit": test_wait_clock_audit,
    "gva_fold": test_gva_fold,
    "lift_equivalence_checker": test_lift_equivalence_checker,
    "module_registry": test_module_registry,
    "corpus_binding": test_corpus_binding,
    "mailbox_set": test_mailbox_set,
    "hle_time": test_hle_time,
    "hle_process": test_hle_process,
    "user32_window": test_user32_window,
    "window_facts": test_window_facts,
    "user_object": test_user_object,
    "user32_display": test_user32_display,
    "hle_system": test_hle_system,
    "hle_memory": test_hle_memory,
    "hle_string": test_hle_string,
    "hle_resource": test_hle_resource,
    "comctl32_imagelist": test_comctl32_imagelist,
    "gdi_dc": test_gdi_dc,
    "gdi_objects": test_gdi_objects,
    "gdi_device": test_gdi_device,
    "gdi_palette": test_gdi_palette,
    "dragdrop": test_dragdrop,
    "scroll_hle": test_scroll_hle,
    "user32_menu": test_user32_menu,
    "registry_hle": test_registry_hle,
    "msvcr80_heap": test_msvcr80_heap,
    "msvcr80_math": test_msvcr80_math,
    "msvcr80_file": test_msvcr80_file,
    "msvcr80_stdio": test_msvcr80_stdio,
    "msvcr80_convert": test_msvcr80_convert,
    "msvcr80_crt": test_msvcr80_crt,
    "msvcr80_secure": test_msvcr80_secure,
    "msvcr80_error": test_msvcr80_error,
    "msvcr80_sort": test_msvcr80_sort,
    "msvcr80_time": test_msvcr80_time,
    "msvcr80_string": test_msvcr80_string,
    "msvcr80_rand": test_msvcr80_rand,
    "legacy_shim": test_legacy_shim,
    "pass4_build": test_pass4_build,
    "overlay": test_overlay,
    "file_layout": test_file_layout,
    "docs": test_docs,
    "upstream_pin": test_upstream_pin,
    "layout": test_layout,
    "config_schema": test_config_schema,
    "corpus_identity": test_corpus_identity,
    "profile_corpus": test_profile_corpus,
    "boot_dispatch_hash": test_boot_dispatch_hash,
    "boot_dispatch_compile": test_boot_dispatch_compile,
    "softfloat_manifest": test_softfloat_manifest,
    "x87_publication_guard": test_x87_publication_guard,
    "splitcorpus_packing": test_splitcorpus_packing,
    "splitcorpus_order": test_splitcorpus_order,
    "splitcorpus_source_order": test_splitcorpus_source_order,
    "dxvk_probe_cache": test_dxvk_probe_cache,
    "kernel32_file": test_kernel32_file,
    "atomic_lift": test_atomic_lift,
    "posix_shim": test_posix_shim,
    "dispatch": test_dispatch,
    "imports": test_imports,
    "thread_audit": test_thread_audit,
    "guest_runner": test_guest_runner,
    "asset_api": test_asset_api,
    "asset_snapshot": test_asset_snapshot,
    "platform_contract": test_platform_contract,
    "platform_headless": test_platform_headless,
    "guest_memory": test_guest_memory,
    "window_settle": test_window_settle,
    "waveout_install": test_waveout_install,
    "waveout_runtime": test_waveout_runtime,
    "waveout_headless": test_waveout_headless,
    "release_hygiene": test_release_hygiene,
    "null_gfx": test_null_gfx,
    "difftest": test_difftest,
}

T0_TESTS = list(TESTS)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", action="append", choices=sorted(TESTS))
    parser.add_argument("--unit", action="store_true",
                        help="run all asset-free T0 gates")
    parser.add_argument("--t1", action="store_true",
                        help="run the Windows oracle difftest gate")
    parser.add_argument("--t2", action="store_true",
                        help="run the Android device end-to-end gate")
    parser.add_argument("--e2e", action="store_true",
                        help="alias for --t2")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)
    if args.list:
        for name in TESTS:
            print(name)
        print("t1")
        print("t2")
        return 0

    if not _CCACHE:
        print("ccache: unavailable; host tests run without a compiler cache",
              file=sys.stderr)

    modes = [args.unit, args.t1, args.t2, args.e2e]
    if sum(bool(x) for x in modes) > 1:
        parser.error("choose one of --unit, --t1, --t2, --e2e")
    if args.test and any(modes):
        parser.error("--test cannot be combined with a gate mode")

    if args.t1:
        names = ["t1"]
    elif args.t2 or args.e2e:
        names = ["t2"]
    elif args.unit:
        names = T0_TESTS
    else:
        names = args.test or list(TESTS)

    functions = dict(TESTS)
    functions["t1"] = test_t1
    functions["t2"] = test_t2
    strict = bool(args.test or any(modes))

    unavailable: list[str] = []
    failed: list[str] = []
    for name in names:
        print(f"=== {name} ===")
        rc = functions[name]()
        if rc == 2:
            unavailable.append(name)
        elif rc != 0:
            failed.append(name)
    if failed:
        print("FAILED: " + ", ".join(failed), file=sys.stderr)
        return 1
    if unavailable:
        print("UNAVAILABLE (not pass): " + ", ".join(unavailable), file=sys.stderr)
        if strict:
            return 2
        return 0
    print("ALL AVAILABLE TESTS PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
