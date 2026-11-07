#!/usr/bin/env python3
"""Validate the four roadmap documents and their W0 state (EX-12, W0.9).

Read-only with respect to the project tree: the only file written is the
report under reports/generated/roadmap/ (gitignored, V1.6).

Checks:
  1. four owner documents exist, are registered in docs/README.md and are
     shorter than 380 lines (R4/R22 near-limit headroom);
  2. every referenced stable id falls inside the declared range of its
     family, and every WP/EX/AS id has a section in its owner document;
  3. config/roadmap_status.json, when present, has a valid schema,
     head_sha == git HEAD, status_digest == hash of `git status --short`,
     and single_writer is set; no `done` package lacks evidence;
  4. config/roadmap_contracts.json, when present, has existing paths with
     matching sha256 and a recomputed contracts_sha256;
  5. W0 artifacts required by the plan are present; their absence is
     `unavailable`, not `pass` (V1.2/ADR-029).

Exit codes (BP-5.3): 0 pass, 1 fail, 2 unavailable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = {
    "ROADMAP_PARALLEL.md": ("WP", "EX", "AS"),
    "ROADMAP_WORKPACKAGES.md": ("ROADMAP_PARALLEL.md", "EX", "AS"),
    "ROADMAP_EXECUTION.md": ("ROADMAP_PARALLEL.md", "WP", "AS"),
    "ROADMAP_ASSURANCE.md": ("ROADMAP_PARALLEL.md", "WP", "EX"),
}
OWNER_OF_SECTION = {"WP": "ROADMAP_WORKPACKAGES.md",
                    "EX": "ROADMAP_EXECUTION.md",
                    "AS": "ROADMAP_ASSURANCE.md"}
REQUIRED_W0 = [
    "config/file_owners.json",
    "config/dep_matrix.json",
    "config/roadmap_contracts.json",
    "config/roadmap_status.json",
    "tools/check_deps.py",
    "tools/check_contracts.py",
    "tools/check_manifest.py",
]
# Declared id ranges (owner documents define the exact members).
RANGES = {
    "WP": (0, 12), "EX": (0, 14), "AS": (0, 19), "G": (1, 15),
    "PM": (1, 24), "TA": (1, 18), "GB": (1, 10), "BP": (1, 11),
    "ABI": (1, 11), "ADR": (1, 29), "M": (1, 8), "R": (1, 22),
    "E": (1, 12), "V": (1, 10), "H": (1, 12), "DUT": (0, 5),
}
ID_REF = re.compile(
    r"(?<![\w-])(WP|EX|AS|G|PM|TA|GB|BP|ABI|ADR|M|R|E|V|H|DUT)-(\d+)"
    r"|\bW0\.(\d+)|\bStep (-?\d+)")
SECTION_HEADING = re.compile(r"^#{2,4}\s+(WP|EX|AS)-(\d+)\b", re.MULTILINE)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                              text=True, check=False).stdout
    except OSError:
        return ""


def status_digest() -> str:
    out = git("status", "--short", "--", ".")
    return hashlib.sha256("\n".join(sorted(out.splitlines())).encode()).hexdigest()


def contracts_digest(contracts: list[dict]) -> str:
    parts = [f"{c['path']}:{c['sha256']}" for c in
             sorted(contracts, key=lambda c: c["path"])]
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()


def check_docs(errors: list[str], warnings: list[str]) -> dict:
    index = (ROOT / "docs" / "README.md").read_text(encoding="utf-8")
    lengths = {}
    for name in DOCS:
        path = ROOT / "docs" / name
        if not path.is_file():
            errors.append(f"missing owner document: docs/{name}")
            continue
        n = len(path.read_text(encoding="utf-8").splitlines())
        lengths[name] = n
        if n >= 380:
            errors.append(f"R22: docs/{name} has {n} lines (plan limit 380)")
        elif n >= 376:
            warnings.append(f"near-limit: docs/{name} has {n} lines "
                            f"(4 lines of headroom to the 380 plan limit)")
        if name not in index:
            errors.append(f"docs/{name} is not registered in docs/README.md")
    return lengths


def check_ids(errors: list[str], warnings: list[str]) -> dict:
    defined: dict[str, set[int]] = {k: set() for k in OWNER_OF_SECTION}
    for prefix, owner in OWNER_OF_SECTION.items():
        text = (ROOT / "docs" / owner).read_text(encoding="utf-8")
        defined[prefix] = {int(n) for p, n in SECTION_HEADING.findall(text)
                           if p == prefix}
    refs: dict[str, set[int]] = {}
    for name in DOCS:
        text = (ROOT / "docs" / name).read_text(encoding="utf-8")
        for m in ID_REF.finditer(text):
            family = m.group(1)
            if family:
                num = int(m.group(2))
            elif m.group(3) is not None:
                family, num = "W0", int(m.group(3))
                if not 1 <= num <= 10:
                    errors.append(f"{name}: W0.{num} outside W0.1..W0.10")
                continue
            elif m.group(4) is not None:
                num = int(m.group(4))
                if not -1 <= num <= 9:
                    errors.append(f"{name}: Step {num} outside Step -1..9")
                continue
            else:
                continue
            refs.setdefault(family, set()).add(num)
            lo, hi = RANGES.get(family, (0, 0))
            if not lo <= num <= hi:
                errors.append(f"{name}: {family}-{num} outside declared range "
                              f"{family}-{lo}..{family}-{hi}")
    for prefix in OWNER_OF_SECTION:
        missing = refs.get(prefix, set()) - defined[prefix]
        if missing:
            warnings.append(f"referenced {prefix}-s without a section in "
                            f"{OWNER_OF_SECTION[prefix]}: {sorted(missing)}")
    return {"families": {k: sorted(v) for k, v in sorted(refs.items())}}


def check_status(errors: list[str], unavailable: list[str]) -> None:
    path = ROOT / "config" / "roadmap_status.json"
    if not path.is_file():
        unavailable.append("config/roadmap_status.json")
        return
    data = json.loads(path.read_text(encoding="utf-8"))
    for key in ("schema", "head_sha", "snapshot_at", "wave", "tracks",
                "sync_points", "updated_at"):
        if key not in data:
            errors.append(f"roadmap_status.json: missing '{key}'")
    if "single_writer" not in data:
        errors.append("roadmap_status.json: missing 'single_writer'")
    head = git("rev-parse", "--short", "HEAD").strip()
    if head and data.get("head_sha") != head:
        errors.append(f"head_sha drift: {data.get('head_sha')} != {head}")
    if "status_digest" in data and data["status_digest"] != status_digest():
        errors.append("status_digest does not match git status")
    for track, state in (data.get("tracks") or {}).items():
        if state.get("status") == "done" and not state.get("evidence_sha256"):
            errors.append(f"track {track}: done without evidence_sha256")


def check_contracts(errors: list[str], unavailable: list[str]) -> None:
    path = ROOT / "config" / "roadmap_contracts.json"
    if not path.is_file():
        unavailable.append("config/roadmap_contracts.json")
        return
    data = json.loads(path.read_text(encoding="utf-8"))
    contracts = data.get("contracts") or []
    for c in contracts:
        p = ROOT / c["path"]
        if not p.is_file():
            errors.append(f"contract path missing: {c['path']}")
        elif sha256_file(p) != c["sha256"]:
            errors.append(f"contract sha256 drift: {c['path']}")
    if data.get("contracts_sha256") != contracts_digest(contracts):
        errors.append("contracts_sha256 is stale; recompute at S*")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="canonical invocation (V7/EX-12); report all findings")
    ap.add_argument("--json", default="reports/generated/roadmap/check_roadmap.json")
    args = ap.parse_args()

    errors: list[str] = []
    warnings: list[str] = []
    unavailable: list[str] = []
    lengths = check_docs(errors, warnings)
    id_report = check_ids(errors, warnings)
    check_status(errors, unavailable)
    check_contracts(errors, unavailable)
    for rel in REQUIRED_W0:
        if not (ROOT / rel).exists():
            unavailable.append(rel)
    unavailable = sorted(set(unavailable))

    if errors:
        status, code = "fail", 1
    elif unavailable:
        status, code = "unavailable", 2
    else:
        status, code = "pass", 0

    report = {
        "gate_id": "ROADMAP", "command": "python tools/check_roadmap.py --strict",
        "status": status, "exit_code": code,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "artifact_sha256": sha256_file(ROOT / "docs" / "ROADMAP_PARALLEL.md"),
        "head_sha": git("rev-parse", "--short", "HEAD").strip(),
        "status_digest": status_digest(),
        "doc_lengths": lengths, "id_ranges": id_report,
        "errors": errors, "warnings": warnings, "unavailable": unavailable,
    }
    out = ROOT / args.json
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")
    print(f"roadmap: {status} ({len(errors)} errors, {len(warnings)} warnings, "
          f"{len(unavailable)} unavailable) -> {args.json}")
    for line in errors + warnings:
        print(("ERROR: " if line in errors else "WARN: ") + line)
    return code


if __name__ == "__main__":
    sys.exit(main())
