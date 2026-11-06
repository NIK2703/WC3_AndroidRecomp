#!/usr/bin/env python3
"""T1 difftest harness for the headless DUT.

The harness compares a headless run report with the frozen oracle identity from
`oracle/captures/1.27.1.7085`.  It does not execute the game and it never turns
missing evidence into `pass`:

    pass          identity, deterministic scenario, golden E3/E4 sets and every
                  V5.2 checkpoint agree, including V5.9 invariants
    fail          a present artifact or invariant disagrees
    unavailable   the oracle identity, normalization, DUT hashes, checkpoints
                  or golden E3/E4 sets are absent

Exit codes follow V5.5: 0 pass, 1 fail, 2 unavailable.  A report is written in
the V1.6 shape under `reports/generated/` by default.

Capture format
--------------
`identity.json` (oracle, read-only) has the V3.3 fields:

    {
      "reference": {...}, "scenario": {...},
      "baseline": {"checkpoints": [...]}, "layout": {...},
      "identity_sha256": "..."
    }

The baseline checkpoints and the DUT run report share one checkpoint schema:

    {
      "name": "boot",
      "markers": ["..."],
      "memory_hashes_normalized": {"war3.exe:0x0": "<hex>"},
      "cpu": {"eax": 0, "eip": 0, "esp": 0, "x87_top": 0, "xmm0": "<hex>"},
      "dispatch_targets": [0, ...],
      "d3d9_stream_sha256": "<hex>",
      "invariants": {
        "locked_range_written": "pass",
        "declared_range_consistency": "pass",
        "degenerate_primitive_ratio": "pass"
      },
      "save":  {"path": "save.bin",   "sha256": "<optional hex>"},
      "replay":{"path": "replay.bin", "sha256": "<optional hex>"}
    }

Checkpoint names must follow V5.2: boot, menu, map, 60s, save, load, exit.
Frame hashes are deliberately not compared in T1 (V4.3).

The run report has the V5.7 DUT hashes and `target: "headless"`:

    {
      "schema": 1, "corpus": "1.27b", "target": "headless",
      "dut": {"headless_sha256": "...", ...},
      "scenario": {...}, "identity_sha256": "...",
      "checkpoints": [...]
    }

V5.7 lists the six hashes as report fields, so the top-level spelling is also
accepted; a nested `dut` object takes precedence when both are present.

`scenario.golden` must mark both E3 and E4 golden sets as present before a pass
is possible.  `scenario.golden_sets: ["E3", "E4"]` is accepted as an equivalent
form.  A harness that has no real golden sets therefore reports `unavailable`,
not a reassuring green result.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ORACLE = ROOT / "oracle"
DEFAULT_REPORT = ROOT / "reports" / "generated" / "difftest.json"

CHECKPOINTS = ("boot", "menu", "map", "60s", "save", "load", "exit")
INVARIANTS = (
    "locked_range_written",
    "declared_range_consistency",
    "degenerate_primitive_ratio",
)
DUT_HASHES = (
    "headless_sha256",
    "platform_contract_sha256",
    "null_backend_sha256",
    "corpus_sha256",
    "registry_sha256",
)
HEX_DIGITS = set("0123456789abcdefABCDEF")
STATUS_EXIT = {"pass": 0, "fail": 1, "unavailable": 2}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def canonical_equal(left: Any, right: Any) -> bool:
    return canonical_bytes(left) == canonical_bytes(right)


def is_hex64(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(ch in HEX_DIGITS for ch in value)
    )


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def identity_digest(identity: dict[str, Any]) -> str:
    """V3.2 identity hash over the four frozen sections.

    DUT hashes are deliberately not part of this digest (V3.3); they are
    pinned separately in the run report (V5.7).
    """
    sections = {
        key: identity[key]
        for key in ("reference", "scenario", "baseline", "layout")
        if key in identity
    }
    return sha256_bytes(canonical_bytes(sections))


def validate_scenario(scenario: Any, where: str, errors: list[str]) -> None:
    if not isinstance(scenario, dict):
        errors.append(f"{where}: scenario is not an object")
        return
    if not is_int(scenario.get("seed")):
        errors.append(f"{where}: scenario.seed is not an integer")
    if not is_hex64(scenario.get("input_script_sha256")):
        errors.append(f"{where}: scenario.input_script_sha256 is not sha256")
    resolution = scenario.get("resolution")
    if (
        not isinstance(resolution, list)
        or len(resolution) != 2
        or not all(is_int(v) and v > 0 for v in resolution)
    ):
        errors.append(f"{where}: scenario.resolution is not [width,height]")
    if "caps_snapshot" not in scenario:
        errors.append(f"{where}: scenario.caps_snapshot is missing")
    golden = scenario.get("golden")
    if golden is not None:
        if not isinstance(golden, dict):
            errors.append(f"{where}: scenario.golden is not an object")
        else:
            for key in ("time", "fp"):
                if key in golden and not isinstance(golden[key], bool):
                    errors.append(f"{where}: scenario.golden.{key} is not bool")
    sets = scenario.get("golden_sets")
    if sets is not None and (
        not isinstance(sets, list) or not all(isinstance(v, str) for v in sets)
    ):
        errors.append(f"{where}: scenario.golden_sets is not a string list")


def validate_checkpoint(checkpoint: Any, where: str, errors: list[str]) -> None:
    if not isinstance(checkpoint, dict):
        errors.append(f"{where}: checkpoint is not an object")
        return
    name = checkpoint.get("name")
    if not isinstance(name, str) or not name:
        errors.append(f"{where}: checkpoint.name is missing")
    markers = checkpoint.get("markers")
    if markers is not None and (
        not isinstance(markers, list) or not all(isinstance(m, str) for m in markers)
    ):
        errors.append(f"{where}: checkpoint.markers is not a string list")
    memory = checkpoint.get("memory_hashes_normalized")
    if memory is not None:
        if not isinstance(memory, dict):
            errors.append(f"{where}: memory_hashes_normalized is not an object")
        else:
            for key, value in memory.items():
                if not isinstance(key, str) or not isinstance(value, str) or not value:
                    errors.append(
                        f"{where}: memory_hashes_normalized entry {key!r} is invalid"
                    )
    cpu = checkpoint.get("cpu")
    if cpu is not None and not isinstance(cpu, dict):
        errors.append(f"{where}: cpu is not an object")
    stream = checkpoint.get("d3d9_stream_sha256")
    if stream is not None and not is_hex64(stream):
        errors.append(f"{where}: d3d9_stream_sha256 is not sha256")
    invariants = checkpoint.get("invariants")
    if invariants is not None:
        if not isinstance(invariants, dict):
            errors.append(f"{where}: invariants is not an object")
        else:
            for key, value in invariants.items():
                if key not in INVARIANTS:
                    errors.append(f"{where}: unknown invariant {key!r}")
                if value not in ("pass", "fail", "unavailable"):
                    errors.append(f"{where}: invariant {key!r} has invalid status")
    for artifact in ("save", "replay"):
        spec = checkpoint.get(artifact)
        if spec is None:
            continue
        if not isinstance(spec, dict):
            errors.append(f"{where}: {artifact} is not an object")
            continue
        if not isinstance(spec.get("path"), str) or not spec["path"]:
            errors.append(f"{where}: {artifact}.path is missing")
        digest = spec.get("sha256")
        if digest is not None and not is_hex64(digest):
            errors.append(f"{where}: {artifact}.sha256 is not sha256")
    targets = checkpoint.get("dispatch_targets")
    if targets is not None and (
        not isinstance(targets, list) or not all(is_int(v) for v in targets)
    ):
        errors.append(f"{where}: dispatch_targets is not an integer list")


def checkpoint_names(checkpoints: Any, where: str, errors: list[str]) -> list[str]:
    if not isinstance(checkpoints, list):
        errors.append(f"{where}: checkpoints is not a list")
        return []
    names: list[str] = []
    for index, checkpoint in enumerate(checkpoints):
        validate_checkpoint(checkpoint, f"{where}.checkpoints[{index}]", errors)
        if isinstance(checkpoint, dict) and isinstance(checkpoint.get("name"), str):
            names.append(checkpoint["name"])
    if len(names) != len(set(names)):
        errors.append(f"{where}: duplicate checkpoint names")
    return names


def validate_identity(identity: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(identity, dict):
        return ["identity.json is not an object"]
    for key in ("reference", "scenario", "baseline", "layout"):
        if key not in identity:
            errors.append(f"identity.json: {key} is missing")
    reference = identity.get("reference")
    if not isinstance(reference, dict):
        errors.append("identity.json: reference is not an object")
    else:
        if reference.get("game_version") != "1.27.1.7085":
            errors.append("identity.json: reference.game_version is not 1.27.1.7085")
        pe_hashes = reference.get("pe_sha256")
        if (
            not isinstance(pe_hashes, list)
            or not pe_hashes
            or not all(is_hex64(v) for v in pe_hashes)
        ):
            errors.append("identity.json: reference.pe_sha256 is not a sha256 list")
        if not is_hex64(reference.get("capture_tool_sha256")):
            errors.append("identity.json: reference.capture_tool_sha256 is not sha256")
        if not isinstance(reference.get("captured_at"), str) or not reference["captured_at"]:
            errors.append("identity.json: reference.captured_at is missing")
    validate_scenario(identity.get("scenario"), "identity.json", errors)
    baseline = identity.get("baseline")
    if not isinstance(baseline, dict):
        errors.append("identity.json: baseline is not an object")
    else:
        names = checkpoint_names(baseline.get("checkpoints"), "identity.json.baseline", errors)
        for required in CHECKPOINTS:
            if required not in names:
                errors.append(f"identity.json: baseline checkpoint {required!r} is missing")
    layout = identity.get("layout")
    if not isinstance(layout, dict):
        errors.append("identity.json: layout is not an object")
    else:
        if not is_hex64(layout.get("sha256")):
            errors.append("identity.json: layout.sha256 is not sha256")
        if not is_int(layout.get("lift_rev")):
            errors.append("identity.json: layout.lift_rev is not an integer")
        modules = layout.get("rebased_modules")
        if not isinstance(modules, list) or not all(isinstance(v, str) for v in modules):
            errors.append("identity.json: layout.rebased_modules is not a string list")
    declared = identity.get("identity_sha256")
    if not is_hex64(declared):
        errors.append("identity.json: identity_sha256 is not sha256")
    elif declared != identity_digest(identity):
        errors.append("identity.json: identity_sha256 does not match its sections")
    return errors


def validate_run(run: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(run, dict):
        return ["run report is not an object"]
    if run.get("schema") != 1:
        errors.append("run: schema is not 1")
    if run.get("corpus") != "1.27b":
        errors.append("run: corpus is not 1.27b")
    if run.get("target") != "headless":
        errors.append("run: target is not headless")
    dut = run.get("dut")
    if dut is not None and not isinstance(dut, dict):
        errors.append("run: dut hashes are not an object")
        dut = None
    hashes: dict[str, Any] = {}
    hashes.update({key: run.get(key) for key in DUT_HASHES})
    if isinstance(dut, dict):
        hashes.update(dut)
    for key in DUT_HASHES:
        if not is_hex64(hashes.get(key)):
            errors.append(f"run: dut hash {key} is not sha256")
    validate_scenario(run.get("scenario"), "run", errors)
    checkpoint_names(run.get("checkpoints"), "run", errors)
    if not is_hex64(run.get("identity_sha256")):
        errors.append("run: identity_sha256 is not sha256")
    return errors


def golden_missing(scenario: dict[str, Any]) -> list[str]:
    """Return missing E3/E4 golden-set descriptions.

    V5.6/E11: without both golden sets a matching trace is `unavailable`, not
    `pass`, even when every byte in the capture agrees.
    """
    time_ok = False
    fp_ok = False
    golden = scenario.get("golden")
    if isinstance(golden, dict):
        time_ok = golden.get("time") is True
        fp_ok = golden.get("fp") is True
    sets = scenario.get("golden_sets")
    if isinstance(sets, list):
        for item in sets:
            if not isinstance(item, str):
                continue
            if item == "E3" or item.startswith("E3."):
                time_ok = True
            if item == "E4" or item.startswith("E4."):
                fp_ok = True
    missing: list[str] = []
    if not time_ok:
        missing.append("E3 golden time set is absent")
    if not fp_ok:
        missing.append("E4 golden FP set is absent")
    return missing


def scenario_disagreements(
    oracle: dict[str, Any], run: dict[str, Any]
) -> tuple[list[str], list[str]]:
    fails: list[str] = []
    unavailable: list[str] = []
    for key in ("seed", "input_script_sha256", "resolution", "caps_snapshot"):
        if not canonical_equal(oracle.get(key), run.get(key)):
            fails.append(f"scenario.{key} differs from the oracle")
    oracle_schedule = oracle.get("schedule_sha256")
    run_schedule = run.get("schedule_sha256")
    if oracle_schedule is not None:
        if run_schedule is None:
            unavailable.append("run has no schedule_sha256 for the fixed scenario")
        elif not canonical_equal(oracle_schedule, run_schedule):
            fails.append("scenario.schedule_sha256 differs from the oracle")
    elif run_schedule is not None:
        unavailable.append("oracle has no schedule_sha256 to pin the run schedule")
    return fails, unavailable


def resolve_artifact(base: Path, relative: str, label: str) -> Path:
    base_resolved = base.resolve()
    candidate = (base / relative).resolve()
    try:
        candidate.relative_to(base_resolved)
    except ValueError:
        raise ValueError(f"{label}: artifact path escapes its capture directory")
    return candidate


def compare_file_bytes(left: Path, right: Path) -> tuple[bool, int | None, int, int]:
    """Compare two files byte for byte without loading them both into memory."""
    left_size = left.stat().st_size
    right_size = right.stat().st_size
    limit = min(left_size, right_size)
    offset = 0
    with left.open("rb") as lf, right.open("rb") as rf:
        while offset < limit:
            chunk = min(1 << 20, limit - offset)
            lb = lf.read(chunk)
            rb = rf.read(chunk)
            if lb != rb:
                for index, (a, b) in enumerate(zip(lb, rb)):
                    if a != b:
                        return False, offset + index, left_size, right_size
                return False, offset + len(lb), left_size, right_size
            offset += len(lb)
    if left_size != right_size:
        return False, limit, left_size, right_size
    return True, None, left_size, right_size


def compare_artifact(
    label: str,
    oracle_spec: Any,
    run_spec: Any,
    oracle_dir: Path,
    run_dir: Path,
) -> dict[str, Any]:
    if not isinstance(oracle_spec, dict) or not isinstance(run_spec, dict):
        return {"status": "unavailable", "reason": f"{label}: artifact is missing"}
    failures: list[str] = []
    unavailable: list[str] = []
    diagnostics: dict[str, Any] = {}
    paths: list[Path] = []
    actual_hashes: list[str] = []
    sizes: list[int] = []
    for side, spec, base in (
        ("oracle", oracle_spec, oracle_dir),
        ("dut", run_spec, run_dir),
    ):
        relative = spec.get("path")
        if not isinstance(relative, str) or not relative:
            unavailable.append(f"{label}: {side} path is missing")
            continue
        try:
            path = resolve_artifact(base, relative, f"{label} {side}")
        except ValueError as exc:
            failures.append(str(exc))
            continue
        if not path.is_file():
            unavailable.append(f"{label}: {side} file is missing: {relative}")
            continue
        actual = sha256_file(path)
        paths.append(path)
        actual_hashes.append(actual)
        sizes.append(path.stat().st_size)
        declared = spec.get("sha256")
        diagnostics.setdefault(label, {})[f"{side}_sha256"] = actual
        diagnostics[label][f"{side}_bytes"] = sizes[-1]
        if declared is not None:
            diagnostics[label][f"{side}_declared_sha256"] = declared
            if declared != actual:
                failures.append(f"{label}: {side} declared sha256 does not match file")
    if len(paths) == 2:
        equal, offset, left_size, right_size = compare_file_bytes(paths[0], paths[1])
        diagnostics[label]["first_diff_offset"] = offset
        if actual_hashes[0] != actual_hashes[1]:
            failures.append(f"{label}: file content differs (sha256)")
        if not equal:
            failures.append(
                f"{label}: byte mismatch at offset {offset} "
                f"({left_size} vs {right_size} bytes)"
            )
    if failures:
        return {"status": "fail", "reasons": failures, "diagnostics": diagnostics}
    if unavailable:
        return {
            "status": "unavailable",
            "reasons": unavailable,
            "diagnostics": diagnostics,
        }
    return {"status": "pass", "reasons": [], "diagnostics": diagnostics}


def compare_checkpoint(
    oracle: dict[str, Any],
    run: dict[str, Any],
    oracle_dir: Path,
    run_dir: Path,
) -> dict[str, Any]:
    name = oracle.get("name", "?")
    failures: list[str] = []
    unavailable: list[str] = []
    diagnostics: dict[str, Any] = {}

    oracle_markers = oracle.get("markers")
    run_markers = run.get("markers")
    if not isinstance(oracle_markers, list) or not isinstance(run_markers, list):
        unavailable.append(f"{name}: markers are missing")
    elif not oracle_markers or not run_markers:
        unavailable.append(f"{name}: markers are empty")
    elif oracle_markers != run_markers:
        failures.append(f"{name}: marker stream differs")
        diagnostics["markers"] = {"oracle": oracle_markers, "dut": run_markers}

    oracle_memory = oracle.get("memory_hashes_normalized")
    run_memory = run.get("memory_hashes_normalized")
    if not isinstance(oracle_memory, dict) or not isinstance(run_memory, dict):
        unavailable.append(f"{name}: normalized guest-memory hashes are missing")
    elif not oracle_memory or not run_memory:
        unavailable.append(f"{name}: normalized guest-memory hashes are empty")
    else:
        memory_diagnostics: dict[str, Any] = {}
        for key, value in oracle_memory.items():
            if key not in run_memory:
                unavailable.append(f"{name}: memory region {key} is missing")
                memory_diagnostics[key] = {"oracle": value, "dut": None}
            elif run_memory[key] != value:
                failures.append(f"{name}: memory region {key} differs")
                memory_diagnostics[key] = {
                    "oracle": value,
                    "dut": run_memory[key],
                }
        for key in run_memory:
            if key not in oracle_memory:
                failures.append(f"{name}: unexpected memory region {key}")
                memory_diagnostics[key] = {"oracle": None, "dut": run_memory[key]}
        if memory_diagnostics:
            diagnostics["memory_hashes_normalized"] = memory_diagnostics

    oracle_cpu = oracle.get("cpu")
    run_cpu = run.get("cpu")
    if not isinstance(oracle_cpu, dict) or not isinstance(run_cpu, dict):
        unavailable.append(f"{name}: CPU snapshot is missing")
    elif not oracle_cpu or not run_cpu:
        unavailable.append(f"{name}: CPU snapshot is empty")
    elif not canonical_equal(oracle_cpu, run_cpu):
        failures.append(f"{name}: CPU snapshot differs")
        diagnostics["cpu"] = {"oracle": oracle_cpu, "dut": run_cpu}

    oracle_stream = oracle.get("d3d9_stream_sha256")
    run_stream = run.get("d3d9_stream_sha256")
    if not is_hex64(oracle_stream) or not is_hex64(run_stream):
        unavailable.append(f"{name}: canonical D3D9 stream hash is missing")
    elif oracle_stream != run_stream:
        failures.append(f"{name}: canonical D3D9 stream differs")
        diagnostics["d3d9_stream_sha256"] = {
            "oracle": oracle_stream,
            "dut": run_stream,
        }

    oracle_invariants = oracle.get("invariants")
    run_invariants = run.get("invariants")
    if not isinstance(oracle_invariants, dict) or not isinstance(run_invariants, dict):
        unavailable.append(f"{name}: V5.9 invariants are missing")
    else:
        for invariant in INVARIANTS:
            oracle_value = oracle_invariants.get(invariant)
            run_value = run_invariants.get(invariant)
            if oracle_value is None or run_value is None:
                unavailable.append(f"{name}: invariant {invariant} is missing")
            elif "fail" in (oracle_value, run_value):
                failures.append(f"{name}: invariant {invariant} failed")
            elif "unavailable" in (oracle_value, run_value):
                unavailable.append(f"{name}: invariant {invariant} is unavailable")
        diagnostics["invariants"] = {
            key: {
                "oracle": oracle_invariants.get(key),
                "dut": run_invariants.get(key),
            }
            for key in INVARIANTS
        }

    if name in ("save", "load", "exit"):
        if oracle.get("save") is None and oracle.get("replay") is None:
            unavailable.append(f"{name}: baseline has no save/replay artifact")
    for label in ("save", "replay"):
        oracle_spec = oracle.get(label)
        run_spec = run.get(label)
        if oracle_spec is None and run_spec is None:
            continue
        if oracle_spec is None or run_spec is None:
            unavailable.append(f"{name}: {label} artifact exists on only one side")
            continue
        result = compare_artifact(label, oracle_spec, run_spec, oracle_dir, run_dir)
        failures.extend(result.get("reasons", []) if result["status"] == "fail" else [])
        if result["status"] == "unavailable":
            unavailable.extend(result.get("reasons", []))
        diagnostics.update(result.get("diagnostics", {}))

    oracle_targets = oracle.get("dispatch_targets")
    run_targets = run.get("dispatch_targets")
    if isinstance(oracle_targets, list) or isinstance(run_targets, list):
        diagnostics["dispatch_targets"] = {
            "oracle": oracle_targets,
            "dut": run_targets,
            "equal": oracle_targets == run_targets,
        }

    status = "fail" if failures else "unavailable" if unavailable else "pass"
    return {
        "name": name,
        "status": status,
        "reasons": failures + unavailable,
        "diagnostics": diagnostics,
    }


def by_name(checkpoints: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {
        checkpoint["name"]: checkpoint
        for checkpoint in checkpoints
        if isinstance(checkpoint, dict) and isinstance(checkpoint.get("name"), str)
    }


def load_json(path: Path, label: str) -> tuple[Any | None, str | None, bool]:
    """Return (document, error, missing)."""
    if not path.is_file():
        return None, f"{label} is missing: {path}", True
    try:
        return json.loads(path.read_text(encoding="utf-8")), None, False
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return None, f"{label} is invalid JSON: {exc}", False


def write_report(
    report_path: Path,
    status: str,
    reasons: list[str],
    checkpoints: list[dict[str, Any]],
    identity_hash: str | None,
    run_hash: str,
    artifact_path: str,
    quiet: bool,
) -> bool:
    code = STATUS_EXIT[status]
    report = {
        "schema": 1,
        "gate_id": "G-09",
        "command": " ".join(["python", "tools/difftest.py", *sys.argv[1:]]),
        "status": status,
        "exit_code": code,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "artifact_sha256": run_hash,
        "artifact_path": artifact_path,
        "identity_sha256": identity_hash,
        "reasons": reasons,
        "checkpoints": checkpoints,
        "artifacts": {
            checkpoint["name"]: checkpoint.get("diagnostics", {})
            for checkpoint in checkpoints
            if checkpoint.get("status") == "fail" and checkpoint.get("diagnostics")
        },
        "diagnostics": {
            "frame_hashes": "not compared in T1 (V4.3)",
            "verdict_owner": "docs/EXACTNESS.md E11",
        },
    }
    try:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    except OSError as exc:
        if not quiet:
            print(f"difftest: cannot write report {report_path}: {exc}", file=sys.stderr)
        return False

    if not quiet:
        print(
            f"difftest: {status} "
            f"(identity {identity_hash or 'missing'}, run {artifact_path or 'missing'})"
        )
        for reason in reasons:
            print(f"  {reason}", file=sys.stderr if status != "pass" else sys.stdout)
        if status != "pass":
            print(f"difftest: report {report_path}")
    return True


def run_compare(
    oracle_dir: Path,
    run_path: Path,
    report_path: Path,
    quiet: bool = False,
) -> tuple[int, dict[str, Any]]:
    reasons: list[str] = []
    checkpoint_results: list[dict[str, Any]] = []
    identity_hash: str | None = None
    run_hash = ""
    if run_path.is_file():
        run_hash = sha256_file(run_path)

    identity_path = oracle_dir / "identity.json"
    normalization_path = oracle_dir / "normalization.json"
    identity_doc, identity_error, identity_missing = load_json(
        identity_path, "oracle identity.json"
    )
    run_doc, run_error, run_missing = load_json(run_path, "DUT run report")
    normalization_doc, normalization_error, normalization_missing = load_json(
        normalization_path, "oracle normalization.json"
    )

    if identity_missing:
        reasons.append(identity_error or "oracle identity.json is missing")
    if normalization_missing:
        reasons.append(normalization_error or "oracle normalization.json is missing")
    if run_missing:
        reasons.append(run_error or "DUT run report is missing")

    if identity_error and not identity_missing:
        reasons.append(identity_error)
    if normalization_error and not normalization_missing:
        reasons.append(normalization_error)
    if run_error and not run_missing:
        reasons.append(run_error)

    identity_invalid = False
    run_invalid = False
    normalization_invalid = False
    if identity_doc is not None:
        validation = validate_identity(identity_doc)
        if validation:
            reasons.extend(validation)
            identity_doc = None
            identity_invalid = True
    if run_doc is not None:
        validation = validate_run(run_doc)
        if validation:
            reasons.extend(validation)
            run_doc = None
            run_invalid = True

    if identity_doc is not None:
        identity_hash = identity_doc.get("identity_sha256")
    if normalization_doc is not None and not isinstance(normalization_doc, dict):
        reasons.append("oracle normalization.json is not an object")
        normalization_doc = None
        normalization_invalid = True

    if identity_doc is None or run_doc is None:
        # A malformed artifact that is present is a failure.  A missing
        # artifact is unavailable; it is never converted into a pass.
        hard_fail = bool(
            identity_invalid
            or run_invalid
            or normalization_invalid
            or (identity_error and not identity_missing)
            or (normalization_error and not normalization_missing)
            or (run_error and not run_missing)
        )
        status = "fail" if hard_fail else "unavailable"
        if not reasons:
            reasons.append("difftest evidence is incomplete")
        ok = write_report(
            report_path,
            status,
            reasons,
            checkpoint_results,
            identity_hash,
            run_hash,
            str(run_path),
            quiet,
        )
        return (STATUS_EXIT[status] if ok else 1), {
            "status": status,
            "reasons": reasons,
            "checkpoints": checkpoint_results,
        }

    if run_doc["identity_sha256"] != identity_doc["identity_sha256"]:
        reasons.append("run identity_sha256 differs from the oracle identity")
        scenario_fails = ["run identity_sha256 differs from the oracle identity"]
    else:
        scenario_fails = []

    extra_scenario_fails, scenario_unavailable = scenario_disagreements(
        identity_doc["scenario"], run_doc["scenario"]
    )
    scenario_fails.extend(extra_scenario_fails)
    reasons.extend(extra_scenario_fails)
    reasons.extend(scenario_unavailable)

    oracle_checkpoints = by_name(identity_doc["baseline"]["checkpoints"])
    run_checkpoints = by_name(run_doc["checkpoints"])
    run_order = [
        checkpoint["name"]
        for checkpoint in run_doc["checkpoints"]
        if isinstance(checkpoint, dict) and checkpoint.get("name") in oracle_checkpoints
    ]
    if run_order != [name for name in CHECKPOINTS if name in run_order]:
        reasons.append("run checkpoint order differs from V5.2")
        scenario_fails.append("run checkpoint order differs from V5.2")
    for name in CHECKPOINTS:
        oracle_checkpoint = oracle_checkpoints.get(name)
        run_checkpoint = run_checkpoints.get(name)
        if oracle_checkpoint is None or run_checkpoint is None:
            checkpoint_results.append(
                {
                    "name": name,
                    "status": "unavailable",
                    "reasons": [f"{name}: checkpoint is missing"],
                    "diagnostics": {},
                }
            )
            continue
        checkpoint_results.append(
            compare_checkpoint(
                oracle_checkpoint,
                run_checkpoint,
                oracle_dir,
                run_path.parent,
            )
        )

    normalization_invalid = bool(
        normalization_invalid or (normalization_error and not normalization_missing)
    )
    if any(result["status"] == "fail" for result in checkpoint_results):
        status = "fail"
    elif scenario_fails or normalization_invalid:
        status = "fail"
    else:
        missing_golden = golden_missing(identity_doc["scenario"])
        if missing_golden:
            reasons.extend(missing_golden)
            status = "unavailable"
        elif normalization_missing or scenario_unavailable or any(
            result["status"] == "unavailable" for result in checkpoint_results
        ):
            status = "unavailable"
        else:
            status = "pass"

    for result in checkpoint_results:
        reasons.extend(result.get("reasons", []))
    if status == "pass":
        reasons = []
    elif not reasons:
        reasons.append("difftest did not produce a complete verdict")

    ok = write_report(
        report_path,
        status,
        reasons,
        checkpoint_results,
        identity_hash,
        run_hash,
        str(run_path),
        quiet,
    )
    return (STATUS_EXIT[status] if ok else 1), {
        "status": status,
        "reasons": reasons,
        "checkpoints": checkpoint_results,
    }


def _fixture(
    base: Path,
    *,
    golden: bool = True,
    missing_identity: bool = False,
    bad_run_json: bool = False,
    stream_mut: bool = False,
    marker_mut: bool = False,
    invariant_fail: bool = False,
    save_shift: bool = False,
) -> tuple[Path, Path, Path]:
    oracle = base / "oracle"
    run_dir = base / "run"
    oracle.mkdir(parents=True)
    run_dir.mkdir(parents=True)
    report = base / "report.json"

    save_bytes = b"WC3SAVE\x00\x01\x02\x03" + bytes(range(64))
    replay_bytes = b"WC3REPLAY\x00\x04\x05" + bytes(range(32))
    (oracle / "save.bin").write_bytes(save_bytes)
    (run_dir / "save.bin").write_bytes(save_bytes)
    (oracle / "replay.bin").write_bytes(replay_bytes)
    (run_dir / "replay.bin").write_bytes(replay_bytes)
    if save_shift:
        shifted = bytearray(save_bytes)
        shifted[-1] ^= 0x01
        (run_dir / "save.bin").write_bytes(bytes(shifted))

    def checkpoint(name: str, save: bool = False, replay: bool = False) -> dict[str, Any]:
        item: dict[str, Any] = {
            "name": name,
            "markers": [f"capture:{name}"],
            "memory_hashes_normalized": {"war3.exe:0x00000000": "aa" * 32},
            "cpu": {
                "eax": 0x12345678,
                "eip": 0x6F123456,
                "esp": 0x0019FF00,
                "x87_top": 0,
                "xmm0": "00" * 16,
            },
            "d3d9_stream_sha256": "ab" * 32,
            "invariants": {key: "pass" for key in INVARIANTS},
            "dispatch_targets": [0x6F123456, 0x00401000],
        }
        if save:
            item["save"] = {"path": "save.bin"}
        if replay:
            item["replay"] = {"path": "replay.bin"}
        return item

    baseline_checkpoints = [checkpoint(name) for name in CHECKPOINTS]
    for item in baseline_checkpoints:
        if item["name"] == "save":
            item["save"] = {"path": "save.bin"}
        elif item["name"] == "load":
            item["save"] = {"path": "save.bin"}
            item["replay"] = {"path": "replay.bin"}
        elif item["name"] == "exit":
            item["replay"] = {"path": "replay.bin"}
    run_checkpoints = copy.deepcopy(baseline_checkpoints)
    if stream_mut:
        run_checkpoints[2]["d3d9_stream_sha256"] = "ff" * 32
    if marker_mut:
        run_checkpoints[1]["markers"].append("unexpected-marker")
    if invariant_fail:
        run_checkpoints[3]["invariants"]["locked_range_written"] = "fail"

    scenario: dict[str, Any] = {
        "seed": 7,
        "input_script_sha256": "11" * 32,
        "resolution": [1024, 768],
        "caps_snapshot": {"max_texture": 2048, "vertex_shader": "3_0"},
        "schedule_sha256": "22" * 32,
        "golden": {"time": golden, "fp": True},
    }
    identity: dict[str, Any] = {
        "reference": {
            "game_version": "1.27.1.7085",
            "pe_sha256": ["33" * 32],
            "capture_tool_sha256": "44" * 32,
            "captured_at": "2026-11-07T00:00:00Z",
        },
        "scenario": scenario,
        "baseline": {
            "checkpoints": baseline_checkpoints,
            "frame_hashes": ["fixture-frame"],
            "markers": ["fixture-marker"],
            "memory_hashes_normalized": ["fixture-memory"],
            "save_hashes": ["fixture-save"],
            "replay_hashes": ["fixture-replay"],
        },
        "layout": {
            "sha256": "55" * 32,
            "lift_rev": 2,
            "rebased_modules": ["Storm.dll"],
        },
    }
    identity["identity_sha256"] = identity_digest(identity)
    run: dict[str, Any] = {
        "schema": 1,
        "corpus": "1.27b",
        "target": "headless",
        "dut": {
            key: hashlib.sha256(f"fixture:{key}".encode()).hexdigest()
            for key in DUT_HASHES
        },
        "scenario": copy.deepcopy(scenario),
        "identity_sha256": identity["identity_sha256"],
        "checkpoints": run_checkpoints,
    }
    (oracle / "normalization.json").write_text(
        json.dumps({"schema": 1, "exceptions": []}, indent=2) + "\n",
        encoding="utf-8",
    )
    if not missing_identity:
        (oracle / "identity.json").write_text(
            json.dumps(identity, indent=2) + "\n", encoding="utf-8"
        )
    run_path = run_dir / "run.json"
    if bad_run_json:
        run_path.write_text("{", encoding="utf-8")
    else:
        run_path.write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
    return oracle, run_path, report


def self_test() -> int:
    failures = 0
    cases = 0

    def expect(name: str, actual: int, expected: int) -> None:
        nonlocal failures, cases
        cases += 1
        if actual != expected:
            print(f"FAIL {name}: exit {actual}, expected {expected}")
            failures += 1

    with tempfile.TemporaryDirectory(prefix="wc3_difftest_") as tmp:
        root = Path(tmp)

        oracle, run, report = _fixture(root / "valid")
        rc, result = run_compare(oracle, run, report, quiet=True)
        expect("valid capture", rc, 0)
        if result["status"] != "pass":
            print(f"FAIL valid status: {result['status']} {result['reasons']}")
            failures += 1
        else:
            cases += 1

        oracle, run, report = _fixture(root / "stream", stream_mut=True)
        expect("corrupted D3D9 stream", run_compare(oracle, run, report, quiet=True)[0], 1)

        oracle, run, report = _fixture(root / "save", save_shift=True)
        expect("shifted save byte", run_compare(oracle, run, report, quiet=True)[0], 1)

        oracle, run, report = _fixture(root / "marker", marker_mut=True)
        expect("changed marker", run_compare(oracle, run, report, quiet=True)[0], 1)

        oracle, run, report = _fixture(root / "invariant", invariant_fail=True)
        expect("failed V5.9 invariant", run_compare(oracle, run, report, quiet=True)[0], 1)

        oracle, run, report = _fixture(root / "golden", golden=False)
        expect("missing E3 golden set", run_compare(oracle, run, report, quiet=True)[0], 2)

        oracle, run, report = _fixture(root / "identity", missing_identity=True)
        expect("missing oracle identity", run_compare(oracle, run, report, quiet=True)[0], 2)

        oracle, run, report = _fixture(root / "bad-json", bad_run_json=True)
        expect("malformed run report", run_compare(oracle, run, report, quiet=True)[0], 1)

        oracle, run, report = _fixture(root / "missing-normalization")
        (oracle / "normalization.json").unlink()
        expect(
            "missing normalization",
            run_compare(oracle, run, report, quiet=True)[0],
            2,
        )

        oracle, run, report = _fixture(root / "bad-normalization")
        (oracle / "normalization.json").write_text("{", encoding="utf-8")
        expect(
            "malformed normalization",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        oracle, run, report = _fixture(root / "bad-identity")
        identity_path = oracle / "identity.json"
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        del identity["layout"]
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        expect(
            "malformed identity",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        oracle, run, report = _fixture(root / "identity-mismatch")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["identity_sha256"] = "00" * 32
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "identity hash mismatch",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        oracle, run, report = _fixture(root / "missing-checkpoint")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["checkpoints"] = run_data["checkpoints"][:-1]
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "missing checkpoint",
            run_compare(oracle, run, report, quiet=True)[0],
            2,
        )

        oracle, run, report = _fixture(root / "frame-hash")
        identity_path = oracle / "identity.json"
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["baseline"]["frame_hashes"] = ["different-frame"]
        identity["identity_sha256"] = identity_digest(identity)
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["identity_sha256"] = identity["identity_sha256"]
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "frame hash is not a T1 gate",
            run_compare(oracle, run, report, quiet=True)[0],
            0,
        )

        oracle, run, report = _fixture(root / "top-level-dut")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        dut = run_data.pop("dut")
        run_data.update(dut)
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "top-level V5.7 hashes",
            run_compare(oracle, run, report, quiet=True)[0],
            0,
        )

        oracle, run, report = _fixture(root / "golden-sets")
        identity_path = oracle / "identity.json"
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["scenario"].pop("golden", None)
        identity["scenario"]["golden_sets"] = ["E3", "E4"]
        identity["identity_sha256"] = identity_digest(identity)
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["scenario"].pop("golden", None)
        run_data["scenario"]["golden_sets"] = ["E3", "E4"]
        run_data["identity_sha256"] = identity["identity_sha256"]
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "golden sets E3/E4",
            run_compare(oracle, run, report, quiet=True)[0],
            0,
        )

        oracle, run, report = _fixture(root / "baseline-invariant")
        identity_path = oracle / "identity.json"
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
        identity["baseline"]["checkpoints"][0]["invariants"][
            "locked_range_written"
        ] = "unavailable"
        identity["identity_sha256"] = identity_digest(identity)
        identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["identity_sha256"] = identity["identity_sha256"]
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "baseline invariant unavailable",
            run_compare(oracle, run, report, quiet=True)[0],
            2,
        )

        oracle, run, report = _fixture(root / "cpu-artifact")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["checkpoints"][0]["cpu"]["eax"] ^= 1
        run_data["checkpoints"][0]["dispatch_targets"][-1] = 0xDEADBEEF
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        rc, result = run_compare(oracle, run, report, quiet=True)
        expect("CPU mismatch", rc, 1)
        report_data = json.loads(report.read_text(encoding="utf-8"))
        if (
            "boot" in report_data.get("artifacts", {})
            and "cpu" in report_data["artifacts"]["boot"]
            and "dispatch_targets" in result["checkpoints"][0]["diagnostics"]
        ):
            cases += 1
        else:
            print("FAIL CPU/dispatch artifacts were not recorded")
            failures += 1

        oracle, run, report = _fixture(root / "schedule")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["scenario"]["schedule_sha256"] = "ff" * 32
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "schedule mismatch",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        oracle, run, report = _fixture(root / "traversal")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        for checkpoint in run_data["checkpoints"]:
            if checkpoint["name"] == "save":
                checkpoint["save"]["path"] = "../save.bin"
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "artifact path traversal",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        oracle, run, report = _fixture(root / "windows-target")
        run_data = json.loads(run.read_text(encoding="utf-8"))
        run_data["target"] = "windows"
        run.write_text(json.dumps(run_data, indent=2) + "\n", encoding="utf-8")
        expect(
            "windows target rejected",
            run_compare(oracle, run, report, quiet=True)[0],
            1,
        )

        left = root / "left.bin"
        right = root / "right.bin"
        left.write_bytes(b"same-bytes")
        right.write_bytes(b"same-bytes")
        equal, offset, _, _ = compare_file_bytes(left, right)
        expect("byte compare equal", 0 if equal and offset is None else 1, 0)
        right.write_bytes(b"same-bytez")
        equal, offset, _, _ = compare_file_bytes(left, right)
        expect("byte compare mismatch", 0 if not equal and offset == 9 else 1, 0)

    if failures:
        print(f"difftest self-test: {failures} failure(s)")
        return 1
    print(f"difftest self-test: OK ({cases} checks)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    compare = sub.add_parser("compare", help="compare a headless run with the oracle")
    compare.add_argument("--oracle", default=str(DEFAULT_ORACLE))
    compare.add_argument("--run", required=True)
    compare.add_argument("--report", default=str(DEFAULT_REPORT))
    sub.add_parser("self-test", help="run the V5.4 harness self-test")
    args = parser.parse_args(argv)
    if args.command == "self-test":
        return self_test()
    rc, _ = run_compare(
        Path(args.oracle),
        Path(args.run),
        Path(args.report),
        quiet=False,
    )
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
