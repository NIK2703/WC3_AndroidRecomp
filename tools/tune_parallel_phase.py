#!/usr/bin/env python3
"""Sweep one live SPMD phase knob and print a measurement table.

This is a host-side debug tool.  It talks to a running, debuggable install over
adb, rewrites one key in the app-private ``files/parallel.cfg`` atomically, and
reads the newest game log (never logcat).  The debug tuner applies the value at
the next empty phase window; this script then collects complete ``[phase-eff]``
intervals and prints a table.

The default sweep is ``phase_chunks=2,3,4,6``.  Repeated values are allowed and
are useful as controls (for example ``--values 2,3,4,6,3,2``).  At the end the
script restores the best value, the original value, or leaves the last value.

Examples::

    python3 tools/tune_parallel_phase.py
    python3 tools/tune_parallel_phase.py --values 2-6 --samples 5
    python3 tools/tune_parallel_phase.py --values 2,3,4,6,3,2 --restore best

The script is read/write only through ``run-as``; it never rebuilds, never uses
logcat, and never hardcodes a device address.  Pass ``--serial`` when more than
one device is connected.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = "wc3.recomp.debug"
DEFAULT_CONFIG = "files/parallel.cfg"
DEFAULT_LOG = "files/wc3_log_debug.txt"
DEFAULT_KEY = "phase_chunks"
DEFAULT_VALUES = "2,3,4,6"
DEFAULT_INTERVAL_S = 2.0
DEFAULT_SETTLE_S = 3.0
DEFAULT_SAMPLES = 6
DEFAULT_MIN_SAMPLES = 3


class SweepError(RuntimeError):
    pass


def _which_adb(explicit: str | None) -> str:
    if explicit:
        found = explicit if Path(explicit).exists() else shutil.which(explicit)
        if found:
            return found
        raise SweepError(f"adb not found: {explicit}")
    for env in ("ADB", "ANDROID_ADB"):
        value = os.environ.get(env)
        if value:
            found = value if Path(value).exists() else shutil.which(value)
            if found:
                return found
    found = shutil.which("adb")
    if found:
        return found
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_SDK"):
        root = os.environ.get(env)
        if not root:
            continue
        for name in ("adb", "adb.exe"):
            candidate = Path(root) / "platform-tools" / name
            if candidate.exists():
                return str(candidate)
    raise SweepError("adb not found; set --adb or ANDROID_HOME")


def _run(cmd: list[str], *, input_bytes: bytes | None = None,
         timeout: float = 60.0, verbose: bool = False) -> subprocess.CompletedProcess:
    if verbose:
        print("+ " + " ".join(shlex.quote(part) for part in cmd), file=sys.stderr)
    return subprocess.run(cmd, input=input_bytes, capture_output=True,
                          timeout=timeout, check=False)


def _decode(proc: subprocess.CompletedProcess) -> str:
    return proc.stdout.decode("utf-8", "replace")


def _check(proc: subprocess.CompletedProcess, what: str) -> None:
    if proc.returncode == 0:
        return
    err = proc.stderr.decode("utf-8", "replace").strip()
    out = proc.stdout.decode("utf-8", "replace").strip()
    detail = err or out or f"exit {proc.returncode}"
    raise SweepError(f"{what} failed: {detail}")


def _adb_devices(adb: str, verbose: bool) -> list[str]:
    proc = _run([adb, "devices"], timeout=30.0, verbose=verbose)
    _check(proc, "adb devices")
    devices: list[str] = []
    for line in _decode(proc).splitlines():
        line = line.strip()
        if not line or line.startswith("List of devices"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def _resolve_serial(adb: str, explicit: str | None, verbose: bool) -> str:
    devices = _adb_devices(adb, verbose)
    if explicit:
        if explicit not in devices:
            raise SweepError(
                f"device {explicit!r} is not connected; connected: {devices or 'none'}")
        return explicit
    if len(devices) == 1:
        return devices[0]
    if not devices:
        raise SweepError("no adb device connected")
    raise SweepError(
        "more than one adb device is connected; pass --serial: " + ", ".join(devices))


def _safe_token(value: str, what: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
        raise SweepError(f"unsafe {what}: {value!r}")
    return value


def _safe_path(value: str, what: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_./-]+", value):
        raise SweepError(f"unsafe {what}: {value!r}")
    if value.startswith("/") or ".." in value.split("/"):
        raise SweepError(f"{what} must stay under the app data dir: {value!r}")
    return value


class Device:
    def __init__(self, adb: str, serial: str, package: str, verbose: bool) -> None:
        self.adb = adb
        self.serial = serial
        self.package = _safe_token(package, "package")
        self.verbose = verbose

    def _shell(self, command: str, *, input_bytes: bytes | None = None,
               timeout: float = 60.0) -> subprocess.CompletedProcess:
        return _run([self.adb, "-s", self.serial, "shell", command],
                    input_bytes=input_bytes, timeout=timeout,
                    verbose=self.verbose)

    def _exec_out(self, args: list[str], *, timeout: float = 60.0) -> subprocess.CompletedProcess:
        return _run([self.adb, "-s", self.serial, "exec-out",
                     "run-as", self.package, *args], timeout=timeout,
                    verbose=self.verbose)

    def read_text(self, path: str) -> str:
        path = _safe_path(path, "path")
        proc = self._exec_out(["cat", path])
        _check(proc, f"read {path}")
        return _decode(proc)

    def read_tail(self, path: str, offset: int) -> str:
        path = _safe_path(path, "path")
        proc = self._exec_out(["tail", "-c", f"+{offset + 1}", path])
        if proc.returncode == 0:
            return _decode(proc)
        # Older toybox builds may not support `tail -c +N`; fall back to a full
        # read and a byte slice.  This is still the game log, not logcat.
        text = self.read_text(path).encode("utf-8", "replace")
        return text[offset:].decode("utf-8", "replace")

    def size(self, path: str) -> int:
        path = _safe_path(path, "path")
        proc = self._shell(f"run-as {self.package} stat -c %s {path}")
        if proc.returncode != 0:
            proc = self._shell(f"run-as {self.package} wc -c {path}")
        _check(proc, f"stat {path}")
        match = re.search(r"\d+", _decode(proc))
        if not match:
            raise SweepError(f"cannot parse size of {path}")
        return int(match.group(0))

    def write_text(self, path: str, text: str) -> None:
        path = _safe_path(path, "path")
        tmp = path + ".tmp"
        # The redirection must be parsed by the app-side sh, so the whole
        # `sh -c '...'` fragment is sent as one adb shell argument.
        proc = self._shell(
            f"run-as {self.package} sh -c 'cat > {tmp}'",
            input_bytes=text.encode("utf-8"))
        _check(proc, f"write {path}")
        proc = self._shell(f"run-as {self.package} mv {tmp} {path}")
        _check(proc, f"rename {path}")
        actual = self.read_text(path)
        if actual != text:
            raise SweepError(f"read-back mismatch for {path}")


def _parse_pairs(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line))


def _parse_values(spec: str) -> list[int]:
    values: list[int] = []
    for raw in spec.split(","):
        token = raw.strip()
        if not token:
            continue
        if re.fullmatch(r"\d+", token):
            values.append(int(token))
            continue
        match = re.fullmatch(r"(\d+)-(\d+)(?::(\d+))?", token)
        if not match:
            raise SweepError(f"bad --values token: {token!r}")
        start, end = int(match.group(1)), int(match.group(2))
        step = int(match.group(3) or "1")
        if step <= 0:
            raise SweepError(f"bad --values step: {token!r}")
        values.extend(range(start, end + 1, step))
    if not values:
        raise SweepError("--values produced no values")
    if any(value < 2 for value in values):
        raise SweepError("phase_chunks values must be >= 2")
    return values


def _get_key(text: str, key: str) -> str | None:
    pattern = re.compile(rf"^\s*{re.escape(key)}\s*=\s*(\S+)\s*$")
    found = None
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            found = match.group(1)
    return found


def _with_key(text: str, key: str, value: int | str, marker: str) -> str:
    lines = text.splitlines()
    key_re = re.compile(rf"^\s*{re.escape(key)}\s*=")
    lines = [line for line in lines if not key_re.match(line)]
    lines = [line for line in lines if not line.startswith("# tune_parallel_phase ")]
    lines.append(f"# tune_parallel_phase key={key} value={value} marker={marker}")
    lines.append(f"{key}={value}")
    return "\n".join(lines) + "\n"


def _phase_state(log_text: str) -> dict[str, Any]:
    phase_lines = [line for line in log_text.splitlines()
                   if line.startswith("[phase] ") and "enabled=" in line]
    if not phase_lines:
        raise SweepError(
            "no `[phase] enabled=...` line in the game log; "
            "is the running build WC3_DEBUG_PARALLEL?")
    phase = _parse_pairs(phase_lines[-1])
    chunks_match = re.match(r"\d+", phase.get("chunks", "0"))
    chunks = int(chunks_match.group(0)) if chunks_match else 0

    workers = None
    for line in log_text.splitlines():
        match = re.search(r"\[parallel\] worker cores:.*\((\d+) worker\(s\)", line)
        if match:
            workers = int(match.group(1))
    return {
        "line": phase_lines[-1],
        "enabled": int(phase.get("enabled", "0")),
        "shadow": int(phase.get("shadow", "0")),
        "shadow_cfg": int(phase.get("shadow_cfg", "0")),
        "main_last": int(phase.get("main_last", "1")),
        "chunks": chunks,
        "workers": workers,
    }


def _parse_phase_metrics(text: str) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    for line in text.splitlines():
        if not line.startswith("[phase-eff] "):
            continue
        pairs = _parse_pairs(line)
        sample: dict[str, Any] = {}
        for key, value in pairs.items():
            if key in {"side"}:
                sample[key] = value
                continue
            try:
                sample[key] = int(value)
            except ValueError:
                try:
                    sample[key] = float(value)
                except ValueError:
                    sample[key] = value
        samples.append(sample)
    return samples


def _parse_reloads(text: str) -> list[dict[str, Any]]:
    reloads: list[dict[str, Any]] = []
    for line in text.splitlines():
        marker = "[phase-tune] reload:"
        if not line.startswith(marker):
            continue
        pairs = _parse_pairs(line[len(marker):])
        item: dict[str, Any] = {}
        for key, value in pairs.items():
            try:
                item[key] = int(value)
            except ValueError:
                item[key] = value
        reloads.append(item)
    return reloads


def _valid_sample(sample: dict[str, Any], key: str, value: int,
                  expected_chunks: int | None) -> bool:
    if int(sample.get("phase_applies", 0) or 0) <= 0:
        return False
    if int(sample.get("phase_par_runs", 0) or 0) <= 0:
        return False
    if int(sample.get("phase_seq_runs", 0) or 0) != 0:
        return False
    if key == "phase_chunks":
        actual = float(sample.get("phase_chunks_avg", 0.0) or 0.0)
        if abs(actual - float(value)) > 0.01:
            return False
    elif expected_chunks is not None:
        actual = float(sample.get("phase_chunks_avg", 0.0) or 0.0)
        if abs(actual - float(expected_chunks)) > 0.01:
            return False
    return True


def _median(samples: list[dict[str, Any]], field: str) -> float | None:
    values: list[float] = []
    for sample in samples:
        value = sample.get(field)
        if value is None or isinstance(value, str):
            continue
        try:
            values.append(float(value))
        except (TypeError, ValueError):
            continue
    return statistics.median(values) if values else None


def _stdev(samples: list[dict[str, Any]], field: str) -> float | None:
    values: list[float] = []
    for sample in samples:
        value = sample.get(field)
        if value is None or isinstance(value, str):
            continue
        try:
            values.append(float(value))
        except (TypeError, ValueError):
            continue
    return statistics.pstdev(values) if len(values) > 1 else 0.0


def _summary(samples: list[dict[str, Any]]) -> dict[str, Any]:
    apply_us = _median(samples, "phase_apply_avg_us")
    run_us = _median(samples, "phase_run_avg_us")
    slice_us = _median(samples, "phase_slice_avg_us")
    serial_us = None
    if apply_us is not None and run_us is not None:
        serial_us = max(0.0, apply_us - run_us)
    slice_run = None
    if slice_us is not None and run_us:
        slice_run = slice_us / run_us
    return {
        "n": len(samples),
        "chunks": _median(samples, "phase_chunks_avg"),
        "count": _median(samples, "phase_avg_count"),
        "applies": _median(samples, "phase_applies"),
        "items": _median(samples, "phase_items"),
        "apply_us": apply_us,
        "apply_sd": _stdev(samples, "phase_apply_avg_us"),
        "run_us": run_us,
        "serial_us": serial_us,
        "wake_us": _median(samples, "phase_wake_avg_us"),
        "join_us": _median(samples, "phase_join_avg_us"),
        "slice_us": slice_us,
        "slice_run": slice_run,
        "par_runs": _median(samples, "phase_par_runs"),
        "seq_runs": _median(samples, "phase_seq_runs"),
    }


def _fmt(value: Any, digits: int = 1) -> str:
    if value is None:
        return "-"
    if isinstance(value, float) and not math.isfinite(value):
        return "-"
    return f"{value:.{digits}f}"


def _print_table(title: str, rows: list[dict[str, Any]]) -> None:
    print()
    print(title)
    header = (
        f"{'value':>5} {'n':>3} {'chunks':>6} {'count':>5} {'applies':>7} "
        f"{'items/int':>9} {'apply_us':>8} {'apply_sd':>8} {'run_us':>7} "
        f"{'serial_us':>9} {'wake_us':>7} {'join_us':>7} {'slice_us':>8} "
        f"{'slice/run':>9} {'par':>5} {'seq':>3}")
    print(header)
    print("-" * len(header))
    for row in rows:
        summary = row["summary"]
        n = summary["n"]
        if n == 0:
            print(f"{str(row['value']):>5} {0:>3}  (no complete samples)")
            continue
        print(
            f"{str(row['value']):>5} {n:>3} "
            f"{_fmt(summary['chunks'], 2):>6} "
            f"{_fmt(summary['count'], 1):>5} "
            f"{_fmt(summary['applies'], 0):>7} "
            f"{_fmt(summary['items'], 0):>9} "
            f"{_fmt(summary['apply_us'], 1):>8} "
            f"{_fmt(summary['apply_sd'], 1):>8} "
            f"{_fmt(summary['run_us'], 1):>7} "
            f"{_fmt(summary['serial_us'], 1):>9} "
            f"{_fmt(summary['wake_us'], 1):>7} "
            f"{_fmt(summary['join_us'], 1):>7} "
            f"{_fmt(summary['slice_us'], 1):>8} "
            f"{_fmt(summary['slice_run'], 2):>9} "
            f"{_fmt(summary['par_runs'], 0):>5} "
            f"{_fmt(summary['seq_runs'], 0):>3}")


def _group_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault(int(row["value"]), []).extend(row["samples"])
    return [{"value": value, "samples": samples, "summary": _summary(samples)}
            for value, samples in sorted(grouped.items())]


def _pick_best(groups: list[dict[str, Any]], min_samples: int) -> dict[str, Any] | None:
    candidates = [row for row in groups
                  if row["summary"]["n"] >= min_samples
                  and row["summary"]["apply_us"] is not None]
    if not candidates:
        return None
    candidates.sort(key=lambda row: (
        row["summary"]["apply_us"],
        row["summary"]["run_us"] if row["summary"]["run_us"] is not None else math.inf,
        row["value"],
    ))
    return candidates[0]


def _write_segment(path: Path, value: int, segment: str) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(f"\n===== {DEFAULT_KEY}={value} =====\n")
        handle.write(segment)
        if segment and not segment.endswith("\n"):
            handle.write("\n")


def _sweep(device: Device, args: argparse.Namespace, values: list[int],
           original: str, key: str, workers: int | None,
           expected_chunks: int | None) -> tuple[list[dict[str, Any]], list[str]]:
    rows: list[dict[str, Any]] = []
    segments: list[str] = []
    max_chunks = workers if workers else 8
    if max_chunks < 2:
        max_chunks = 2
    if key == "phase_chunks":
        clamped: list[int] = []
        for value in values:
            effective = min(value, max_chunks)
            if effective != value:
                print(f"warning: {key}={value} is clamped by the pool to "
                      f"{effective} (workers={max_chunks})", file=sys.stderr)
            clamped.append(effective)
        values = clamped

    for index, value in enumerate(values, start=1):
        print(f"[{index}/{len(values)}] {key}={value} "
              f"settle={args.settle:.1f}s samples={args.samples}",
              file=sys.stderr)
        offset = device.size(args.log)
        marker = f"{_dt.datetime.now().strftime('%Y%m%d_%H%M%S')}-{index}"
        content = _with_key(original, key, value, marker)
        if args.dry_run:
            print(f"--- would write {args.config} ---\n{content}", file=sys.stderr)
            continue
        args.wrote = True
        device.write_text(args.config, content)
        time.sleep(max(0.0, args.settle))
        time.sleep(max(0.0, args.samples * args.interval + 0.75))
        segment = device.read_tail(args.log, offset)
        all_samples = _parse_phase_metrics(segment)
        reloads = _parse_reloads(segment)
        valid = [sample for sample in all_samples
                 if _valid_sample(sample, key, value, expected_chunks)]
        summary = _summary(valid)
        rows.append({
            "value": value,
            "samples": valid,
            "all_samples": all_samples,
            "reloads": reloads,
            "summary": summary,
        })
        segments.append(segment)
        print(f"    reloads={len(reloads)} samples={len(all_samples)} "
              f"valid={len(valid)} chunks={_fmt(summary['chunks'], 2)} "
              f"apply_us={_fmt(summary['apply_us'], 1)} "
              f"run_us={_fmt(summary['run_us'], 1)}",
              file=sys.stderr)
        if reloads:
            last = reloads[-1]
            print(f"    applied: chunks={last.get('chunks', '-')} "
                  f"shadow={last.get('shadow', '-')} "
                  f"enabled={last.get('enabled', '-')}", file=sys.stderr)
        if not valid:
            print("    warning: no complete samples; is the game paused or "
                  "is the log stale?", file=sys.stderr)
    return rows, segments


def _restore(device: Device, args: argparse.Namespace, original: str,
             key: str, value: int | str) -> None:
    print(f"restoring {key}={value} ...", file=sys.stderr)
    marker = f"restore-{_dt.datetime.now().strftime('%Y%m%d_%H%M%S')}"
    content = _with_key(original, key, value, marker)
    # Record the offset before the write so the confirmation can see the
    # tuner's reload line even though it appears during the settle window.
    offset = device.size(args.log)
    device.write_text(args.config, content)
    time.sleep(max(0.0, args.settle))
    # Read only the new tail; this is confirmation, not another measurement.
    segment = device.read_tail(args.log, offset)
    reloads = _parse_reloads(segment)
    if reloads:
        last = reloads[-1]
        print(f"restored: chunks={last.get('chunks', '-')} "
              f"shadow={last.get('shadow', '-')} "
              f"enabled={last.get('enabled', '-')}", file=sys.stderr)
    else:
        print("restored; no reload line seen yet (tuner may be idle)",
              file=sys.stderr)


def _save(args: argparse.Namespace, key: str, original: str,
          state: dict[str, Any], rows: list[dict[str, Any]],
          segments: list[str], best: dict[str, Any] | None,
          restored: str) -> Path | None:
    if args.no_save:
        return None
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    json_path = Path(args.json_out) if args.json_out else out_dir / f"{key}_sweep_{stamp}.json"
    raw_path = out_dir / f"{key}_sweep_{stamp}.log"
    if not json_path.is_absolute():
        json_path = ROOT / json_path
    json_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema": 1,
        "created": _dt.datetime.now().isoformat(timespec="seconds"),
        "device": args.serial,
        "package": args.package,
        "config": args.config,
        "log": args.log,
        "key": key,
        "original_key_value": _get_key(original, key),
        "phase_state": state,
        "settings": {
            "settle_s": args.settle,
            "samples": args.samples,
            "interval_s": args.interval,
            "min_samples": args.min_samples,
        },
        "results": rows,
        "grouped": _group_rows(rows),
        "best": best,
        "restored": restored,
    }
    json_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    raw_path.write_text("", encoding="utf-8")
    for index, segment in enumerate(segments):
        with raw_path.open("a", encoding="utf-8") as handle:
            handle.write(f"\n===== segment {index + 1} =====\n")
            handle.write(segment)
            if segment and not segment.endswith("\n"):
                handle.write("\n")
    print(f"saved: {json_path}", file=sys.stderr)
    print(f"saved: {raw_path}", file=sys.stderr)
    return json_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Sweep one live SPMD phase key through parallel.cfg and "
                    "print a [phase-eff] table.")
    parser.add_argument("--adb", help="adb executable (default: PATH/ANDROID_HOME)")
    parser.add_argument("--serial", help="adb serial; auto-selected if exactly one device")
    parser.add_argument("--package", default=DEFAULT_PACKAGE,
                        help=f"debug package (default: {DEFAULT_PACKAGE})")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help=f"app-private config (default: {DEFAULT_CONFIG})")
    parser.add_argument("--log", default=DEFAULT_LOG,
                        help=f"app-private game log (default: {DEFAULT_LOG})")
    parser.add_argument("--key", default=DEFAULT_KEY,
                        help=f"config key to sweep (default: {DEFAULT_KEY})")
    parser.add_argument("--values", default=DEFAULT_VALUES,
                        help=f"comma list or range, e.g. 2,3,4,6 or 2-6 "
                             f"(default: {DEFAULT_VALUES})")
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_S,
                        help=f"seconds after write before sampling (default: {DEFAULT_SETTLE_S})")
    parser.add_argument("--samples", type=int, default=DEFAULT_SAMPLES,
                        help=f"target [phase-eff] intervals per value (default: {DEFAULT_SAMPLES})")
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_S,
                        help=f"metric interval seconds (default: {DEFAULT_INTERVAL_S})")
    parser.add_argument("--min-samples", type=int, default=DEFAULT_MIN_SAMPLES,
                        help=f"minimum complete samples for best selection (default: {DEFAULT_MIN_SAMPLES})")
    parser.add_argument("--baseline", type=int,
                        help="effective value to restore when the original config has no key (default: 2)")
    parser.add_argument("--restore", choices=("best", "original", "none"),
                        default="best", help="what to leave in parallel.cfg")
    parser.add_argument("--out-dir", default="reports/generated",
                        help="directory for JSON/raw evidence (default: reports/generated)")
    parser.add_argument("--json-out", help="explicit JSON path")
    parser.add_argument("--no-save", action="store_true", help="do not write evidence files")
    parser.add_argument("--dry-run", action="store_true",
                        help="read state and print planned configs; do not write")
    parser.add_argument("--verbose", action="store_true", help="print adb commands")
    args = parser.parse_args(argv)

    key = args.key
    if not re.fullmatch(r"[A-Za-z0-9_]+", key):
        parser.error("--key must contain only letters, digits and underscores")
    if args.samples < 1:
        parser.error("--samples must be >= 1")
    if args.interval <= 0:
        parser.error("--interval must be > 0")
    if args.min_samples < 1:
        parser.error("--min-samples must be >= 1")
    values = _parse_values(args.values)

    adb = _which_adb(args.adb)
    serial = _resolve_serial(adb, args.serial, args.verbose)
    args.serial = serial
    device = Device(adb, serial, args.package, args.verbose)
    print(f"device={serial} package={args.package}", file=sys.stderr)

    original = device.read_text(args.config)
    log_text = device.read_text(args.log)
    state = _phase_state(log_text)
    print(f"phase: {state['line']}", file=sys.stderr)
    if state["enabled"] != 1:
        raise SweepError("phase is disabled; set phase=1 in parallel.cfg first")
    if state["shadow"] != 0:
        raise SweepError("phase shadow mode is active; set phase_shadow=0 and "
                         "phase=1 before measuring the product path")
    if state["workers"]:
        print(f"pool workers={state['workers']}", file=sys.stderr)
    if key == "phase_chunks" and args.baseline is None:
        original_value = _get_key(original, key)
        if original_value is not None and original_value.isdigit():
            args.baseline = int(original_value)
        else:
            args.baseline = 2
    if key != "phase_chunks":
        print(f"note: key={key}; sample validity does not filter on chunks",
              file=sys.stderr)

    expected_chunks = None
    if key != "phase_chunks":
        expected_chunks = state["chunks"] or None

    rows: list[dict[str, Any]] = []
    segments: list[str] = []
    best: dict[str, Any] | None = None
    restored = "none"
    args.wrote = False
    completed = False
    try:
        rows, segments = _sweep(device, args, values, original, key,
                                state["workers"], expected_chunks)
        if args.dry_run:
            print("dry-run: no measurements", file=sys.stderr)
            return 0
        if not rows:
            raise SweepError("no values were measured")

        _print_table("per-run samples", rows)
        print("\nnote: one sample is one [phase-eff] interval (~2 s); "
              "apply_us includes run_us, serial_us = apply_us - run_us.",
              file=sys.stderr)
        groups = _group_rows(rows)
        if len(groups) != len(rows):
            _print_table("grouped by value (all runs)", groups)
        best = _pick_best(groups, args.min_samples)
        if best is None:
            print(f"no value has at least {args.min_samples} complete samples; "
                  f"best selection skipped", file=sys.stderr)
        else:
            summary = best["summary"]
            print(f"\nbest: {key}={best['value']} "
                  f"median apply_us={_fmt(summary['apply_us'], 1)} "
                  f"run_us={_fmt(summary['run_us'], 1)} "
                  f"slice_us={_fmt(summary['slice_us'], 1)} "
                  f"chunks={_fmt(summary['chunks'], 2)}", file=sys.stderr)
            ranked = sorted(
                [row for row in groups
                 if row["summary"]["n"] >= args.min_samples
                 and row["summary"]["apply_us"] is not None],
                key=lambda row: row["summary"]["apply_us"])
            if len(ranked) > 1:
                second = ranked[1]
                delta = (second["summary"]["apply_us"] -
                         ranked[0]["summary"]["apply_us"])
                base = ranked[0]["summary"]["apply_us"] or 1.0
                if abs(delta) / base < 0.02:
                    print(f"note: {ranked[0]['value']} and "
                          f"{second['value']} are within 2% "
                          f"({delta:+.1f} us); prefer the lower-overhead value "
                          f"if the scene is unchanged", file=sys.stderr)

        restore_value: int | str | None = None
        if args.restore == "best":
            if best is not None:
                restore_value = best["value"]
            else:
                original_value = _get_key(original, key)
                if original_value is not None:
                    restore_value = original_value
                elif key == "phase_chunks":
                    restore_value = args.baseline
                else:
                    restore_value = state["chunks"]
        elif args.restore == "original":
            original_value = _get_key(original, key)
            if original_value is None:
                restore_value = args.baseline if key == "phase_chunks" else state["chunks"]
            else:
                restore_value = original_value
        if restore_value is not None:
            _restore(device, args, original, key, restore_value)
            restored = str(restore_value)
        else:
            print("restore=none: leaving the last measured value", file=sys.stderr)
            restored = "none"
        completed = True
        _save(args, key, original, state, rows, segments, best, restored)
        return 0
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        raise
    finally:
        if args.wrote and not completed and not args.dry_run:
            # Best-effort safety restore on a failed/interrupted sweep.  Do not
            # mask the original error if this also fails.
            try:
                fallback: int | str = args.baseline if key == "phase_chunks" else state["chunks"]
                print(f"error path: restoring {key}={fallback}", file=sys.stderr)
                _restore(device, args, original, key, fallback)
            except Exception as exc:  # noqa: BLE001 - keep the original failure
                print(f"error path restore failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SweepError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        raise SystemExit(130)
