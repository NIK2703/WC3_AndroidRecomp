#!/usr/bin/env python3
"""Reproduce the PASS11 dynamic audio counters from a device log.

PASS11 O-105 established the first dynamic baseline for the audio path from
``build/device_run.txt``.  This tool keeps that measurement reproducible
instead of leaving the numbers in a prose table:

    python tools/audio_runtime_profile.py --check
    python tools/audio_runtime_profile.py --json build/audio_profile.json

The log is a historical debug capture, not an acceptance run.  The tool only
reports what the text contains; it does not claim a speedup or replace PM-8 /
PM-11 / PM-19 measurements.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG = ROOT / "build" / "device_run.txt"

AA_RE = re.compile(
    r"^\[wave\] aa cb=(?P<cb>\d+) short=(?P<short>\d+) "
    r"slow=(?P<slow>\d+) silent=(?P<silent>\d+) min=(?P<min>\d+) "
    r"maxgap_us=(?P<maxgap>\d+) gap5=(?P<gap5>\d+) gap20=(?P<gap20>\d+) "
    r"xruns=(?P<xruns>\d+) lockmax_us=(?P<lockmax>\d+)$"
)
ENERGY_RE = re.compile(
    r"^\[wave\] cb energy t=(?P<t>\d+) cb=(?P<cb>\d+) "
    r"want=(?P<want>\d+) take=(?P<take>\d+) ring=(?P<ring>\d+) "
    r"blocks=(?P<blocks>\d+)"
)
DONE_RE = re.compile(
    r"^\[wave\] DONE t=(?P<t>\d+) hdr=(?P<hdr>[0-9A-Fa-f]+) "
    r"cb=(?P<cb>[0-9A-Fa-f]+) gid=(?P<gid>-?\d+)"
)

# PASS11 section 3-5 values from the historical log.  Keep these in one place
# so a fresh capture cannot silently inherit the old table.
PASS11_EXPECTED = {
    "callbacks_total": 31583,
    "callbacks_per_s": 105.24,
    "energy_reports": 149,
    "want_bytes_min": 96,
    "want_bytes_mean": 1591,
    "want_bytes_max": 3528,
    "short": 105,
    "slow": 0,
    "xruns": 0,
    "lockmax_us": 424,
    "maxgap_us": 231531,
    "wom_done_count": 6004,
    "wom_done_per_s": 19.89,
    "wom_done_interval_ms_median": 50,
    "wom_done_interval_ms_p99": 61,
    "wom_done_interval_ms_max": 1058,
    "waveout_write_lines": 6004,
    "callbacks_per_wom_done": 5.26,
    "ring_samples": 149,
    "ring_min": 0,
    "ring_mean": 34181,
    "ring_max": 43800,
    "service_busy_lines": 9,
    "cb_underrun_lines": 2,
}


def _ratio(numerator: int, denominator: int) -> float:
    if denominator == 0:
        return 0.0
    return numerator / denominator


def _p99(values: list[int]) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, (len(ordered) * 99 + 99) // 100 - 1)
    return ordered[index]


def parse_lines(lines: list[str]) -> dict:
    energy: list[dict] = []
    done: list[dict] = []
    aa: dict | None = None
    writes = 0
    busy = 0
    underruns = 0

    for line in lines:
        line = line.rstrip("\r\n")
        match = ENERGY_RE.match(line)
        if match:
            energy.append({key: int(value) for key, value in
                           match.groupdict().items()})
            continue
        match = DONE_RE.match(line)
        if match:
            done.append({key: int(value) if key != "hdr" and key != "cb"
                         else value for key, value in match.groupdict().items()})
            continue
        match = AA_RE.match(line)
        if match:
            aa = {key: int(value) for key, value in match.groupdict().items()}
            continue
        if line.startswith("[wave] Write "):
            writes += 1
        elif line.startswith("[wave] service busy"):
            busy += 1
        elif line.startswith("[wave] cb underrun"):
            underruns += 1

    if not energy:
        raise SystemExit("audio profile: no [wave] cb energy lines")
    if not done:
        raise SystemExit("audio profile: no [wave] DONE lines")

    # [wave] cb energy t= is microseconds; [wave] DONE t= is milliseconds.
    energy_span_s = (energy[-1]["t"] - energy[0]["t"]) / 1_000_000.0
    done_span_s = (done[-1]["t"] - done[0]["t"]) / 1000.0
    intervals = [b["t"] - a["t"] for a, b in zip(done, done[1:])]
    wants = [row["want"] for row in energy]
    rings = [row["ring"] for row in energy]
    blocks = Counter(row["blocks"] for row in energy)
    callbacks_total = aa["cb"] if aa else energy[-1]["cb"]
    callbacks_in_span = energy[-1]["cb"]
    callbacks_per_s = round(_ratio(callbacks_in_span, energy_span_s), 2)
    wom_per_s = round(_ratio(len(done), done_span_s), 2)
    ring_max = max(rings)

    return {
        "schema": 1,
        "kind": "audio_runtime_profile",
        "source_log": {
            "path": str(DEFAULT_LOG.relative_to(ROOT)).replace("\\", "/"),
            "sha256": None,
            "historical": True,
        },
        "measured": {
            "aaudio": {
                "callbacks_total": callbacks_total,
                "callbacks_measured_span_s": round(energy_span_s, 3),
                "callbacks_per_s": callbacks_per_s,
                "energy_reports": len(energy),
                "want_bytes_min": min(wants),
                "want_bytes_mean": int(round(statistics.fmean(wants))),
                "want_bytes_max": max(wants),
                "short": aa["short"] if aa else 0,
                "slow": aa["slow"] if aa else 0,
                "silent": aa["silent"] if aa else 0,
                "xruns": aa["xruns"] if aa else 0,
                "lockmax_us": aa["lockmax"] if aa else 0,
                "maxgap_us": aa["maxgap"] if aa else 0,
            },
            "wom_done": {
                "count": len(done),
                "span_s": round(done_span_s, 3),
                "per_s": wom_per_s,
                "interval_ms_median": int(statistics.median(intervals))
                if intervals else 0,
                "interval_ms_p99": _p99(intervals),
                "interval_ms_max": max(intervals) if intervals else 0,
                "unique_hdr": len({row["hdr"] for row in done}),
                "unique_gid": len({row["gid"] for row in done}),
            },
            "waveOutWrite_log_lines": writes,
            "callbacks_per_wom_done": round(
                _ratio(callbacks_total, len(done)), 2),
            "ring_occupancy_bytes": {
                "samples": len(rings),
                "min": min(rings),
                "mean": int(round(statistics.fmean(rings))),
                "max": ring_max,
                "ratio_8MiB_to_peak": round(
                    _ratio(8 * 1024 * 1024, ring_max), 1),
                "ratio_64KiB_to_peak": round(
                    _ratio(64 * 1024, ring_max), 1),
            },
            "blocks_in_flight_distribution": {
                str(key): blocks.get(key, 0) for key in range(6)
            },
            "service_busy_log_lines": busy,
            "cb_underrun_log_lines": underruns,
            "guest_header_bytes_A13_1": 8832,
        },
    }


def load_profile(path: Path) -> dict:
    raw = path.read_bytes()
    profile = parse_lines(raw.decode("utf-8", errors="replace").splitlines())
    try:
        display = str(path.resolve().relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        display = str(path).replace("\\", "/")
    profile["source_log"]["path"] = display
    profile["source_log"]["sha256"] = hashlib.sha256(raw).hexdigest()
    return profile


def _expected_view(profile: dict) -> dict:
    measured = profile["measured"]
    aaudio = measured["aaudio"]
    wom = measured["wom_done"]
    ring = measured["ring_occupancy_bytes"]
    return {
        "callbacks_total": aaudio["callbacks_total"],
        "callbacks_per_s": aaudio["callbacks_per_s"],
        "energy_reports": aaudio["energy_reports"],
        "want_bytes_min": aaudio["want_bytes_min"],
        "want_bytes_mean": aaudio["want_bytes_mean"],
        "want_bytes_max": aaudio["want_bytes_max"],
        "short": aaudio["short"],
        "slow": aaudio["slow"],
        "xruns": aaudio["xruns"],
        "lockmax_us": aaudio["lockmax_us"],
        "maxgap_us": aaudio["maxgap_us"],
        "wom_done_count": wom["count"],
        "wom_done_per_s": wom["per_s"],
        "wom_done_interval_ms_median": wom["interval_ms_median"],
        "wom_done_interval_ms_p99": wom["interval_ms_p99"],
        "wom_done_interval_ms_max": wom["interval_ms_max"],
        "waveout_write_lines": measured["waveOutWrite_log_lines"],
        "callbacks_per_wom_done": measured["callbacks_per_wom_done"],
        "ring_samples": ring["samples"],
        "ring_min": ring["min"],
        "ring_mean": ring["mean"],
        "ring_max": ring["max"],
        "service_busy_lines": measured["service_busy_log_lines"],
        "cb_underrun_lines": measured["cb_underrun_log_lines"],
    }


def check_against_pass11(profile: dict) -> list[str]:
    actual = _expected_view(profile)
    problems = []
    for key, expected in PASS11_EXPECTED.items():
        if actual.get(key) != expected:
            problems.append(f"{key}: expected {expected}, got {actual.get(key)}")
    return problems


def self_test() -> int:
    lines = [
        "[wave] cb energy t=1000 cb=1 want=100 take=100 ring=0 blocks=0 fake=0",
        "[wave] DONE t=1000 hdr=61F133A0 cb=351130B0 gid=0 flg=00000003",
        "[wave] Write t=1000 ghwo=1 hdr=61F133A0 len=4",
        "[wave] cb energy t=1100 cb=2 want=200 take=200 ring=64 blocks=1 fake=0",
        "[wave] DONE t=1050 hdr=61F133A0 cb=351130B0 gid=1 flg=00000003",
        "[wave] cb underrun cb=2 want=200 avail=0",
        "[wave] service busy t=1 wait=1",
        "[wave] aa cb=2 short=0 slow=0 silent=0 min=100 maxgap_us=50 "
        "gap5=0 gap20=0 xruns=0 lockmax_us=7",
    ]
    profile = parse_lines(lines)
    measured = profile["measured"]
    assert measured["aaudio"]["callbacks_total"] == 2
    assert measured["aaudio"]["want_bytes_mean"] == 150
    assert measured["aaudio"]["energy_reports"] == 2
    assert measured["wom_done"]["count"] == 2
    assert measured["ring_occupancy_bytes"]["max"] == 64
    assert measured["blocks_in_flight_distribution"]["1"] == 1
    assert measured["service_busy_log_lines"] == 1
    assert measured["cb_underrun_log_lines"] == 1
    print("audio_runtime_profile: self-test passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="compare the log with the PASS11 table")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.log.is_file():
        print(f"audio profile: unavailable ({args.log} not found)",
              file=sys.stderr)
        return 2

    profile = load_profile(args.log)
    problems = check_against_pass11(profile)
    if args.check and problems:
        for problem in problems:
            print(f"audio profile: {problem}", file=sys.stderr)
        return 1

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(profile, indent=2) + "\n",
                             encoding="utf-8")
        print(f"audio profile: wrote {args.json}")
    elif args.check:
        print("audio profile: PASS11 counters match")
    else:
        print(json.dumps(profile, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
