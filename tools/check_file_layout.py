#!/usr/bin/env python3
"""Enforce the file-structure rules R1-R22 from docs/FILE_LAYOUT.md.

`--strict` fails on hard violations.  Known migration debt (missing schema/
dir, tool grouping, vendor manifest, own .git) is reported as a warning and
only becomes fatal with `--warnings-as-errors` (target: step M6).  R4 document
overflow is an error (R22); documents within 20 lines of the limit get a
near-limit warning.  R19 validates config/migration.json as the machine-
readable state of the M1-M7 migration.

Usage:
    python tools/check_file_layout.py
    python tools/check_file_layout.py --strict --json reports/generated/file_layout.json
    python tools/check_file_layout.py --strict --warnings-as-errors   # M6 gate
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_EXT = (".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".ps1", ".java")
BINARY_EXT = (".o", ".obj", ".a", ".so", ".exe", ".apk", ".dll", ".lib")
GENERATED_IN_CONFIG = re.compile(
    r"(\.static\.json$|_plan\.json$|\.after\.json$|^coverage\.json$)")
LEGACY_NAME = re.compile(r"(_old|_v2|_test_py|_test_lift|_agent)\.[ch](pp)?$")

# M2 closed the monolith debt: every doc/*.md is now held to the same R4 limit.
KNOWN_LARGE_DOCS: dict[str, int] = {}
SOURCE_LIMIT = 2000


def lines(path: Path) -> int:
    try:
        with path.open("rb") as fh:
            return sum(1 for _ in fh)
    except OSError:
        return 0


def check_migration_ledger(errors: list[str], warnings: list[str]) -> None:
    """R19: the M1-M7 state is a pinned input, not prose and not a report."""
    ledger = ROOT / "config" / "migration.json"
    if not ledger.is_file():
        errors.append("R19: config/migration.json is missing")
        return
    try:
        data = json.loads(ledger.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"R19: config/migration.json is invalid ({exc})")
        return
    if not isinstance(data, dict) or data.get("schema") != 1:
        errors.append("R19: config/migration.json must be an object with schema: 1")
        return
    allowed = set(data.get("statuses", ["pending", "in_progress", "done", "blocked"]))
    steps = data.get("steps")
    if not isinstance(steps, list) or not steps:
        errors.append("R19: config/migration.json has no steps[]")
        return
    seen: set[str] = set()
    required = {"id", "status", "owner", "gate", "evidence", "updated_at"}
    for index, step in enumerate(steps):
        if not isinstance(step, dict):
            errors.append(f"R19: step {index} is not an object")
            continue
        missing = sorted(required - set(step))
        if missing:
            errors.append(f"R19: step {index} misses {missing}")
            continue
        sid = str(step["id"])
        if sid in seen:
            errors.append(f"R19: duplicate step id {sid}")
        seen.add(sid)
        if step["status"] not in allowed:
            errors.append(f"R19: {sid} has unknown status {step['status']!r}")
        if step["status"] == "done" and not step["evidence"]:
            errors.append(f"R19: {sid} is done without evidence (F2)")
    rules = data.get("rules", [])
    rule_ids: set[str] = set()
    for index, rule in enumerate(rules):
        if not isinstance(rule, dict) or not {"id", "status", "owner", "check"} <= set(rule):
            errors.append(f"R19: rule {index} is incomplete")
            continue
        rid = str(rule["id"])
        if rid in rule_ids:
            errors.append(f"R19: duplicate rule id {rid}")
        rule_ids.add(rid)


def source_files() -> list[Path]:
    out = []
    for base in ("runtime", "apps", "tests", "tools", "oracle"):
        root = ROOT / base
        if not root.is_dir():
            continue
        for p in root.rglob("*"):
            if (p.is_file() and p.suffix in SRC_EXT
                    and "__pycache__" not in p.parts
                    and "third_party" not in p.parts):
                out.append(p)
    return out


def check() -> dict:
    errors: list[str] = []
    warnings: list[str] = []
    rel = lambda p: p.relative_to(ROOT).as_posix()

    # R14 - repository hygiene
    gi = ROOT / ".gitignore"
    if not gi.is_file():
        errors.append("R14: .gitignore is missing")
    else:
        text = gi.read_text(encoding="utf-8")
        for required in ("/build/", "/runtime/guest/generated/",
                         "__pycache__/", "/reports/generated/"):
            if required not in text:
                errors.append(f"R2/R3/R14: .gitignore misses {required}")
    if not (ROOT / ".gitattributes").is_file():
        warnings.append("R14: .gitattributes is missing (CRLF policy)")
    if not (ROOT / ".git").exists():
        warnings.append("R14: no own .git; tree lives in the parent repo")

    # R1 - config contains inputs only
    cfg = ROOT / "config"
    if not (ROOT / "reports" / "generated").is_dir():
        errors.append("R1: reports/generated/ is missing")
    if cfg.is_dir():
        for p in cfg.iterdir():
            if p.is_file() and GENERATED_IN_CONFIG.search(p.name):
                errors.append(f"R1: generated report in config/: {rel(p)}")
        if not (cfg / "schema").is_dir():
            warnings.append("R10: config/schema/ is missing (step M3)")
        if not (cfg / "import_policy.1.27b.json").is_file():
            warnings.append("R10: import_policy.1.27b.json is missing "
                            "(832 unresolved PE imports are unclassified)")
        for p in sorted(cfg.glob("*.json")):
            try:
                data = json.loads(p.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"R10: {rel(p)} is not valid JSON ({exc})")
                continue
            if isinstance(data, dict) and "schema" not in data:
                warnings.append(f"R10: {rel(p)} has no 'schema' key")

    # R3 - compiled artefacts must be ignored
    ignored = gi.read_text(encoding="utf-8") if gi.is_file() else ""
    binaries = [p for p in ROOT.rglob("*")
                if p.is_file() and p.suffix in BINARY_EXT
                and "build" not in p.parts and "third_party" not in p.parts]
    if binaries:
        errors.append(f"R3: {len(binaries)} unignored binary artefact(s): "
                      + ", ".join(rel(p) for p in binaries[:4]))
    if "/third_party/softfloat/build-host/" not in ignored:
        warnings.append("R3: vendored SoftFloat build output is not ignored")

    # R4 - monolith gate
    for p in source_files():
        n = lines(p)
        if n > SOURCE_LIMIT:
            errors.append(f"R4: {rel(p)} has {n} lines (limit {SOURCE_LIMIT})")
    docs = ROOT / "docs"
    near_limit: list[str] = []
    if docs.is_dir():
        for p in sorted(docs.glob("*.md")):
            n = lines(p)
            limit = KNOWN_LARGE_DOCS.get(p.name, 400)
            if n > limit:
                errors.append(
                    f"R4/R22: {rel(p)} has {n} lines (limit {limit}); "
                    "split into a normative owner and register it in the index"
                )
            elif n >= 380:
                near_limit.append(f"{p.name} ({n})")
    if near_limit:
        warnings.append(
            "R22: documents within 20 lines of the R4 limit, a split owner "
            "must be declared before the next revision: " + ", ".join(near_limit)
        )
    check_migration_ledger(errors, warnings)
    if not (docs / "README.md").is_file():
        errors.append("R11: docs/README.md index is missing")

    # R5 / R13 - no dispatcher variants or legacy names in source
    dispatchers = [p for p in source_files()
                   if "dispatch" in p.name.lower() and p.suffix in (".c", ".h")]
    generated_dispatch = [p for p in dispatchers if p.name == "gen_dispatch.py"]
    if len(dispatchers) > 1 and not generated_dispatch:
        warnings.append("R5: several dispatcher-like sources; verify one generator")
    for p in source_files():
        if LEGACY_NAME.search(p.name):
            errors.append(f"R13: legacy variant in source tree: {rel(p)}")

    # R8 - oracle outside runtime
    if (ROOT / "runtime" / "host_ref").exists():
        errors.append("R8: Windows oracle must live in oracle/, not runtime/")

    # R9 - vendor inventory
    if not (ROOT / "third_party" / "VENDOR_MANIFEST.md").is_file():
        warnings.append("R9: third_party/VENDOR_MANIFEST.md is missing (step M6)")

    # R12 - tool/test grouping (target of M4)
    for d in ("tools/checks", "tools/fixtures", "tools/perf",
              "tests/unit", "tests/golden", "tests/device"):
        if not (ROOT / d).is_dir():
            warnings.append(f"R12: {d}/ is missing (step M4)")

    return {
        "root": ROOT.as_posix(),
        "source_files_scanned": len(source_files()),
        "errors": errors,
        "warnings": warnings,
        "verdict": "FAIL" if errors else ("WARN" if warnings else "OK"),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--warnings-as-errors", action="store_true")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    report = check()
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if args.strict and report["errors"]:
        return 1
    if args.warnings_as_errors and (report["errors"] or report["warnings"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
