#!/usr/bin/env python3
"""HLE presence/arity coverage gate.

`table` mode validates the bootstrap arity/cdecl table.  `pe` mode reads the
real 1.27b import directories and requires every non-excluded import to have
an HLE row with a known arity.  An unresolved or forbidden import is a gate
failure, not a warning; the fixture itself is `available` now that the licensed
PE exists.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMPORTS = ROOT / "config" / "imports.1.27b.json"
DEFAULT_ARITY = ROOT / "config" / "hle_arity.1.27b.json"
DEFAULT_CDECL = ROOT / "config" / "cdecl_allowlist.1.27b.json"
DEFAULT_TABLE = ROOT / "config" / "hle_table.1.27b.tsv"

FORBIDDEN_MODULES = {"ws2_32.dll", "wsock32.dll", "wininet.dll", "wldap32.dll"}


def load(path: Path) -> dict:
    if not path.exists():
        raise SystemExit(f"missing {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def key(row: dict) -> tuple[str, str, int]:
    return (row["module"].lower(), row.get("name") or "", row.get("ordinal") or 0)


def module_key(module: str) -> str:
    """Basename shared by import descriptors and the PE export index."""
    return module.replace("\\", "/").rsplit("/", 1)[-1].lower()


def read_table(path: Path) -> list[dict]:
    rows = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 6:
            raise SystemExit(f"{path}:{line_no}: expected 6 TSV columns")
        module, name, ordinal, argc, callconv, source = parts
        rows.append(
            {
                "module": module,
                "name": name or None,
                "ordinal": int(ordinal) if ordinal else None,
                "argc": int(argc),
                "callconv": callconv,
                "source": source,
            }
        )
    return rows


def validate_rows(rows: list[dict], where: str) -> list[str]:
    errors: list[str] = []
    seen: set[tuple[str, str, int]] = set()
    for i, row in enumerate(rows):
        module = row.get("module", "")
        if module.lower() in FORBIDDEN_MODULES and row.get("status") not in (
            "excluded",
            None,
        ):
            errors.append(f"{where}[{i}]: forbidden network module {module}")
        if row.get("callconv") not in ("stdcall", "cdecl"):
            errors.append(f"{where}[{i}]: bad callconv {row.get('callconv')!r}")
        if not isinstance(row.get("argc"), int) or not (0 <= row["argc"] <= 64):
            errors.append(f"{where}[{i}]: implausible argc {row.get('argc')!r}")
        if not row.get("name") and row.get("ordinal") is None:
            errors.append(f"{where}[{i}]: neither name nor ordinal")
        k = key(row)
        if k in seen:
            errors.append(f"{where}[{i}]: duplicate {k}")
        seen.add(k)
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--imports", type=Path, default=DEFAULT_IMPORTS)
    parser.add_argument("--arity", type=Path, default=DEFAULT_ARITY)
    parser.add_argument("--cdecl", type=Path, default=DEFAULT_CDECL)
    parser.add_argument("--table", type=Path, default=DEFAULT_TABLE)
    parser.add_argument("--fixture-kind", choices=["table", "pe"], default="pe")
    parser.add_argument("--require-pe", action="store_true")
    parser.add_argument(
        "--require-hle",
        action="store_true",
        help="fail on unresolved/forbidden imports (implementation gate, Step 5)",
    )
    args = parser.parse_args(argv)

    imports = load(args.imports)
    arity = load(args.arity)
    cdecl = load(args.cdecl)
    table = read_table(args.table)
    errors: list[str] = []
    unresolved = 0
    forbidden = 0

    if imports.get("corpus") != args.corpus:
        errors.append(f"corpus mismatch: {imports.get('corpus')!r}")
    if args.fixture_kind == "pe":
        if imports.get("presence") != "pe":
            errors.append("PE fixture requested but imports are table-derived")
        if imports.get("pe_fixture_status") != "available":
            errors.append("pe_fixture_status must be available")
        rows = imports.get("entries", [])
        if imports.get("total") != len(rows):
            errors.append("total does not match entries")
        status_counts = {
            "hle": 0,
            "lifted": 0,
            "unresolved": 0,
            "excluded": 0,
            "forbidden": 0,
        }
        for i, row in enumerate(rows):
            status = row.get("status")
            if status not in status_counts:
                errors.append(f"entry {i}: bad status {status!r}")
                continue
            status_counts[status] += 1
            if status in ("unresolved", "forbidden") and not row.get("reason"):
                # `unresolved` is allowed by the presence gate but must be
                # explicit, never silently dropped from the fixture.
                errors.append(f"entry {i}: {status} import without reason")
            elif status == "excluded" and not row.get("reason"):
                errors.append(f"entry {i}: excluded without reason")
            elif status == "hle":
                if not isinstance(row.get("argc"), int):
                    errors.append(f"entry {i}: HLE import has no argc")
                if row.get("callconv") not in ("stdcall", "cdecl"):
                    errors.append(f"entry {i}: HLE import has no callconv")
        unresolved = status_counts["unresolved"]
        forbidden = status_counts["forbidden"]
        for name, count in status_counts.items():
            if imports.get(f"{name}_total", count) != count:
                errors.append(f"{name}_total mismatch")
        if sum(status_counts.values()) != imports.get("total"):
            errors.append("status counts do not sum to total")
        # The classification is only as good as the export index it was
        # derived from.  Re-check every row against the PE fixture: an import
        # from a loaded module must be lifted, and a lifted symbol must exist
        # in that module's export table.  A stale or hand-edited entry cannot
        # silence HLE debt this way.
        fixture_path = ROOT / str(imports.get("pe_imports_file", ""))
        try:
            fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            fixture = {}
            errors.append("cannot read the PE export fixture")
        in_scope = {
            module_key(str(item.get("name", "")))
            for item in fixture.get("files", [])
            if isinstance(item, dict) and item.get("target_in_scope")
        }
        exports = {
            name.lower(): index
            for name, index in fixture.get("exports", {}).items()
            if name.lower() in in_scope
        }
        for i, row in enumerate(rows):
            module = module_key(row.get("module", ""))
            if module in in_scope and row.get("status") != "lifted":
                errors.append(
                    f"entry {i}: import from loaded module {module} is not lifted"
                )
            if row.get("status") != "lifted":
                continue
            index = exports.get(module)
            if index is None:
                errors.append(f"entry {i}: lifted module has no export index")
            elif row.get("name") is not None:
                if row["name"] not in index.get("names", []):
                    errors.append(f"entry {i}: lifted name is not exported")
            elif row.get("ordinal") is None or row["ordinal"] not in index.get(
                "ordinals", []
            ):
                errors.append(f"entry {i}: lifted ordinal is not exported")
        files = imports.get("pe_files", [])
        pinned = [f for f in files if f.get("hash_pinned")]
        if len(pinned) < 5:
            errors.append("PE fixture has fewer than five hash-pinned modules")
        if any(not f.get("hash_verified") for f in pinned):
            errors.append("PE fixture contains a pinned module with a hash mismatch")
        if args.require_hle:
            if unresolved:
                errors.append(f"{unresolved} unresolved PE imports (HLE implementation gate)")
            if forbidden:
                errors.append(f"{forbidden} forbidden PE imports (policy gate)")
    else:
        if not table:
            errors.append("empty HLE table")
        if imports.get("presence") != "pe" and args.require_pe:
            errors.append("PE fixture required but not present")

    if not table:
        errors.append("HLE table is empty")
    errors += validate_rows(table, "table")

    arity_rows = {
        key(r): (r.get("argc"), r.get("callconv")) for r in arity.get("entries", [])
    }
    if set(arity_rows) != {key(r) for r in table}:
        errors.append("arity table does not match HLE table")
    for row in table:
        expected = (row["argc"], row["callconv"])
        if arity_rows.get(key(row)) != expected:
            errors.append(f"arity mismatch: {key(row)}")
    cdecl_keys = {key(r) for r in cdecl.get("entries", [])}
    expected_cdecl = {key(r) for r in table if r["callconv"] == "cdecl"}
    if cdecl_keys != expected_cdecl:
        errors.append("cdecl allowlist does not match table")

    if errors:
        for e in errors[:80]:
            print(f"ERROR: {e}", file=sys.stderr)
        if len(errors) > 80:
            print(f"ERROR: ... {len(errors) - 80} more", file=sys.stderr)
        print(f"hle coverage: FAIL ({len(errors)} errors)", file=sys.stderr)
        return 1

    print(f"presence(table) OK: {len(table)} entries")
    print(f"arity OK: {len(table)} entries")
    print(f"pe_fixture_status={imports.get('pe_fixture_status')}")
    if args.fixture_kind == "pe":
        print(
            f"PE presence OK: total={imports.get('total')} "
            f"hle={imports.get('hle_total')} "
            f"lifted={imports.get('lifted_total')} "
            f"excluded={imports.get('excluded_total')} "
            f"unresolved={unresolved} forbidden={forbidden}"
        )
        if unresolved or forbidden:
            print(
                "implementation debt: unresolved/forbidden imports are fatal at "
                "startup until Step 5 (use --require-hle to gate them)",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
