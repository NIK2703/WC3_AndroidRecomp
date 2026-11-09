#!/usr/bin/env python3
"""Source-only audit of guest wait clocks (O-66).

``runtime/legacy/hle_android.c`` is read-only under R15/BP-3.3.  Its
``h_wait_many()`` loop polls with ``usleep(1000)`` and ``h_wait_mutex()``
builds its deadline from ``CLOCK_REALTIME``.  The Android platform boundary
already uses monotonic clocks for its own waits.  This tool records that split
so the M5 cutover cannot silently keep the wall-clock wait or replace the
completion -> timer -> completion delivery order with a plain condvar.

The audit is report-only unless ``--strict`` is supplied.  Strict mode checks
that the expected shape is still present; a change means the read-only source
or the platform boundary moved and the O-66 decision needs a fresh review.

Usage:
  python tools/wait_clock_audit.py
  python tools/wait_clock_audit.py --strict --json reports/generated/wait_clocks.json
  python tools/wait_clock_audit.py --self-test
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
LEGACY = ROOT / "runtime" / "legacy" / "hle_android.c"
PLATFORM = ROOT / "runtime" / "platform" / "android" / "platform_android.c"


def find_function(lines: list[str], signature: str) -> tuple[int, int] | None:
    """Return the inclusive line span of a C function body."""
    start = next((i for i, line in enumerate(lines) if signature in line), None)
    if start is None:
        return None

    depth = 0
    seen_open = False
    in_block_comment = False
    in_line_comment = False
    in_string: str | None = None
    escaped = False
    # #if/#else branches can each open the same brace and close it after
    # #endif.  Track one valid branch at a time by restoring the depth at
    # #else/#elif; the last branch is the one we keep at #endif.
    conditional_depth: list[int] = []

    for i in range(start, len(lines)):
        line = lines[i]
        stripped = line.lstrip()
        if not in_block_comment and not in_line_comment and stripped.startswith("#"):
            directive = stripped[1:].lstrip()
            word = re.match(r"[A-Za-z]+", directive)
            name = word.group(0) if word else ""
            if name in ("if", "ifdef", "ifndef"):
                conditional_depth.append(depth)
            elif name in ("else", "elif") and conditional_depth:
                depth = conditional_depth[-1]
            elif name == "endif" and conditional_depth:
                conditional_depth.pop()
            continue

        j = 0
        while j < len(line):
            c = line[j]
            nxt = line[j + 1] if j + 1 < len(line) else ""
            if in_block_comment:
                if c == "*" and nxt == "/":
                    in_block_comment = False
                    j += 2
                    continue
                j += 1
                continue
            if in_line_comment:
                break
            if in_string is not None:
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == in_string:
                    in_string = None
                j += 1
                continue
            if c == "/" and nxt == "*":
                in_block_comment = True
                j += 2
                continue
            if c == "/" and nxt == "/":
                in_line_comment = True
                break
            if c in ('"', "'"):
                in_string = c
                j += 1
                continue
            if c == "{":
                depth += 1
                seen_open = True
            elif c == "}":
                depth -= 1
                if seen_open and depth == 0:
                    return start, i
                if depth < 0:
                    return None
            j += 1
        in_line_comment = False
    return None


def span_text(lines: list[str], span: tuple[int, int] | None) -> str:
    if span is None:
        return ""
    return "\n".join(lines[span[0]:span[1] + 1])


def first_line(lines: list[str], span: tuple[int, int] | None,
               needle: str) -> int | None:
    if span is None:
        return None
    for i in range(span[0], span[1] + 1):
        if needle in lines[i]:
            return i + 1
    return None


def usleep_us(text: str) -> int | None:
    match = re.search(r"\busleep\((\d+)\)", text)
    return int(match.group(1)) if match else None


def audit() -> dict[str, object]:
    if not LEGACY.is_file() or not PLATFORM.is_file():
        raise FileNotFoundError("wait-clock sources are missing")
    legacy_lines = LEGACY.read_text(encoding="utf-8", errors="replace").splitlines()
    platform_lines = PLATFORM.read_text(
        encoding="utf-8", errors="replace").splitlines()

    wait_mutex = find_function(legacy_lines, "static void h_wait_mutex(")
    wait_many = find_function(legacy_lines, "static void h_wait_many(")
    poll_pause = find_function(platform_lines, "static void android_poll_pause(")
    cond_init = find_function(platform_lines, "static int android_sync_cond_init(")

    wait_mutex_text = span_text(legacy_lines, wait_mutex)
    wait_many_text = span_text(legacy_lines, wait_many)
    poll_pause_text = span_text(platform_lines, poll_pause)
    cond_init_text = span_text(platform_lines, cond_init)

    legacy = {
        "h_wait_many": {
            "line": wait_many[0] + 1 if wait_many else None,
            "deadline_clock": ("monotonic"
                               if "wave_now_ms()" in wait_many_text else None),
            "poll_usleep_us": usleep_us(wait_many_text),
            "condvar": "pthread_cond_timedwait" in wait_many_text,
        },
        "h_wait_mutex": {
            "line": wait_mutex[0] + 1 if wait_mutex else None,
            "deadline_clock": ("realtime"
                               if "CLOCK_REALTIME" in wait_mutex_text else None),
            "condvar": "pthread_cond_timedwait" in wait_mutex_text,
            "realtime_line": first_line(legacy_lines, wait_mutex,
                                        "CLOCK_REALTIME"),
        },
    }
    platform = {
        "android_poll_pause": {
            "line": poll_pause[0] + 1 if poll_pause else None,
            "clock": ("monotonic"
                      if "clock_nanosleep(CLOCK_MONOTONIC" in poll_pause_text
                      else None),
        },
        "android_sync_cond_init": {
            "line": cond_init[0] + 1 if cond_init else None,
            "clock": ("monotonic"
                      if "pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)"
                      in cond_init_text else None),
        },
    }

    checks = {
        "h_wait_many_present": wait_many is not None,
        "h_wait_many_polls_1ms": legacy["h_wait_many"]["poll_usleep_us"] == 1000,
        "h_wait_many_uses_monotonic":
            legacy["h_wait_many"]["deadline_clock"] == "monotonic",
        "h_wait_mutex_present": wait_mutex is not None,
        "h_wait_mutex_uses_realtime":
            legacy["h_wait_mutex"]["deadline_clock"] == "realtime",
        "platform_poll_uses_monotonic":
            platform["android_poll_pause"]["clock"] == "monotonic",
        "platform_sync_cond_uses_monotonic":
            platform["android_sync_cond_init"]["clock"] == "monotonic",
    }
    return {
        "schema": "wc3.wait_clock_audit.v1",
        "finding": "O-66",
        "status": "blocked-read-only",
        "reason": (
            "h_wait_many()/h_wait_mutex() live in runtime/legacy/, which is "
            "read-only under R15/BP-3.3.  Changing the poll to a monotonic "
            "condvar would also change the completion -> timer -> completion "
            "delivery order; A-12.4 rejected a separate guest thread for the "
            "same reason."
        ),
        "legacy": legacy,
        "platform": platform,
        "checks": checks,
        "action": (
            "Keep the polling shape until M5.  At cutover preserve the "
            "completion/timer order, use a monotonic deadline, measure "
            "wakeups/second and idle CPU (PM-12/T2), and record an ADR for E3."
        ),
    }


def print_report(report: dict[str, object]) -> None:
    legacy = report["legacy"]
    platform = report["platform"]
    print(f"O-66 wait clocks: {report['status']}")
    many = legacy["h_wait_many"]  # type: ignore[index]
    mutex = legacy["h_wait_mutex"]  # type: ignore[index]
    print(f"  legacy h_wait_many  line {many['line']}: "
          f"poll={many['poll_usleep_us']} us, "
          f"deadline={many['deadline_clock']}, condvar={many['condvar']}")
    print(f"  legacy h_wait_mutex line {mutex['line']}: "
          f"deadline={mutex['deadline_clock']}, condvar={mutex['condvar']}")
    for name, value in platform.items():  # type: ignore[union-attr]
        print(f"  platform {name}: line {value['line']}, clock={value['clock']}")
    print(f"  action: {report['action']}")


def run_self_test() -> int:
    sample = [
        "static void f(int x) {",
        "    if (x) { /* a } in a comment */ }",
        "    const char *s = \"}\";",
        "    usleep(1000);",
        "}",
        "static void g(void) {",
        "    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts);",
        "}",
    ]
    span = find_function(sample, "static void f(")
    if span != (0, 4):
        print("wait clock self-test: function span failed")
        return 1
    if usleep_us(span_text(sample, span)) != 1000:
        print("wait clock self-test: usleep parse failed")
        return 1
    if find_function(sample, "static void missing(") is not None:
        print("wait clock self-test: missing function accepted")
        return 1
    if find_function(sample, "static void g(") != (5, 7):
        print("wait clock self-test: second span failed")
        return 1

    conditional = [
        "static void h(void) {",
        "#ifdef WC3_DEBUG",
        "    if (x) {",
        "#else",
        "    if (x) {",
        "#endif",
        "        body();",
        "    }",
        "}",
    ]
    if find_function(conditional, "static void h(") != (0, 8):
        print("wait clock self-test: conditional span failed")
        return 1
    print("wait clock self-test: OK")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=Path,
                        help="write the full report as JSON")
    parser.add_argument("--strict", action="store_true",
                        help="fail if the audited wait shape changed")
    parser.add_argument("--self-test", action="store_true",
                        help="check the source parser without reading the tree")
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        return run_self_test()

    try:
        report = audit()
    except (OSError, FileNotFoundError) as exc:
        print(f"wait clock audit: unavailable ({exc})")
        return 2

    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"report: {args.json}")

    checks = report["checks"]  # type: ignore[assignment]
    failed = [name for name, ok in checks.items() if not ok]  # type: ignore[union-attr]
    if args.strict and failed:
        print("wait clock audit: shape changed: " + ", ".join(failed),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
