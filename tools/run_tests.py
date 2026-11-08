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


def test_file_layout() -> int:
    """R1-R14 from docs/FILE_LAYOUT.md; warnings are known M2-M6 debt."""
    return run([sys.executable, str(ROOT / "tools" / "check_file_layout.py"),
                "--strict"])


def test_docs() -> int:
    """R11: index/link/policy checks; full path inventory arrives with M6."""
    return run([sys.executable, str(ROOT / "tools" / "check_docs.py")])


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
    "overlay": test_overlay,
    "file_layout": test_file_layout,
    "docs": test_docs,
    "upstream_pin": test_upstream_pin,
    "layout": test_layout,
    "config_schema": test_config_schema,
    "corpus_identity": test_corpus_identity,
    "atomic_lift": test_atomic_lift,
    "posix_shim": test_posix_shim,
    "dispatch": test_dispatch,
    "imports": test_imports,
    "thread_audit": test_thread_audit,
    "guest_runner": test_guest_runner,
    "asset_api": test_asset_api,
    "platform_contract": test_platform_contract,
    "platform_headless": test_platform_headless,
    "guest_memory": test_guest_memory,
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
