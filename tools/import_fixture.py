#!/usr/bin/env python3
"""Build the 1.27b import/arity/cdecl baseline from the licensed PE.

The PE import directory is the presence source of truth.  `--pe` reads the
licensed 1.27.1.7085 files under `--game-dir` and writes the PE fixture before
merging it with the old HLE arity table.  A table-only run is still available
for an asset-free CI runner, but it is explicitly marked
`pe_fixture_status=unavailable` and is not a substitute for the PE gate.

Usage:
  python tools/import_fixture.py --corpus 1.27b --pe \
      --game-dir "E:/Games/Warcraft III" \
      --out config/imports.1.27b.json
  python tools/import_fixture.py --corpus 1.27b \
      --source E:/projects/warcraft3_recomp/runtime/android/hle_android.c \
      --write-table config/hle_table.1.27b.tsv
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from pe_imports import DEFAULT_GAME_DIR, build_fixture

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TABLE = ROOT / "config" / "hle_table.1.27b.tsv"
DEFAULT_PE = ROOT / "config" / "pe_imports.1.27b.json"

MODULE_BY_MACRO = {
    "K": "KERNEL32.dll",
    "KC": "KERNEL32.dll",
    "CR": "MSVCR80.dll",
    "U": "USER32.dll",
    "UC": "USER32.dll",
    "G": "GDI32.dll",
    "A": "ADVAPI32.dll",
    "S": "SHELL32.dll",
    "I": "IMM32.dll",
    "W": "WINMM.dll",
}
CDECL_BY_MACRO = {"KC", "CR", "UC"}

# LAN/Battle.net is out of scope for the 1.27 Android target.  The Windows-era
# HLE table contained no-op WS2_32 rows; they must not become "present".
EXCLUDED_MODULES = {"ws2_32.dll", "wsock32.dll", "wininet.dll", "wldap32.dll"}
# The old Android HLE table named the CRT by its 1.26 module.  1.27's PE imports
# msvcr120.dll; the same function surface is bound through the MSVCR80 entries.
MODULE_ALIASES = {"msvcr120.dll": "msvcr80.dll"}
# Imported by the graphics fallback path only.  The target has no OpenGL
# frontend; these rows stay visible as a policy blocker rather than becoming a
# hidden no-op.
FORBIDDEN_MODULES = {"opengl32.dll"}

MACRO_RE = re.compile(
    r'^\s*(K|KC|CR|U|UC|G|A|S|I|W)\(\s*"([^"]+)"\s*,\s*\w+\s*,\s*(\d+)\s*\)'
)
EXPLICIT_RE = re.compile(
    r'^\s*\{\s*"([^"]+)"\s*,\s*("([^"]*)"|NULL)\s*,\s*[^,]+,\s*'
    r'(\d+)\s*,\s*(\d+)\s*(?:,\s*(\d+)\s*)?\}'
)


@dataclass(frozen=True)
class Entry:
    module: str
    name: str | None
    ordinal: int | None
    argc: int
    callconv: str
    source: str

    def key(self) -> tuple[str, str, int]:
        return (self.module.lower(), self.name or "", self.ordinal or 0)


def parse_c_table(path: Path) -> list[Entry]:
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    start = None
    for i, line in enumerate(text):
        if re.search(r"static\s+const\s+HleDef\s+g_defs\s*\[\s*\]\s*=", line):
            start = i
            break
    if start is None:
        raise ValueError(f"g_defs table not found in {path}")

    entries: list[Entry] = []
    for line_no, line in enumerate(text[start + 1 :], start=start + 2):
        if line.strip() == "};":
            break
        m = MACRO_RE.match(line)
        if m:
            macro, name, argc = m.groups()
            entries.append(
                Entry(
                    module=MODULE_BY_MACRO[macro],
                    name=name,
                    ordinal=None,
                    argc=int(argc),
                    callconv="cdecl" if macro in CDECL_BY_MACRO else "stdcall",
                    source=f"{path.name}:{line_no}",
                )
            )
            continue
        m = EXPLICIT_RE.match(line)
        if m:
            module, raw_name, name, argc, cdecl, ordinal = m.groups()
            entries.append(
                Entry(
                    module=module,
                    name=name if raw_name != "NULL" else None,
                    ordinal=int(ordinal) if ordinal else None,
                    argc=int(argc),
                    callconv="cdecl" if int(cdecl) else "stdcall",
                    source=f"{path.name}:{line_no}",
                )
            )
    if not entries:
        raise ValueError(f"no HLE rows parsed from {path}")
    return entries


def load_overrides(path: Path) -> dict[tuple[str, str, int], Entry]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[tuple[str, str, int], Entry] = {}
    for row in data.get("entries", []):
        e = Entry(
            module=row["module"],
            name=row.get("name"),
            ordinal=row.get("ordinal"),
            argc=int(row["argc"]),
            callconv=row["callconv"],
            source=row.get("source", str(path)),
        )
        out[e.key()] = e
    return out


def apply_overrides(entries: list[Entry], overrides: dict[tuple[str, str, int], Entry]) -> tuple[list[Entry], set[tuple[str, str, int]]]:
    used: set[tuple[str, str, int]] = set()
    out: list[Entry] = []
    for e in entries:
        override = overrides.get(e.key())
        if override is None:
            out.append(e)
        else:
            out.append(override)
            used.add(e.key())
    missing = set(overrides) - used
    if missing:
        names = ", ".join(k[0] + "!" + (k[1] or "#" + str(k[2])) for k in sorted(missing))
        raise ValueError(f"arity overrides did not match any table row: {names}")
    return out, used


def dedupe(entries: list[Entry]) -> tuple[list[Entry], list[str]]:
    kept: dict[tuple[str, str, int], Entry] = {}
    warnings: list[str] = []
    for e in entries:
        prev = kept.get(e.key())
        if prev is None:
            kept[e.key()] = e
            continue
        if (prev.argc, prev.callconv) != (e.argc, e.callconv):
            raise ValueError(
                "ambiguous duplicate: "
                f"{e.module}!{e.name or ('#' + str(e.ordinal))} "
                f"{prev.argc}/{prev.callconv} vs {e.argc}/{e.callconv}; "
                "add a config/arity_overrides entry with evidence"
            )
        if prev.source != e.source:
            warnings.append(
                f"duplicate ignored: {e.module}!{e.name or ('#' + str(e.ordinal))} "
                f"({e.source}, first {prev.source})"
            )
    return list(kept.values()), warnings


def filter_excluded(entries: list[Entry]) -> tuple[list[Entry], list[dict]]:
    """LAN/Internet networking is outside the 1.27 Android target.

    The legacy table carried WS2_32 no-op rows from the Windows port.  Keeping
    them would silently make a mandatory network import look implemented, so
    they are removed from the baseline and reported explicitly.
    """
    kept: list[Entry] = []
    excluded: list[dict] = []
    for e in entries:
        if e.module.lower() in EXCLUDED_MODULES:
            excluded.append(
                {
                    "module": e.module,
                    "name": e.name,
                    "ordinal": e.ordinal,
                    "source": e.source,
                    "reason": "LAN/Battle.net out of scope",
                }
            )
        else:
            kept.append(e)
    return kept, excluded


def read_tsv(path: Path) -> list[Entry]:
    rows: list[Entry] = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 6:
            raise ValueError(f"{path}:{line_no}: expected 6 TSV columns")
        module, name, ordinal, argc, callconv, source = parts
        if callconv not in ("stdcall", "cdecl"):
            raise ValueError(f"{path}:{line_no}: bad callconv {callconv!r}")
        rows.append(
            Entry(
                module=module,
                name=name or None,
                ordinal=int(ordinal) if ordinal else None,
                argc=int(argc),
                callconv=callconv,
                source=source,
            )
        )
    if not rows:
        raise ValueError(f"no rows in {path}")
    return rows


def write_tsv(path: Path, entries: list[Entry]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# module\tname\tordinal\targc\tcallconv\tsource",
        "# Bootstrap export from the historical Android HLE table; not a PE fixture.",
    ]
    for e in sorted(entries, key=lambda x: (x.module.lower(), (x.name or "").lower(), x.ordinal or 0)):
        lines.append(
            "\t".join(
                [
                    e.module,
                    e.name or "",
                    "" if e.ordinal is None else str(e.ordinal),
                    str(e.argc),
                    e.callconv,
                    e.source,
                ]
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def as_json_entry(e: Entry) -> dict:
    return {
        "module": e.module,
        "name": e.name,
        "ordinal": e.ordinal,
        "argc": e.argc,
        "callconv": e.callconv,
        "source": e.source,
        "va": None,
        "presence": "table",
    }


def hle_key(module: str, name: str | None, ordinal: int | None) -> tuple[str, str, int]:
    return (module.lower(), (name or "").lower(), ordinal or 0)


def find_hle(hle_map: dict[tuple[str, str, int], Entry], module: str,
             name: str | None, ordinal: int | None) -> Entry | None:
    found = hle_map.get(hle_key(module, name, ordinal))
    if found is not None:
        return found
    alias = MODULE_ALIASES.get(module.lower())
    if alias:
        return hle_map.get(hle_key(alias, name, ordinal))
    return None


def merge_pe(pe: dict, hle_entries: list[Entry]) -> tuple[list[dict], dict, list[Entry]]:
    hle_map = {hle_key(e.module, e.name, e.ordinal): e for e in hle_entries}
    used: set[tuple[str, str, int]] = set()
    rows: list[dict] = []
    counts = {"hle": 0, "unresolved": 0, "excluded": 0, "forbidden": 0}

    for pe_row in pe.get("entries", []):
        module = pe_row["module"]
        name = pe_row.get("name")
        ordinal = pe_row.get("ordinal")
        hle = find_hle(hle_map, module, name, ordinal)
        status: str
        reason = None
        if hle is not None:
            status = "hle"
            used.add(hle_key(hle.module, hle.name, hle.ordinal))
            argc = hle.argc
            callconv = hle.callconv
            hle_source = hle.source
        elif module.lower() in EXCLUDED_MODULES:
            status = "excluded"
            reason = "LAN/Battle.net out of scope"
            argc = None
            callconv = None
            hle_source = None
        elif module.lower() in FORBIDDEN_MODULES:
            status = "forbidden"
            reason = "OpenGL/GLES frontend is forbidden in the 1.27 target"
            argc = None
            callconv = None
            hle_source = None
        else:
            status = "unresolved"
            reason = "no HLE implementation in the bootstrap table"
            argc = None
            callconv = None
            hle_source = None
        counts[status] += 1
        rows.append(
            {
                "file": pe_row.get("file"),
                "va": pe_row.get("slot_va"),
                "module": module,
                "name": name,
                "ordinal": ordinal,
                "argc": argc,
                "callconv": callconv,
                "status": status,
                "reason": reason,
                "hle_source": hle_source,
                "source": f"pe:{pe_row.get('file')}:0x{pe_row.get('slot_va'):08X}"
                if pe_row.get("slot_va") is not None
                else f"pe:{pe_row.get('file')}",
                "presence": "pe+hle" if hle is not None else "pe",
            }
        )

    table_only = [e for e in hle_entries if hle_key(e.module, e.name, e.ordinal) not in used]
    return rows, counts, table_only


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source", type=Path, help="legacy C file containing g_defs")
    parser.add_argument("--table", type=Path, default=DEFAULT_TABLE)
    parser.add_argument("--pe", action="store_true", help="read the licensed PE import directories")
    parser.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR)
    parser.add_argument("--pe-imports", type=Path, default=DEFAULT_PE)
    parser.add_argument("--pe-out", type=Path, default=DEFAULT_PE)
    parser.add_argument("--no-pe", action="store_true")
    parser.add_argument(
        "--overrides",
        type=Path,
        default=ROOT / "config" / "arity_overrides.1.27b.json",
    )
    parser.add_argument("--write-table", type=Path)
    parser.add_argument("--arity-out", type=Path)
    parser.add_argument("--cdecl-out", type=Path)
    args = parser.parse_args(argv)

    overrides = load_overrides(args.overrides)
    if args.source:
        raw_entries = parse_c_table(args.source)
    else:
        if not args.table.exists():
            raise SystemExit(
                f"missing {args.table}; pass --source once to export the legacy table"
            )
        raw_entries = read_tsv(args.table)

    filtered, excluded = filter_excluded(raw_entries)
    entries, warnings = dedupe(apply_overrides(filtered, overrides)[0])
    if args.write_table:
        write_tsv(args.write_table, entries)

    entries = sorted(
        entries,
        key=lambda x: (x.module.lower(), (x.name or "").lower(), x.ordinal or 0),
    )

    pe = None
    pe_path = args.pe_imports
    if args.pe and not args.no_pe:
        try:
            pe = build_fixture(args.game_dir, args.corpus)
        except (FileNotFoundError, ValueError) as exc:
            print(f"PE fixture failed: {exc}", file=sys.stderr)
            return 1
        bad = [f["name"] for f in pe["files"] if f["hash_pinned"] and not f["hash_verified"]]
        if bad:
            print(
                f"PE fixture hash mismatch for required module(s): {', '.join(bad)}",
                file=sys.stderr,
            )
            return 1
        args.pe_out.parent.mkdir(parents=True, exist_ok=True)
        args.pe_out.write_text(
            json.dumps(pe, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        pe_path = args.pe_out
    elif not args.no_pe and pe_path and pe_path.exists():
        pe = json.loads(pe_path.read_text(encoding="utf-8"))

    if pe is not None:
        if pe.get("pe_fixture_status") != "available":
            print("PE fixture is present but pe_fixture_status is not available", file=sys.stderr)
            return 1
        if pe.get("total") != len(pe.get("entries", [])):
            print("PE fixture total does not match entries", file=sys.stderr)
            return 1
        try:
            pe_display = pe_path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            pe_display = pe_path.name
        rows, counts, table_only = merge_pe(pe, entries)
        unresolved = [r for r in rows if r["status"] == "unresolved"]
        forbidden = [r for r in rows if r["status"] == "forbidden"]
        imports = {
            "schema": 1,
            "corpus": args.corpus,
            "generated_by": "tools/import_fixture.py",
            "presence": "pe",
            "pe_fixture_status": "available",
            "pe_imports_file": pe_display,
            "pe_files": pe.get("files", []),
            "total": len(rows),
            "hle_total": counts["hle"],
            "unresolved_total": counts["unresolved"],
            "forbidden_total": counts["forbidden"],
            "excluded_total": counts["excluded"],
            "hle_table_total": len(entries),
            "hle_table_only_total": len(table_only),
            "excluded_table": excluded,
            "unresolved": unresolved,
            "forbidden": forbidden,
            "warnings": warnings,
            "entries": rows,
        }
    else:
        imports = {
            "schema": 1,
            "corpus": args.corpus,
            "generated_by": "tools/import_fixture.py",
            "presence": "table",
            "pe_fixture_status": "unavailable",
            "pe_fixture_reason": "PE import fixture was not supplied",
            "total": len(entries),
            "excluded_total": len(excluded),
            "excluded": excluded,
            "warnings": warnings,
            "entries": [as_json_entry(e) for e in entries],
        }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(imports, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    if args.arity_out:
        arity = {
            "schema": 1,
            "corpus": args.corpus,
            "generated_by": "tools/import_fixture.py",
            "entries": [
                {
                    "module": e.module,
                    "name": e.name,
                    "ordinal": e.ordinal,
                    "argc": e.argc,
                    "callconv": e.callconv,
                }
                for e in entries
            ],
        }
        args.arity_out.parent.mkdir(parents=True, exist_ok=True)
        args.arity_out.write_text(
            json.dumps(arity, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
    if args.cdecl_out:
        allow = {
            "schema": 1,
            "corpus": args.corpus,
            "generated_by": "tools/import_fixture.py",
            "entries": [
                {"module": e.module, "name": e.name, "ordinal": e.ordinal}
                for e in entries
                if e.callconv == "cdecl"
            ],
        }
        args.cdecl_out.parent.mkdir(parents=True, exist_ok=True)
        args.cdecl_out.write_text(
            json.dumps(allow, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )

    if imports.get("presence") == "pe":
        print(
            f"imports: {imports['total']} PE slots, hle={imports['hle_total']}, "
            f"unresolved={imports['unresolved_total']}, "
            f"forbidden={imports['forbidden_total']}, "
            f"excluded={imports['excluded_total']} (pe_fixture=available)"
        )
    else:
        print(
            f"imports: {imports['total']} table entries "
            f"(presence=table, pe_fixture=unavailable)"
        )
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
