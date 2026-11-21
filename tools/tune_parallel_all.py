#!/usr/bin/env python3
"""Two-pass coordinate-descent tuner for the live SPMD phase.

The script sweeps every *product* phase knob through ``parallel.cfg``, one knob
at a time, keeps the best value, and repeats the pass with a local refinement
around each pass-1 optimum.  It prints per-knob tables, a note and a next-step
idea for every knob, and writes a Markdown/JSON report plus the raw game-log
segments.

It deliberately does not use logcat and does not rebuild.  It requires a
debuggable install (``run-as``) and a running map.

Examples::

    python3 tools/tune_parallel_all.py --dry-run
    python3 tools/tune_parallel_all.py --samples 3 --settle 1.5
    python3 tools/tune_parallel_all.py --passes 2 --pass2 full

Scope:
  measured: phase_chunks, phase_main_last, phase_weighted, phase_spin_us,
            phase_stack_zero (safe subset), phase_threshold
  skipped:  phase, phase_shadow (mode switches), phase_max (shadow-only),
            old ray/vertex/triangle keys (old chunked paths are disabled)

The selection metric is the median ``phase_apply_avg_us`` over complete
``[phase-eff]`` intervals.  ``run_us``, ``seq_us``, ``join_us`` and throughput
are diagnostics.  ``phase_stack_zero`` is measured but never lowered below the
256-byte safety default by this tool; a shadow exactness run is required before
that is considered.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Running `python3 tools/tune_parallel_all.py` puts tools/ on sys.path.
from tune_parallel_phase import (  # noqa: E402
    Device,
    SweepError,
    _fmt,
    _get_key,
    _median,
    _parse_pairs,
    _parse_phase_metrics,
    _parse_reloads,
    _phase_state,
    _resolve_serial,
    _summary,
    _which_adb,
    _with_key,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = "wc3.recomp.debug"
DEFAULT_CONFIG = "files/parallel.cfg"
DEFAULT_LOG = "files/wc3_log_debug.txt"
DEFAULT_INTERVAL_S = 2.0
DEFAULT_SETTLE_S = 1.5
DEFAULT_SAMPLES = 3
DEFAULT_MIN_SAMPLES = 2

BASELINE = {
    "phase_chunks": 2,
    "phase_main_last": 1,
    "phase_weighted": 0,
    "phase_spin_us": 1000,
    "phase_stack_zero": 256,
    "phase_threshold": 2,
}

# The phase config field that the tuner prints in `[phase-tune] reload`.
RELOAD_FIELD = {
    "phase_chunks": "chunks",
    "phase_main_last": "main_last",
    "phase_weighted": "weighted",
    "phase_threshold": "threshold",
    "phase_stack_zero": "stack_zero",
    "phase_spin_us": "spin_us",
}


@dataclass
class Param:
    key: str
    values: list[int]
    kind: str = "perf"          # perf | threshold | safety
    apply_best: bool = True
    lower: int | None = None
    upper: int | None = None
    description: str = ""


SKIPPED = [
    ("phase", "mode switch; product measurement requires phase=1"),
    ("phase_shadow", "mode switch; shadow is a separate exactness run"),
    ("phase_max", "shadow-only capture window; product phase does not read it"),
    ("ray_threshold", "old chunked raycast path is disabled in this build"),
    ("ray_chunks", "old chunked raycast path is disabled in this build"),
    ("min_chunk", "old chunked raycast path is disabled in this build"),
    ("main_last", "old chunked path key; phase uses phase_main_last"),
    ("weighted_split", "old chunked path key; phase uses phase_weighted"),
    ("reject_slow", "old chunked path is disabled in this build"),
    ("adaptive", "old main-spin control; phase uses phase_spin_us"),
    ("main_spin_us", "old chunked path key; phase uses phase_spin_us"),
    ("worker_spin_us", "old chunked path is disabled in this build"),
    ("vertex_threshold", "old vertex path is disabled in this build"),
    ("vertex_chunks", "old vertex path is disabled in this build"),
    ("vertex_min_chunk", "old vertex path is disabled in this build"),
    ("triangle_threshold", "old triangle path is disabled in this build"),
    ("triangle_chunks", "old triangle path is disabled in this build"),
    ("triangle_min_chunk", "old triangle path is disabled in this build"),
]


def _params(workers: int) -> list[Param]:
    chunk_max = max(2, workers)
    return [
        Param("phase_chunks", [2, 3, 4, 6, 8],
              lower=2, upper=chunk_max,
              description="chunks per phase window; 2 = main + one worker"),
        Param("phase_main_last", [1, 0],
              description="caller runs the last chunk (1) or waits (0)"),
        Param("phase_weighted", [0, 1],
              description="equal (0) or weighted (1) split"),
        Param("phase_spin_us", [0, 100, 500, 1000, 2000, 5000],
              lower=0, upper=1_000_000,
              description="main-thread join spin before sleeping"),
        Param("phase_stack_zero", [256, 512, 1024, 4096],
              kind="safety", apply_best=False,
              lower=64, upper=4096,
              description="bytes of worker stack cleared per chunk; "
                          "below 256 is safety-sensitive"),
        Param("phase_threshold", [2, 8, 32, 64, 80, 256],
              kind="threshold", apply_best=True,
              lower=2, upper=256,
              description="minimum requests for fork-join; above count = "
                          "sequential fallback"),
    ]


def _int_field(pairs: dict[str, str], key: str, default: int) -> int:
    raw = pairs.get(key, str(default))
    match = re.match(r"\d+", raw)
    return int(match.group(0)) if match else default


def _current_values(pairs: dict[str, str], workers: int) -> dict[str, int]:
    chunks = _int_field(pairs, "chunks", 2)
    if chunks == 0:
        chunks = min(workers, 4)
    return {
        "phase_chunks": max(2, min(chunks, workers)),
        "phase_main_last": _int_field(pairs, "main_last", 1),
        "phase_weighted": _int_field(pairs, "weighted", 0),
        "phase_threshold": _int_field(pairs, "threshold", 2),
        "phase_stack_zero": _int_field(pairs, "stack_zero", 256),
        "phase_spin_us": _int_field(pairs, "spin_us", 1000),
    }


def _effective_chunks(value: int, workers: int) -> int:
    if value == 0:
        return min(workers, 4)
    return max(2, min(value, workers))


def _reload_matches(reloads: list[dict[str, Any]], key: str, value: int,
                    workers: int) -> tuple[bool, str]:
    if not reloads:
        return False, "no [phase-tune] reload"
    field = RELOAD_FIELD.get(key)
    if field is None:
        return False, f"no reload field for {key}"
    last = reloads[-1]
    raw = last.get(field)
    if raw is None:
        return False, f"reload has no {field}"
    match = re.match(r"\d+", str(raw))
    if not match:
        return False, f"cannot parse reload {field}={raw!r}"
    actual = int(match.group(0))
    expected = value
    if key == "phase_chunks":
        expected = _effective_chunks(value, workers)
    if actual != expected:
        return False, f"reload {field}={actual}, expected {expected}"
    return True, ""


def _fatal_marker(text: str) -> str | None:
    for marker in ("[phase] commit failed", "phase run failed",
                   "FATAL", "Aborted", "assertion failed"):
        if marker in text:
            return marker
    return None


def _valid_sample(sample: dict[str, Any], param: Param, value: int,
                  workers: int, reload_ok: bool) -> bool:
    if not reload_ok:
        return False
    applies = int(sample.get("phase_applies", 0) or 0)
    par = int(sample.get("phase_par_runs", 0) or 0)
    seq = int(sample.get("phase_seq_runs", 0) or 0)
    if applies <= 0 or par + seq <= 0:
        return False
    if param.kind != "threshold" and seq != 0:
        return False
    if param.key == "phase_chunks":
        actual = float(sample.get("phase_chunks_avg", 0.0) or 0.0)
        if abs(actual - float(_effective_chunks(value, workers))) > 0.01:
            return False
    return True


def _summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    summary = _summary(samples)
    summary["seq_us"] = _median(samples, "phase_seq_avg_us")
    par = summary.get("par_runs") or 0
    seq = summary.get("seq_runs") or 0
    active = summary.get("run_us") if par > 0 else summary.get("seq_us")
    if summary.get("apply_us") is not None and active is not None:
        summary["serial_us"] = max(0.0, summary["apply_us"] - active)
    return summary


def _table(title: str, param: Param, rows: list[dict[str, Any]]) -> list[str]:
    lines = ["", title]
    header = (
        f"{'value':>5} {'n':>3} {'chunks':>6} {'applies':>7} {'items/int':>9} "
        f"{'apply_us':>8} {'run_us':>7} {'seq_us':>7} {'join_us':>7} "
        f"{'wake_us':>7} {'slice_us':>8} {'par':>5} {'seq':>3}")
    lines.append(header)
    lines.append("-" * len(header))
    for row in rows:
        summary = row["summary"]
        n = summary["n"]
        if n == 0:
            lines.append(f"{str(row['value']):>5} {0:>3}  (no complete samples)")
            continue
        lines.append(
            f"{str(row['value']):>5} {n:>3} "
            f"{_fmt(summary.get('chunks'), 2):>6} "
            f"{_fmt(summary.get('applies'), 0):>7} "
            f"{_fmt(summary.get('items'), 0):>9} "
            f"{_fmt(summary.get('apply_us'), 1):>8} "
            f"{_fmt(summary.get('run_us'), 1):>7} "
            f"{_fmt(summary.get('seq_us'), 1):>7} "
            f"{_fmt(summary.get('join_us'), 1):>7} "
            f"{_fmt(summary.get('wake_us'), 1):>7} "
            f"{_fmt(summary.get('slice_us'), 1):>8} "
            f"{_fmt(summary.get('par_runs'), 0):>5} "
            f"{_fmt(summary.get('seq_runs'), 0):>3}")
    return lines


def _select_best(param: Param, rows: list[dict[str, Any]],
                 min_samples: int) -> dict[str, Any] | None:
    candidates = [
        row for row in rows
        if row["summary"]["n"] >= min_samples
        and row["summary"].get("apply_us") is not None
        and (param.kind == "threshold"
             or (row["summary"].get("seq_runs") or 0) == 0)
    ]
    if not candidates:
        return None
    candidates.sort(key=lambda row: (
        row["summary"]["apply_us"],
        row["summary"].get("run_us")
        if row["summary"].get("run_us") is not None
        else (row["summary"].get("seq_us") or math.inf),
        row["value"],
    ))
    return candidates[0]


def _baseline_row(param: Param, rows: list[dict[str, Any]],
                  current: int) -> dict[str, Any] | None:
    for row in rows:
        if row["value"] == current:
            return row
    return None


def _should_apply(param: Param, best: dict[str, Any] | None,
                  baseline: dict[str, Any] | None, current: int,
                  pass_no: int, history: dict[str, Any] | None,
                  min_improvement: float,
                  consistency_margin: float) -> tuple[bool, str]:
    """Return (apply, reason) using noise and pass-consistency guards."""
    if best is None:
        return False, "no complete samples; keep current"
    if param.kind == "safety":
        return False, "safety knob; keep current"
    summary = best["summary"]
    value = int(best["value"])
    par = summary.get("par_runs") or 0
    seq = summary.get("seq_runs") or 0
    if param.kind == "threshold" and par == 0 and seq > 0:
        return False, "sequential fallback; phase on/off decision"
    if value == current:
        return False, "current value is optimal"
    if baseline is None or not baseline["summary"].get("apply_us"):
        return False, "no baseline sample for the current value"
    base = baseline["summary"]["apply_us"]
    best_apply = summary.get("apply_us")
    if best_apply is None:
        return False, "best value has no apply time"
    improvement = (base - best_apply) / base * 100.0
    if improvement < min_improvement:
        return False, (f"within noise ({improvement:.1f}% < "
                       f"{min_improvement:.1f}%)")
    if (pass_no > 1 and history is not None
            and int(history["value"]) != value):
        if improvement < consistency_margin:
            return False, (
                f"pass-1 best {history['value']} differs; "
                f"{improvement:.1f}% < {consistency_margin:.1f}% "
                f"consistency margin")
    return True, f"improvement {improvement:.1f}%"


def _note(param: Param, best: dict[str, Any] | None,
          baseline: dict[str, Any] | None, current: int,
          values: list[int], workers: int) -> str:
    if best is None:
        return "no complete samples; keep the current value and re-measure"
    summary = best["summary"]
    value = best["value"]
    apply_us = summary.get("apply_us")
    par = summary.get("par_runs") or 0
    seq = summary.get("seq_runs") or 0
    delta = None
    if baseline is not None and baseline["summary"].get("apply_us"):
        base = baseline["summary"]["apply_us"]
        delta = (apply_us - base) / base * 100.0

    if param.kind == "threshold" and par == 0 and seq > 0:
        return (f"{value} forces sequential fallback; "
                f"apply_us={_fmt(apply_us, 1)} us. The parallel path is not "
                f"used at this threshold; this is a phase on/off decision, "
                f"not a parallel speedup.")
    if param.kind == "safety":
        return (f"performance best at {value}, but this is a safety knob; "
                f"keep 256 unless a shadow run proves a lower value equal.")
    if param.key == "phase_main_last":
        if value == current:
            return f"current main_last={value} is optimal; keep it."
        mode = "caller runs the last chunk" if value else "caller waits; workers run all chunks"
        suffix = f" ({delta:+.1f}% vs baseline)" if delta is not None else ""
        return f"main_last={value} is better: {mode}{suffix}."
    if param.key == "phase_weighted":
        if value == current:
            return f"current weighted={value} is optimal; keep it."
        mode = "weighted split" if value else "equal split"
        suffix = f" ({delta:+.1f}% vs baseline)" if delta is not None else ""
        return f"{mode} is better{suffix}."
    if value == current:
        suffix = f" ({delta:+.1f}% vs baseline)" if delta is not None else ""
        return f"current value is optimal{suffix}; keep it."
    if abs(delta or 0.0) < 2.0:
        return (f"{value} is within noise of the current value "
                f"({delta:+.1f}%); prefer the current value.")
    all_values = sorted(param.values)
    if value == min(all_values):
        if param.key == "phase_chunks" and value == 2:
            return ("2 is the code minimum and the best measured value; "
                    "more chunks add join/wake and scheduling overhead.")
        if param.key == "phase_spin_us" and value == 0:
            return ("0 is the minimum; the main thread sleeps immediately "
                    "instead of spinning.")
        if param.key == "phase_threshold" and value == 2:
            return ("2 is the minimum; every window is eligible for fork-join.")
        return (f"optimum at the tested lower bound {value}; "
                f"extend the range downward only if the code permits it.")
    if value == max(all_values):
        return (f"optimum at the tested upper bound {value}; "
                f"extend the range upward and re-measure.")
    return (f"interior knee at {value}; apply_us={_fmt(apply_us, 1)} us "
            f"({delta:+.1f}% vs baseline).")


def _next_idea(param: Param, best: dict[str, Any] | None,
               values: list[int], workers: int) -> str:
    if best is None:
        return "increase samples or check that the game is running and logging."
    summary = best["summary"]
    value = best["value"]
    par = summary.get("par_runs") or 0
    seq = summary.get("seq_runs") or 0
    if param.kind == "threshold" and par == 0 and seq > 0:
        return ("decide explicitly whether the phase should fork-join this "
                "workload; if yes keep threshold=2, if no keep the higher "
                "threshold and re-run the structural knobs.")
    if param.kind == "safety":
        return ("run shadow with phase=0 phase_shadow=1 and require equal=1 "
                "mismatch=0 before testing values below 256.")
    if param.key == "phase_chunks" and value == 2:
        return ("2 is the code minimum; run a chunks x phase_main_last x "
                "phase_weighted grid to confirm the split interaction.")
    if param.key == "phase_spin_us" and value == 0:
        return ("0 is the minimum; re-check 0 vs 100 with more samples after "
                "the final chunks/main_last choice.")
    if param.key == "phase_threshold" and value == 2:
        return ("2 is the minimum; compare it with a sequential threshold "
                "separately before deciding whether to fork-join at all.")
    if param.key == "phase_main_last":
        if value == 1:
            return ("1 is the binary maximum (caller participates); compare "
                    "with chunks and weighted before keeping it.")
        return ("0 is the binary minimum (workers only); compare with chunks "
                "and weighted before keeping it.")
    if param.key == "phase_weighted":
        if value == 1:
            return ("1 is the binary maximum (weighted split); check whether "
                    "request cost varies before keeping it.")
        return ("0 is the binary minimum (equal split); re-check in a scene "
                "with uneven request cost.")
    all_values = sorted(param.values)
    if value == min(all_values):
        return f"test a wider lower range around {value} in the next scene."
    if value == max(all_values):
        return f"extend the range above {value} and re-measure."
    if param.key == "phase_chunks":
        return ("run a small chunks x phase_main_last x phase_weighted grid; "
                "the best chunk count depends on the split and main policy.")
    if param.key == "phase_spin_us":
        return ("re-measure spin after the final chunks/main_last choice; "
                "join time and CPU burn move together.")
    if param.key == "phase_main_last":
        return ("compare main_last with the final chunks count; main-last "
                "trades caller work for one fewer worker wake.")
    if param.key == "phase_weighted":
        return ("check whether request cost varies; a flat result means the "
                "equal split is sufficient.")
    return (f"refine the neighborhood around {value} with more samples; "
            "the two passes disagree if the note says so.")


def _refine(param: Param, best: dict[str, Any] | None, current: int,
            workers: int) -> list[int]:
    if best is None:
        return list(param.values)
    value = int(best["value"])
    if len(param.values) <= 2:
        return list(param.values)
    if param.key == "phase_chunks":
        candidates = {max(2, value - 1), value, min(workers, value + 1), current}
    else:
        ordered = sorted(param.values)
        nearest = sorted(ordered, key=lambda item: (abs(item - value), item))[:3]
        candidates = set(nearest)
        candidates.add(current)
    return sorted(candidates)


def _sweep(device: Device, args: argparse.Namespace, config_text: str,
           param: Param, values: list[int], current: dict[str, int],
           workers: int, pass_no: int) -> tuple[list[dict[str, Any]], str, list[str]]:
    rows: list[dict[str, Any]] = []
    raw_segments: list[str] = []
    for index, raw_value in enumerate(values, start=1):
        value = raw_value
        if param.key == "phase_chunks":
            value = min(max(2, value), workers)
        print(f"[pass {pass_no}] {param.key}={value} ({index}/{len(values)}) "
              f"settle={args.settle:.1f}s samples={args.samples}",
              file=sys.stderr)
        offset = device.size(args.log)
        marker = (f"all-{pass_no}-{param.key}-{index}-"
                  f"{_dt.datetime.now().strftime('%H%M%S')}")
        content = _with_key(config_text, param.key, value, marker)
        device.write_text(args.config, content)
        time.sleep(max(0.0, args.settle))
        time.sleep(max(0.0, args.samples * args.interval + 0.75))
        segment = device.read_tail(args.log, offset)
        raw_segments.append(segment)
        fatal = _fatal_marker(segment)
        reloads = _parse_reloads(segment)
        reload_ok, reload_reason = _reload_matches(
            reloads, param.key, value, workers)
        all_samples = _parse_phase_metrics(segment)
        valid = [sample for sample in all_samples
                 if _valid_sample(sample, param, value, workers, reload_ok)]
        summary = _summarize(valid)
        rows.append({
            "pass": pass_no,
            "value": value,
            "samples": valid,
            "all_samples": all_samples,
            "reloads": reloads,
            "summary": summary,
            "reload_ok": reload_ok,
            "reload_reason": reload_reason,
            "fatal": fatal,
        })
        print(f"    reload_ok={int(reload_ok)} "
              f"samples={len(all_samples)} valid={len(valid)} "
              f"chunks={_fmt(summary.get('chunks'), 2)} "
              f"apply_us={_fmt(summary.get('apply_us'), 1)} "
              f"run_us={_fmt(summary.get('run_us'), 1)} "
              f"seq_us={_fmt(summary.get('seq_us'), 1)}",
              file=sys.stderr)
        if not reload_ok:
            print(f"    warning: {reload_reason}", file=sys.stderr)
        if fatal:
            raise SweepError(f"fatal marker in {param.key}={value}: {fatal}")
        if not valid:
            print("    warning: no complete samples; game paused or log stale?",
                  file=sys.stderr)
    return rows, config_text, raw_segments


def _write_final(device: Device, args: argparse.Namespace,
                 config_text: str) -> None:
    device.write_text(args.config, config_text)
    time.sleep(max(0.0, args.settle))


def _save(args: argparse.Namespace, report: dict[str, Any],
          raw_segments: list[tuple[str, int, str]]) -> tuple[Path, Path, Path]:
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    json_path = out_dir / f"parallel_all_sweep_{stamp}.json"
    md_path = out_dir / f"parallel_all_sweep_{stamp}.md"
    raw_path = out_dir / f"parallel_all_sweep_{stamp}.log"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    raw_path.write_text("", encoding="utf-8")
    with raw_path.open("a", encoding="utf-8") as handle:
        for key, value, segment in raw_segments:
            handle.write(f"\n===== {key}={value} =====\n")
            handle.write(segment)
            if segment and not segment.endswith("\n"):
                handle.write("\n")
    md_path.write_text(_render_markdown(report), encoding="utf-8")
    return json_path, md_path, raw_path


def _render_markdown(report: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append("# Live parallel-phase coordinate sweep")
    lines.append("")
    lines.append(f"- Created: `{report['created']}`")
    lines.append(f"- Device: `{report['device']}`")
    lines.append(f"- Package: `{report['package']}`")
    lines.append(f"- Phase startup: `{report['phase_line']}`")
    lines.append(f"- Live reload: `{report.get('live_reload', {})}`")
    lines.append(f"- Workers: `{report['workers']}`")
    lines.append(f"- Samples/value: `{report['settings']['samples']}`, "
                 f"settle `{report['settings']['settle_s']}s`")
    lines.append(f"- Min improvement: "
                 f"`{report['settings']['min_improvement_pct']}%`, "
                 f"pass-2 consistency margin: "
                 f"`{report['settings']['consistency_margin_pct']}%`")
    lines.append(f"- Reset to baseline: "
                 f"`{report['settings']['reset']}`")
    lines.append("")
    lines.append("Selection metric: median `phase_apply_avg_us`; lower is "
                 "better. `apply_us` includes the fork-join/sequential run.")
    lines.append("")
    lines.append("## Final values")
    lines.append("")
    lines.append("| key | value | applied | decision | note |")
    lines.append("|---|---:|---|---|---|")
    for item in report["final"]:
        lines.append(f"| `{item['key']}` | `{item['value']}` | "
                     f"`{item['applied']}` | {item['decision']} | "
                     f"{item['note']} |")
    lines.append("")
    for pass_data in report["passes"]:
        lines.append(f"## Pass {pass_data['pass']}")
        lines.append("")
        for result in pass_data["results"]:
            lines.append(f"### `{result['key']}`")
            lines.append("")
            lines.append(result["description"])
            lines.append("")
            lines.append("```")
            lines.extend(result["table"])
            lines.append("```")
            lines.append("")
            lines.append(f"- Best: `{result['best_value']}`; "
                         f"current: `{result['current_value']}`; "
                         f"applied: `{result['applied']}`")
            lines.append(f"- Decision: {result['decision']}")
            lines.append(f"- Note: {result['note']}")
            lines.append(f"- Next: {result['next']}")
            lines.append("")
    lines.append("## Skipped")
    lines.append("")
    lines.append("| key | reason |")
    lines.append("|---|---|")
    for key, reason in report["skipped"]:
        lines.append(f"| `{key}` | {reason} |")
    lines.append("")
    lines.append("## Next ideas")
    lines.append("")
    for idea in report["next_ideas"]:
        lines.append(f"- {idea}")
    lines.append("")
    return "\n".join(lines)


def _build_next_ideas(report: dict[str, Any]) -> list[str]:
    ideas = [
        "Run shadow exactness with `phase=0 phase_shadow=1` and require "
        "`[phase-shadow] equal=1 mismatch=0` before changing product defaults.",
        "Repeat the same two passes in a second scene/foreground state; the "
        "optimum is workload-dependent.",
        "Increase samples per value for any knob whose 2% band overlaps the "
        "runner-up.",
        "Test interactions: `phase_chunks x phase_main_last x phase_weighted`, "
        "then re-tune `phase_spin_us`.",
    ]
    for item in report["final"]:
        if item["key"] == "phase_stack_zero":
            ideas.append("Do not lower `phase_stack_zero` below 256 without a "
                         "shadow equality run; it is a safety knob.")
        if item["key"] == "phase_threshold" and item.get("sequential"):
            ideas.append("`phase_threshold` preferred sequential fallback in "
                         "this workload; decide whether the phase should "
                         "fork-join at all, then re-run structural knobs.")
    return ideas


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Two-pass coordinate-descent tuner for live parallel "
                    "phase knobs.")
    parser.add_argument("--adb", help="adb executable")
    parser.add_argument("--serial", help="adb serial; auto-select if one device")
    parser.add_argument("--package", default=DEFAULT_PACKAGE)
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--log", default=DEFAULT_LOG)
    parser.add_argument("--passes", type=int, default=2,
                        help="number of passes over all knobs (default: 2)")
    parser.add_argument("--pass2", choices=("local", "full"), default="local",
                        help="pass 2 range: local refinement or full range")
    parser.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_S)
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_S)
    parser.add_argument("--min-samples", type=int, default=DEFAULT_MIN_SAMPLES)
    parser.add_argument("--min-improvement", type=float, default=2.0,
                        help="minimum median apply improvement, percent "
                             "(default: 2.0)")
    parser.add_argument("--consistency-margin", type=float, default=10.0,
                        help="pass-2 improvement needed to overturn a "
                             "different pass-1 best, percent (default: 10.0)")
    parser.add_argument("--reset", action="store_true",
                        help="write the baseline phase values before pass 1")
    parser.add_argument("--out-dir", default="reports/generated")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.passes < 1:
        parser.error("--passes must be >= 1")
    if args.samples < 1:
        parser.error("--samples must be >= 1")
    if args.interval <= 0:
        parser.error("--interval must be > 0")
    if args.min_samples < 1:
        parser.error("--min-samples must be >= 1")
    if args.min_improvement < 0:
        parser.error("--min-improvement must be >= 0")
    if args.consistency_margin < 0:
        parser.error("--consistency-margin must be >= 0")

    adb = _which_adb(args.adb)
    serial = _resolve_serial(adb, args.serial, args.verbose)
    args.serial = serial
    device = Device(adb, serial, args.package, args.verbose)
    original = device.read_text(args.config)
    log_text = device.read_text(args.log)
    state = _phase_state(log_text)
    print(f"device={serial} package={args.package}", file=sys.stderr)
    print(f"phase: {state['line']}", file=sys.stderr)
    if state["enabled"] != 1:
        raise SweepError("phase is disabled; set phase=1 in parallel.cfg first")
    if state["shadow"] != 0:
        raise SweepError("shadow mode is active; set phase_shadow=0 and "
                         "phase=1 before tuning the product path")
    workers = state["workers"] or 8
    phase_pairs = _parse_pairs(state["line"])
    reloads = _parse_reloads(log_text)
    live_reload = reloads[-1] if reloads else {}
    for field in ("chunks", "main_last", "weighted", "threshold",
                  "stack_zero", "spin_us"):
        if field in live_reload:
            phase_pairs[field] = str(live_reload[field])
    current = _current_values(phase_pairs, workers)
    params = _params(workers)
    print(f"workers={workers} current={current}", file=sys.stderr)

    if args.dry_run:
        for param in params:
            print(f"--- {param.key}: {param.values} "
                  f"(current {current.get(param.key)}) ---", file=sys.stderr)
            for value in param.values:
                print(_with_key(original, param.key, value, "dry-run"),
                      file=sys.stderr)
        for key, reason in SKIPPED:
            print(f"skip {key}: {reason}", file=sys.stderr)
        return 0

    config_text = original
    # Ensure the measured path is the live product path.  Threshold is reset
    # before the structural sweeps so every window is eligible for fork-join.
    if args.reset:
        for key, value in BASELINE.items():
            config_text = _with_key(config_text, key, value, "reset")
            current[key] = value
    config_text = _with_key(config_text, "phase", 1, "all-start")
    config_text = _with_key(config_text, "phase_shadow", 0, "all-start")
    config_text = _with_key(config_text, "phase_threshold", 2, "all-start")
    current["phase_threshold"] = 2
    _write_final(device, args, config_text)

    best_history: dict[str, dict[str, Any] | None] = {}
    report: dict[str, Any] = {
        "schema": 1,
        "created": _dt.datetime.now().isoformat(timespec="seconds"),
        "device": serial,
        "package": args.package,
        "config": args.config,
        "log": args.log,
        "phase_line": state["line"],
        "live_reload": live_reload,
        "workers": workers,
        "settings": {
            "passes": args.passes,
            "pass2": args.pass2,
            "samples": args.samples,
            "settle_s": args.settle,
            "interval_s": args.interval,
            "min_samples": args.min_samples,
            "min_improvement_pct": args.min_improvement,
            "consistency_margin_pct": args.consistency_margin,
            "reset": args.reset,
        },
        "passes": [],
        "final": [],
        "skipped": SKIPPED,
        "next_ideas": [],
    }
    raw_segments: list[tuple[str, int, str]] = []
    completed = False
    wrote = False
    try:
        wrote = True
        for pass_no in range(1, args.passes + 1):
            if pass_no > 1:
                # Structural sweeps need an eligible threshold.  The threshold
                # knob is measured last in every pass.
                config_text = _with_key(config_text, "phase_threshold", 2,
                                        f"pass{pass_no}-reset")
                current["phase_threshold"] = 2
                _write_final(device, args, config_text)
                wrote = True
            pass_data: dict[str, Any] = {"pass": pass_no, "results": []}
            for param in params:
                if pass_no == 1:
                    values = list(param.values)
                elif args.pass2 == "full":
                    values = list(param.values)
                else:
                    values = _refine(param, best_history.get(param.key),
                                     current.get(param.key, param.values[0]),
                                     workers)
                if param.key == "phase_chunks":
                    values = sorted({max(2, min(v, workers)) for v in values})
                current_value = current.get(param.key, param.values[0])
                if current_value not in values:
                    values.append(current_value)
                    values = sorted(set(values))
                rows, config_text, segments = _sweep(
                    device, args, config_text, param, values, current,
                    workers, pass_no)
                wrote = True
                raw_segments.extend((param.key, row["value"], segment)
                                    for row, segment in zip(rows, segments))
                best = _select_best(param, rows, args.min_samples)
                baseline = _baseline_row(param, rows, current_value)
                note = _note(param, best, baseline, current_value, values,
                             workers)
                next_idea = _next_idea(param, best, values, workers)
                history = best_history.get(param.key)
                apply, decision = _should_apply(
                    param, best, baseline, current_value, pass_no, history,
                    args.min_improvement, args.consistency_margin)
                chosen = int(best["value"]) if (apply and best) else current_value
                if apply:
                    config_text = _with_key(
                        config_text, param.key, chosen,
                        f"apply-p{pass_no}-{param.key}")
                    current[param.key] = chosen
                else:
                    config_text = _with_key(
                        config_text, param.key, current_value,
                        f"keep-p{pass_no}-{param.key}")
                    current[param.key] = current_value
                best_history[param.key] = best
                table = _table(f"pass {pass_no}: {param.key}",
                               param, rows)
                for line in table:
                    print(line)
                print(f"  note: {note}")
                print(f"  decision: {decision}")
                print(f"  next: {next_idea}")
                sequential = bool(
                    best is not None
                    and param.kind == "threshold"
                    and (best["summary"].get("par_runs") or 0) == 0
                    and (best["summary"].get("seq_runs") or 0) > 0)
                pass_data["results"].append({
                    "key": param.key,
                    "description": param.description,
                    "values": values,
                    "current_value": current_value,
                    "best_value": best["value"] if best else None,
                    "applied": apply,
                    "decision": decision,
                    "sequential": sequential,
                    "note": note,
                    "next": next_idea,
                    "table": table,
                    "rows": rows,
                    "summary": best["summary"] if best else None,
                })
            report["passes"].append(pass_data)

        last_results: dict[str, dict[str, Any]] = {}
        for pass_data in report["passes"]:
            for result in pass_data["results"]:
                last_results[result["key"]] = result
        final_rows = []
        for param in params:
            result = last_results.get(param.key)
            final_value = current.get(param.key)
            applied = bool(result and result.get("applied"))
            if param.kind == "safety":
                applied = False
            note = result["note"] if result else "not measured"
            decision = result.get("decision", "") if result else ""
            if result and not applied and decision:
                note = f"{note} Decision: {decision}."
            final_rows.append({
                "key": param.key,
                "value": final_value,
                "applied": applied,
                "sequential": bool(result and result.get("sequential")),
                "decision": decision,
                "note": note,
            })
        report["final"] = final_rows
        report["next_ideas"] = _build_next_ideas(report)
        _write_final(device, args, config_text)
        completed = True
        json_path, md_path, raw_path = _save(args, report, raw_segments)
        print(f"\nfinal config:\n{config_text}", file=sys.stderr)
        print(f"saved: {md_path}", file=sys.stderr)
        print(f"saved: {json_path}", file=sys.stderr)
        print(f"saved: {raw_path}", file=sys.stderr)
        return 0
    finally:
        if wrote and not completed:
            try:
                print("error path: restoring original parallel.cfg",
                      file=sys.stderr)
                _write_final(device, args, original)
            except Exception as exc:  # noqa: BLE001
                print(f"error path restore failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SweepError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        raise SystemExit(130)
