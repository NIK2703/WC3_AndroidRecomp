#!/usr/bin/env python3
"""Android end-to-end (T2) gate.

This is deliberately strict about the difference between "the APK launched" and
"the guest reached the menu/map".  A pass requires every `--marker` supplied by
the caller to appear in logcat.  With no marker the result is `unavailable`, not
a pass, because Step 0 has not fixed the boot/menu/map marker set yet.

No device, no adb or no APK is reported as unavailable (exit 2), never as a
successful test.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = "com.wc3.recomp"
DEFAULT_ACTIVITY = "com.wc3.recomp.Wc3Activity"
APK_CANDIDATES = (
    ROOT / "build" / "android" / "wc3gameactivity.apk",
    ROOT / "build" / "android" / "app-debug.apk",
    ROOT / "apps" / "android_gameactivity" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk",
    ROOT / "apps" / "android_gameactivity" / "build" / "outputs" / "apk" / "release" / "app-release.apk",
)


def run(cmd: list[str], timeout: int = 120) -> subprocess.CompletedProcess:
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          encoding="utf-8", errors="replace")


def find_adb(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).exists() else shutil.which(explicit)
    for env in ("ADB", "ANDROID_ADB"):
        value = os.environ.get(env)
        if value and Path(value).exists():
            return value
    found = shutil.which("adb")
    if found:
        return found
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_SDK"):
        root = os.environ.get(env)
        if not root:
            continue
        for name in ("adb.exe", "adb"):
            candidate = Path(root) / "platform-tools" / name
            if candidate.exists():
                return str(candidate)
    return None


def connected_devices(adb: str) -> list[str]:
    proc = run([adb, "devices"])
    if proc.returncode != 0:
        return []
    devices = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def find_apk(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.is_file() and path.stat().st_size > 0 else None
    for path in APK_CANDIDATES:
        if path.is_file() and path.stat().st_size > 0:
            return path
    return None


def logcat(adb: str, serial: str, clear: bool = False, dump: bool = True) -> str:
    cmd = [adb, "-s", serial, "logcat"]
    if clear:
        run([adb, "-s", serial, "logcat", "-c"])
        return ""
    if not dump:
        return ""
    proc = run([adb, "-s", serial, "logcat", "-d", "-v", "brief"], timeout=120)
    return proc.stdout if proc.returncode == 0 else ""


def save_log(text: str, tag: str) -> Path:
    out = ROOT / "reports" / "generated"
    out.mkdir(parents=True, exist_ok=True)
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    path = out / f"device_test_{tag}_{stamp}.log"
    path.write_text(text, encoding="utf-8", errors="replace")
    print(f"T2: logcat saved to {path.relative_to(ROOT)}")
    return path


def missing_markers(text: str, markers: list[str]) -> list[str]:
    return [marker for marker in markers if marker not in text]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default="1.27b")
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    parser.add_argument("--apk")
    parser.add_argument("--package", default=DEFAULT_PACKAGE)
    parser.add_argument("--activity", default=DEFAULT_ACTIVITY)
    parser.add_argument("--marker", action="append", default=[],
                        help="required logcat substring; repeat for each marker")
    parser.add_argument("--timeout", type=int, default=180,
                        help="seconds to wait for all markers")
    parser.add_argument("--poll", type=int, default=2,
                        help="logcat poll interval in seconds")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--require-device", action="store_true",
                        help="accepted for CI parity; no device is still unavailable")
    args = parser.parse_args(argv)
    if args.corpus != "1.27b":
        print(f"T2: error: unknown corpus {args.corpus}", file=sys.stderr)
        return 1

    adb = find_adb(args.adb)
    if not adb:
        print("T2: unavailable (adb not found; set ANDROID_HOME or --adb)")
        return 2

    devices = connected_devices(adb)
    if args.list_devices:
        print("T2: devices: " + (", ".join(devices) if devices else "none"))
        return 0 if devices else 2
    if not devices:
        print("T2: unavailable (adb sees no device)")
        return 2
    if args.serial and args.serial not in devices:
        print(f"T2: error: device {args.serial} is not connected", file=sys.stderr)
        return 1
    serial = args.serial or devices[0]

    apk = find_apk(args.apk)
    if apk is None:
        print("T2: unavailable (no non-empty APK; build it first)")
        return 2

    if not args.marker:
        print("T2: unavailable (no --marker: boot/menu/map markers are not fixed yet)")
        return 2

    proc = run([adb, "-s", serial, "install", "-r", "-t", "-d", str(apk)],
               timeout=300)
    if proc.returncode != 0:
        print("T2: error: adb install failed", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        return 1

    logcat(adb, serial, clear=True)
    launch = f"{args.package}/{args.activity}"
    proc = run([adb, "-s", serial, "shell", "am", "force-stop", args.package])
    proc = run([adb, "-s", serial, "shell", "am", "start", "-W", "-S", "-n",
                launch], timeout=120)
    if proc.returncode != 0:
        print("T2: error: activity launch failed", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        return 1

    deadline = time.monotonic() + max(1, args.timeout)
    text = ""
    while True:
        text = logcat(adb, serial)
        missing = missing_markers(text, args.marker)
        if not missing:
            print(f"T2: PASS ({len(args.marker)} marker(s) on {serial})")
            return 0
        if time.monotonic() >= deadline:
            save_log(text, "timeout")
            print("T2: FAIL: missing logcat markers: " + ", ".join(missing),
                  file=sys.stderr)
            return 1
        time.sleep(max(1, args.poll))


if __name__ == "__main__":
    raise SystemExit(main())
