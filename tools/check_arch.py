#!/usr/bin/env python3
"""Static architecture gates for the Android-only target."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
HLE_ABI = RUNTIME / "hle" / "abi" / "hle_abi.h"
PLATFORM = RUNTIME / "platform" / "platform.h"

# Guest-visible locking lives in HLE objects; the CPU and ABI layers only
# emulate the guest's own lock-prefixed instructions.  `--concurrency` therefore
# checks zones and call graphs, not the spelling "mutex" in the runtime.
HOST_LOCK_RE = re.compile(
    r"\b(?:pthread_mutex_(?:lock|unlock|trylock|timedlock)|pthread_cond_wait|"
    r"std::(?:recursive_)?mutex|std::lock_guard|std::unique_lock|"
    r"EnterCriticalSection|LeaveCriticalSection|TryEnterCriticalSection|"
    r"WaitForSingleObject(?:Ex)?)\b"
)
SPINLOCK_RE = re.compile(
    r"\b(?:while|for)\s*\([^;{}]*(?:__sync_lock_test_and_set|"
    r"_InterlockedCompareExchange|atomic_flag_test_and_set)"
)
ATOMIC_RE = re.compile(r"\b(?:__atomic_|__sync_|_Interlocked|std::atomic)\w*")
DISPATCH_RE = re.compile(r"\b(?:dispatch|dispatch_jmp|hle_call)\s*\(")
# These zones must not contain host locks or spinlocks.  The CPU layer may use
# seq_cst atomics for the guest's lock-prefixed instructions; the mailbox is
# explicitly lock-free and uses acquire/release atomics.
GUEST_ZONES = (
    "runtime/cpu/",
    "runtime/guest/",
    "runtime/hle/abi/",
    "runtime/app/guest_runner.",
)
LOCK_FREE_ZONES = ("runtime/ipc/",)
HLE_PREFIX = "runtime/hle/"
PLATFORM_PREFIX = "runtime/platform/"

FORBIDDEN_NET = ("NetApi", "Wc3NetApi")
REQUIRED_APIS = (
    "Wc3MemApi",
    "Wc3FileApi",
    "Wc3TimeApi",
    "Wc3LocaleApi",
    "Wc3WindowApi",
    "Wc3InputApi",
    "Wc3AudioApi",
    "Wc3ClipboardApi",
    "Wc3SurfaceApi",
    "Wc3LogApi",
    "Wc3ConfigApi",
    "Wc3AssetApi",
    "Wc3ThreadApi",
    "Wc3SyncApi",
    "Wc3VulkanDriverApi",
)

def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def source_files() -> list[Path]:
    out = []
    for ext in ("*.c", "*.h", "*.cc", "*.cpp", "*.hpp"):
        out.extend(RUNTIME.rglob(ext))
    return sorted(p for p in out if "third_party" not in p.parts)


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def check_hle_abi() -> list[str]:
    errors: list[str] = []
    text = HLE_ABI.read_text(encoding="utf-8")
    if text.count("typedef struct HleEntry") != 1:
        errors.append("hle_abi.h: expected exactly one HleEntry definition")
    if "#if WC3_DEBUG" not in text or "HleDebugEntry" not in text:
        errors.append("hle_abi.h: debug names are not gated by #if WC3_DEBUG")
    if "hle_stub" in text:
        errors.append("hle_abi.h: hle_stub must not exist")
    abi_dir = HLE_ABI.parent
    for path in abi_dir.glob("*.[ch]"):
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in re.finditer(r"__atomic_|__sync_|pthread_mutex_|std::mutex", body):
            line = body[: m.start()].count("\n") + 1
            errors.append(f"{rel(path)}:{line}: lock/atomic in HLE ABI")
    call = (abi_dir / "hle_abi.c").read_text(encoding="utf-8")
    if "hle_entry_for" not in call or "stack_cleanup" not in call:
        errors.append("hle_abi.c: direct thunk lookup/cleanup missing")
    return errors


def _function_segments(body: str):
    """Yield rough top-level function segments for lock/dispatch checks."""
    depth = 0
    start = 0
    seg_start = None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                header = body[start:i]
                if "(" in header and not header.rstrip().endswith(";"):
                    seg_start = start
                else:
                    seg_start = None
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
                if depth == 0:
                    if seg_start is not None:
                        yield body[seg_start : i + 1]
                    start = i + 1
                    seg_start = None


def check_concurrency() -> list[str]:
    errors: list[str] = []
    for path in source_files():
        key = rel(path)
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        guest_zone = any(key.startswith(prefix) for prefix in GUEST_ZONES)
        lock_free_zone = any(key.startswith(prefix) for prefix in LOCK_FREE_ZONES)
        hle_zone = key.startswith(HLE_PREFIX) and not key.startswith(
            "runtime/hle/abi/"
        )
        platform_zone = key.startswith(PLATFORM_PREFIX)

        for m in HOST_LOCK_RE.finditer(body):
            if guest_zone or lock_free_zone:
                line = body[: m.start()].count("\n") + 1
                errors.append(
                    f"{key}:{line}: host lock in guest/lock-free zone: {m.group(0)}"
                )
            elif not hle_zone and not platform_zone:
                line = body[: m.start()].count("\n") + 1
                errors.append(f"{key}:{line}: unowned host lock: {m.group(0)}")

        for m in SPINLOCK_RE.finditer(body):
            line = body[: m.start()].count("\n") + 1
            errors.append(f"{key}:{line}: spinlock is forbidden")

        # The invariant is about the call graph, not the presence of a lock:
        # a per-object HLE lock may exist, but dispatch()/hle_call() must not
        # run while it is held.
        for segment in _function_segments(body):
            lock = HOST_LOCK_RE.search(segment) or SPINLOCK_RE.search(segment)
            if lock and DISPATCH_RE.search(segment):
                line = body[: body.find(segment)].count("\n") + 1
                errors.append(
                    f"{key}:{line}: dispatch/hle_call reachable with a host lock"
                )
    return errors


def check_platform_contract() -> list[str]:
    errors: list[str] = []
    text = PLATFORM.read_text(encoding="utf-8")
    body = strip_comments(text)
    for api in REQUIRED_APIS:
        if api not in body:
            errors.append(f"platform.h: missing {api}")
    for bad in FORBIDDEN_NET:
        if bad in body:
            errors.append(f"platform.h: forbidden {bad} (LAN is out of scope)")
    if "wc3_platform_install" not in body or "wc3_platform" not in body:
        errors.append("platform.h: missing platform installation contract")
    if "guest" not in text.lower():
        errors.append("platform.h: guest thread contract is not documented")
    return errors


def check_release() -> list[str]:
    errors: list[str] = []
    for path in source_files():
        body = path.read_text(encoding="utf-8", errors="replace")
        if "#ifdef WC3_DEBUG" in body:
            errors.append(f"{rel(path)}: use #if WC3_DEBUG, not #ifdef")
    abi = HLE_ABI.read_text(encoding="utf-8")
    if "#ifndef WC3_DEBUG" not in abi or "#define WC3_DEBUG 0" not in abi:
        errors.append("hle_abi.h: missing WC3_DEBUG default zero")
    return errors


def check_lifecycle() -> list[str]:
    errors: list[str] = []
    hdr = (RUNTIME / "app" / "guest_runner.h").read_text(encoding="utf-8")
    src = (RUNTIME / "app" / "guest_runner.c").read_text(encoding="utf-8")
    for state in (
        "CREATED",
        "RUNNING",
        "PAUSED",
        "SURFACE_LOST",
        "AUDIO_DEVICE_LOST",
        "STOPPING",
        "STOPPED",
    ):
        if f"WC3_RUNNER_{state}" not in hdr:
            errors.append(f"guest_runner.h: missing state {state}")
    for event in (
        "START",
        "PAUSE",
        "RESUME",
        "SURFACE_LOST",
        "SURFACE_RESTORED",
        "AUDIO_DEVICE_LOST",
        "AUDIO_DEVICE_RESTORED",
        "STOP",
        "FINISHED",
    ):
        if f"WC3_RUNNER_EV_{event}" not in src:
            errors.append(f"guest_runner.c: missing transition {event}")
    if "wc3_mailbox_push" not in src:
        errors.append("guest_runner.c: Android events are not routed through the mailbox")
    if "pthread_mutex" in src or "std::mutex" in src:
        errors.append("guest_runner.c: host lock added")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hle-abi", action="store_true")
    parser.add_argument("--concurrency", action="store_true")
    parser.add_argument("--platform-contract", action="store_true")
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--lifecycle", action="store_true")
    parser.add_argument("--renderer", action="store_true")
    parser.add_argument("--policy", action="store_true")
    args = parser.parse_args(argv)

    if not any(vars(args).values()):
        parser.error("choose at least one check")

    errors: list[str] = []
    if args.hle_abi or args.strict:
        errors += check_hle_abi()
    if args.concurrency or args.strict:
        errors += check_concurrency()
    if args.platform_contract or args.strict:
        errors += check_platform_contract()
    if args.release or args.strict:
        errors += check_release()
    if args.lifecycle or args.strict:
        errors += check_lifecycle()
    if args.renderer or args.policy:
        print("not implemented yet: renderer/policy check", file=sys.stderr)
        return 2

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print("architecture checks OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
