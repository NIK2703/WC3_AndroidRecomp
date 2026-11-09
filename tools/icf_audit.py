#!/usr/bin/env python3
"""Static identical-code-folding audit for an already-built ELF (O-64).

The audit reads ``.symtab``/``.dynsym`` and executable sections from an ELF64
AArch64 shared object.  It folds aliases by virtual address, hashes every
function body, and reports byte-identical groups.  That is the same upper-bound
measurement used by PERFORMANCE_AUDIT_PASS6.md O-64; it is not a link step and
it makes no runtime claim (PM-17).  Missing symbols or artifacts are reported
as ``unavailable``, never as a pass.

The tool also reports the relocation counts that must stay visible when ICF is
evaluated: ``R_AARCH64_RELATIVE``, ``JUMP_SLOT`` and ``GLOB_DAT``.

Usage:
  python tools/icf_audit.py --so build/android_app/debug/cmake/libwc3gameactivity.so \
      --json reports/generated/icf_audit.json
  python tools/icf_audit.py --so candidate.so --compare baseline.so \
      --json reports/generated/icf_ab.json
  python tools/icf_audit.py --self-test

The linker option remains opt-in (``WC3_ICF``); this audit does not enable it
and does not edit the artifact.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SO = ROOT / "build" / "android_app" / "debug" / "cmake" / \
    "libwc3gameactivity.so"

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_DYNSYM = 11
SHF_EXECINSTR = 0x4
STT_FUNC = 2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2
SHN_XINDEX = 0xFFFF

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027


class ElfError(ValueError):
    """The input is not a supported ELF64 little-endian artifact."""


@dataclass(frozen=True)
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int


@dataclass(frozen=True)
class Symbol:
    name: str
    value: int
    size: int
    section_index: int


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_sections(data: bytes) -> list[Section]:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise ElfError("only ELF64 little-endian artifacts are supported")
    e_shoff = _u64(data, 0x28)
    e_shentsize = _u16(data, 0x3A)
    e_shnum = _u16(data, 0x3C)
    e_shstrndx = _u16(data, 0x3E)
    if e_shentsize < 64:
        raise ElfError("section header entry is too small")
    if e_shoff == 0:
        raise ElfError("no section table")

    def raw_section(i: int) -> tuple[int, int, int, int, int, int, int, int,
                                     int, int]:
        base = e_shoff + i * e_shentsize
        if base + 64 > len(data):
            raise ElfError("section table is truncated")
        return (
            _u32(data, base + 0),    # sh_name
            _u32(data, base + 4),    # sh_type
            _u64(data, base + 8),    # sh_flags
            _u64(data, base + 16),   # sh_addr
            _u64(data, base + 24),   # sh_offset
            _u64(data, base + 32),   # sh_size
            _u32(data, base + 40),   # sh_link
            _u32(data, base + 44),   # sh_info
            _u64(data, base + 48),   # sh_addralign
            _u64(data, base + 56),   # sh_entsize
        )

    # ELF extended numbering: e_shnum == 0 stores the real count in section 0's
    # sh_size, and e_shstrndx == SHN_XINDEX stores the real index in sh_link.
    if e_shnum == 0:
        section_zero = raw_section(0)
        e_shnum = section_zero[5]
        if e_shnum == 0:
            raise ElfError("empty section table")
    raw = [raw_section(i) for i in range(e_shnum)]
    if e_shstrndx == SHN_XINDEX:
        e_shstrndx = raw[0][6]
    if e_shstrndx >= len(raw):
        raise ElfError("bad section-name string table index")
    names_sec = raw[e_shstrndx]
    names_start, names_size = names_sec[4], names_sec[5]
    if names_start + names_size > len(data):
        raise ElfError("section-name string table is truncated")
    names = data[names_start:names_start + names_size]

    def name_at(offset: int) -> str:
        if offset >= len(names):
            return ""
        end = names.find(b"\0", offset)
        if end < 0:
            end = len(names)
        return names[offset:end].decode("ascii", errors="replace")

    sections: list[Section] = []
    for i, item in enumerate(raw):
        sections.append(Section(
            index=i,
            name=name_at(item[0]),
            type=item[1],
            flags=item[2],
            addr=item[3],
            offset=item[4],
            size=item[5],
            link=item[6],
            entsize=item[9],
        ))
    return sections


def read_symbols(data: bytes, sections: Sequence[Section]) -> list[Symbol]:
    tables = [s for s in sections if s.type in (SHT_SYMTAB, SHT_DYNSYM)]
    if not tables:
        raise ElfError("no symbol table")
    # Prefer .symtab: the audit needs names that a stripped dynamic table drops.
    table = next((s for s in tables if s.type == SHT_SYMTAB), tables[0])
    if table.link >= len(sections):
        raise ElfError("symbol table has no string table link")
    strings_section = sections[table.link]
    if strings_section.type != SHT_STRTAB:
        raise ElfError("symbol table link is not a string table")
    strings = data[strings_section.offset:
                   strings_section.offset + strings_section.size]
    entsize = table.entsize or 24
    if entsize < 24:
        raise ElfError("symbol entry is too small")
    count = table.size // entsize
    symbols: list[Symbol] = []
    for i in range(count):
        base = table.offset + i * entsize
        if base + 24 > len(data):
            raise ElfError("symbol table is truncated")
        st_name = _u32(data, base + 0)
        st_info = data[base + 4]
        st_shndx = _u16(data, base + 6)
        st_value = _u64(data, base + 8)
        st_size = _u64(data, base + 16)
        if (st_info & 0x0F) != STT_FUNC:
            continue
        if st_size == 0 or st_shndx in (SHN_UNDEF, SHN_ABS, SHN_COMMON):
            continue
        if st_name >= len(strings):
            name = ""
        else:
            end = strings.find(b"\0", st_name)
            if end < 0:
                end = len(strings)
            name = strings[st_name:end].decode("ascii", errors="replace")
        symbols.append(Symbol(name, st_value, st_size, st_shndx))
    return symbols


def function_bodies(data: bytes, sections: Sequence[Section],
                    symbols: Sequence[Symbol]) -> list[tuple[str, int, bytes]]:
    bodies: list[tuple[str, int, bytes]] = []
    for symbol in symbols:
        if symbol.section_index >= len(sections):
            continue
        section = sections[symbol.section_index]
        if not (section.flags & SHF_EXECINSTR) and not (
                section.name == ".text" or section.name.startswith(".text.")):
            continue
        if symbol.value < section.addr:
            continue
        rel = symbol.value - section.addr
        end = rel + symbol.size
        if rel >= section.size or end > section.size:
            continue
        body = data[section.offset + rel:section.offset + end]
        if len(body) != symbol.size:
            continue
        bodies.append((symbol.name, symbol.value, body))
    return bodies


def summarize_bodies(
        bodies: Iterable[tuple[str, int, bytes]],
        top: int = 10) -> dict[str, object]:
    """Fold aliases by VA and group the remaining bodies by bytes."""
    by_address: dict[int, tuple[str, bytes]] = {}
    aliases = 0
    for name, value, body in bodies:
        previous = by_address.get(value)
        if previous is not None:
            aliases += 1
            # Keep the lexicographically smallest name for stable output.
            if name and (not previous[0] or name < previous[0]):
                by_address[value] = (name, body)
        else:
            by_address[value] = (name, body)

    groups: dict[tuple[int, str], list[tuple[str, int]]] = {}
    for name, body in by_address.values():
        key = (len(body), sha256_bytes(body))
        groups.setdefault(key, []).append((name, len(body)))

    duplicate_functions = 0
    duplicate_bytes = 0
    duplicate_groups = 0
    l_groups = 0
    for members in groups.values():
        if len(members) < 2:
            continue
        duplicate_groups += 1
        duplicate_functions += len(members) - 1
        duplicate_bytes += (len(members) - 1) * members[0][1]
        if any(name.startswith("L_") for name, _ in members):
            l_groups += 1

    ordered = sorted(
        (members for members in groups.values() if len(members) > 1),
        key=lambda members: (-(len(members) - 1) * members[0][1],
                             -len(members), members[0][0]),
    )
    top_groups = []
    for members in ordered[:top]:
        names = sorted(name for name, _ in members)
        top_groups.append({
            "size": members[0][1],
            "count": len(members),
            "duplicate_bytes": (len(members) - 1) * members[0][1],
            "names": names[:12],
            "names_truncated": max(0, len(names) - 12),
        })

    return {
        "function_addresses": len(by_address),
        "aliases_folded": aliases,
        "groups": len(groups),
        "duplicate_groups": duplicate_groups,
        "groups_with_lifted_member": l_groups,
        "duplicate_functions": duplicate_functions,
        "duplicate_bytes": duplicate_bytes,
        "top_groups": top_groups,
    }


def section_bytes(sections: Sequence[Section], prefix: str) -> int:
    return sum(s.size for s in sections if s.name.startswith(prefix))


def relocation_counts(data: bytes,
                      sections: Sequence[Section]) -> dict[str, int]:
    counts = {
        "relative": 0,
        "jump_slot": 0,
        "glob_dat": 0,
        "abs64": 0,
        "other": 0,
        "total": 0,
    }
    for section in sections:
        if section.type != SHT_RELA or section.entsize < 24:
            continue
        count = section.size // section.entsize
        for i in range(count):
            base = section.offset + i * section.entsize
            if base + 24 > len(data):
                break
            info = _u64(data, base + 8)
            kind = info & 0xFFFFFFFF
            counts["total"] += 1
            if kind == R_AARCH64_RELATIVE:
                counts["relative"] += 1
            elif kind == R_AARCH64_JUMP_SLOT:
                counts["jump_slot"] += 1
            elif kind == R_AARCH64_GLOB_DAT:
                counts["glob_dat"] += 1
            elif kind == R_AARCH64_ABS64:
                counts["abs64"] += 1
            else:
                counts["other"] += 1
    return counts


def audit(path: Path, top: int = 10) -> dict[str, object]:
    data = path.read_bytes()
    sections = read_sections(data)
    symbols = read_symbols(data, sections)
    bodies = function_bodies(data, sections, symbols)
    summary = summarize_bodies(bodies, top=top)
    relocs = relocation_counts(data, sections)
    symtab = next((s for s in sections if s.type == SHT_SYMTAB), None)
    dynsym = next((s for s in sections if s.type == SHT_DYNSYM), None)
    executable = sum(s.size for s in sections if s.flags & SHF_EXECINSTR)
    return {
        "schema": "wc3.icf_audit.v1",
        "status": "report-only",
        "artifact": str(path).replace("\\", "/"),
        # Hash the same bytes that were parsed; a concurrent rebuild must not
        # make the reported identity describe a different file.
        "artifact_sha256": sha256_bytes(data),
        "artifact_bytes": len(data),
        "sections": {
            "executable_bytes": executable,
            "text_bytes": sum(s.size for s in sections if s.name == ".text"),
            "debug_bytes": section_bytes(sections, ".debug_"),
            "rela_dyn_bytes": sum(s.size for s in sections
                                   if s.name == ".rela.dyn"),
            "relr_dyn_bytes": sum(s.size for s in sections
                                   if s.name == ".relr.dyn"),
            "symtab_bytes": symtab.size if symtab else None,
            "dynsym_bytes": dynsym.size if dynsym else None,
            "plt_bytes": sum(s.size for s in sections if s.name == ".plt"),
            "bss_bytes": sum(s.size for s in sections if s.name == ".bss"),
            "rodata_bytes": sum(s.size for s in sections
                                 if s.name == ".rodata"),
        },
        "relocations": relocs,
        "functions": summary,
        "comparison": {
            "available": False,
            "reason": "no baseline artifact supplied (use --compare)",
        },
    }


COMPARE_SECTION_KEYS = (
    "executable_bytes", "text_bytes", "debug_bytes", "rela_dyn_bytes",
    "relr_dyn_bytes", "symtab_bytes", "dynsym_bytes", "plt_bytes",
    "bss_bytes", "rodata_bytes",
)
COMPARE_RELOCATION_KEYS = (
    "relative", "jump_slot", "glob_dat", "abs64", "other", "total",
)
COMPARE_FUNCTION_KEYS = (
    "function_addresses", "aliases_folded", "groups", "duplicate_groups",
    "groups_with_lifted_member", "duplicate_functions", "duplicate_bytes",
)


def _numeric_delta(base: object, candidate: object) -> int | None:
    if base is None or candidate is None:
        return None
    return int(candidate) - int(base)


def compare_reports(baseline: dict[str, object],
                    candidate: dict[str, object]) -> dict[str, object]:
    """Return a static candidate-minus-baseline ELF delta.

    The function compares only fields produced by :func:`audit`; it does not
    turn a byte delta into a runtime claim.  This is the repeatable form of the
    O-64 A/B measurement: both inputs carry their own sha256, so a caller can
    reject a comparison whose artifacts are not the expected pair.
    """
    def group(name: str, keys: Sequence[str]) -> dict[str, int | None]:
        base_group = baseline.get(name, {})
        cand_group = candidate.get(name, {})
        assert isinstance(base_group, dict) and isinstance(cand_group, dict)
        return {key: _numeric_delta(base_group.get(key), cand_group.get(key))
                for key in keys}

    def identity(report: dict[str, object]) -> dict[str, object]:
        return {
            "path": report.get("artifact"),
            "sha256": report.get("artifact_sha256"),
            "bytes": report.get("artifact_bytes"),
        }

    return {
        "available": True,
        "kind": "static-elf-delta",
        "baseline": identity(baseline),
        "candidate": identity(candidate),
        "delta": {
            "artifact_bytes": _numeric_delta(
                baseline.get("artifact_bytes"), candidate.get("artifact_bytes")),
            "sections": group("sections", COMPARE_SECTION_KEYS),
            "relocations": group("relocations", COMPARE_RELOCATION_KEYS),
            "functions": group("functions", COMPARE_FUNCTION_KEYS),
        },
    }


def print_comparison(comparison: dict[str, object]) -> None:
    if not comparison.get("available"):
        return
    baseline = comparison["baseline"]  # type: ignore[index]
    candidate = comparison["candidate"]  # type: ignore[index]
    delta = comparison["delta"]  # type: ignore[index]
    print("comparison (candidate - baseline, static bytes only):")
    print(f"  baseline  {baseline['sha256']} {baseline['bytes']} B")
    print(f"  candidate {candidate['sha256']} {candidate['bytes']} B")
    print(f"  artifact_bytes {delta['artifact_bytes']:+d}")
    for group_name in ("sections", "relocations", "functions"):
        print(f"  {group_name}:")
        for name, value in delta[group_name].items():  # type: ignore[union-attr]
            if value is not None:
                print(f"    {name:28s} {value:+d}")


def print_report(report: dict[str, object]) -> None:
    print(f"artifact: {report['artifact']}")
    print(f"  sha256 {report['artifact_sha256']}")
    print(f"  bytes  {report['artifact_bytes']}")
    sections = report["sections"]
    print("sections:")
    for name, value in sections.items():  # type: ignore[union-attr]
        print(f"  {name:18s} {value}")
    relocs = report["relocations"]
    print("relocations:")
    for name, value in relocs.items():  # type: ignore[union-attr]
        print(f"  {name:18s} {value}")
    functions = report["functions"]
    print("functions:")
    for name in ("function_addresses", "aliases_folded", "groups",
                 "duplicate_groups", "groups_with_lifted_member",
                 "duplicate_functions", "duplicate_bytes"):
        print(f"  {name:28s} {functions[name]}")  # type: ignore[index]
    print("top duplicate groups:")
    for group in functions["top_groups"]:  # type: ignore[index]
        names = ", ".join(group["names"])
        if group["names_truncated"]:
            names += f", ... (+{group['names_truncated']})"
        print(f"  {group['size']:>6d} B x {group['count']:<5d} {names}")
    comparison = report.get("comparison")
    if isinstance(comparison, dict):
        print_comparison(comparison)


def run_self_test() -> int:
    bodies = [
        ("L_00401000", 0x401000, b"same"),
        ("L_00402000", 0x402000, b"same"),
        ("alias", 0x402000, b"same"),
        ("L_00403000", 0x403000, b"different"),
        ("other", 0x404000, b"same"),
    ]
    summary = summarize_bodies(bodies)
    if summary["function_addresses"] != 4:
        print("icf audit self-test: alias folding failed")
        return 1
    if summary["aliases_folded"] != 1:
        print("icf audit self-test: alias count failed")
        return 1
    if summary["duplicate_functions"] != 2:
        print("icf audit self-test: duplicate count failed")
        return 1
    if summary["duplicate_bytes"] != 8:
        print("icf audit self-test: duplicate bytes failed")
        return 1
    if summary["groups_with_lifted_member"] != 1:
        print("icf audit self-test: lifted group count failed")
        return 1

    # Empty and single-member groups are not duplicates.
    if summarize_bodies([])["duplicate_functions"] != 0:
        print("icf audit self-test: empty input failed")
        return 1

    baseline = {
        "artifact": "baseline.so",
        "artifact_sha256": "a" * 64,
        "artifact_bytes": 100,
        "sections": {"text_bytes": 60, "plt_bytes": 8},
        "relocations": {"jump_slot": 3, "total": 7},
        "functions": {"duplicate_bytes": 20},
    }
    candidate = {
        "artifact": "candidate.so",
        "artifact_sha256": "b" * 64,
        "artifact_bytes": 120,
        "sections": {"text_bytes": 55, "plt_bytes": 12},
        "relocations": {"jump_slot": 1, "total": 9},
        "functions": {"duplicate_bytes": 24},
    }
    comparison = compare_reports(baseline, candidate)
    delta = comparison["delta"]  # type: ignore[index]
    if (not comparison["available"] or delta["artifact_bytes"] != 20 or
            delta["sections"]["text_bytes"] != -5 or
            delta["sections"]["plt_bytes"] != 4 or
            delta["relocations"]["jump_slot"] != -2 or
            delta["functions"]["duplicate_bytes"] != 4):
        print("icf audit self-test: comparison delta failed")
        return 1

    with tempfile.TemporaryDirectory(prefix="wc3_icf_audit_") as tmp:
        bad = Path(tmp) / "not-elf.bin"
        bad.write_bytes(b"not an elf")
        try:
            read_sections(bad.read_bytes())
        except ElfError:
            pass
        else:
            print("icf audit self-test: malformed ELF accepted")
            return 1

    print("icf audit self-test: OK")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", type=Path, default=DEFAULT_SO,
                        help="already-built ELF64 AArch64 shared object")
    parser.add_argument("--compare", type=Path,
                        help="baseline ELF for a static candidate-minus-baseline "
                             "delta")
    parser.add_argument("--json", type=Path,
                        help="write the full report as JSON")
    parser.add_argument("--top", type=int, default=10,
                        help="number of largest duplicate groups to print")
    parser.add_argument("--self-test", action="store_true",
                        help="check grouping and comparison logic without an "
                             "ELF artifact")
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        return run_self_test()

    if not args.so.is_file():
        print(f"icf audit: unavailable (missing artifact: {args.so})")
        return 2
    baseline: dict[str, object] | None = None
    try:
        if args.compare:
            if not args.compare.is_file():
                print(f"icf audit: unavailable (missing baseline: "
                      f"{args.compare})")
                return 2
            if args.compare.resolve() != args.so.resolve():
                baseline = audit(args.compare, top=0)
        report = audit(args.so, top=max(0, args.top))
        if baseline is None and args.compare:
            baseline = report
        if baseline is not None:
            report["comparison"] = compare_reports(baseline, report)
    except ElfError as exc:
        print(f"icf audit: unavailable ({exc})")
        return 2

    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"report: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
