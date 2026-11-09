#!/usr/bin/env python3
"""Static PASS8 audit for an already-built ELF (O-78/O-79/O-80).

The eighth performance pass attributes executable bytes to owners by symbol
name, measures the function-size tail, and counts repeated RELATIVE addends.
This tool makes those measurements reproducible on a new artifact without
changing the artifact or the linker.  It reads ``.symtab`` directly and
decodes either ``.rela.dyn`` or a RELR ``.relr.dyn`` table.  For RELR it reads
the 8-byte value already present at each relocated target as the implicit
RELATIVE addend; this is a static file read, not loader execution.

The output is static only: it is not a runtime profile (PM-13) and it makes no
speed claim (PM-2/PM-3/PM-17).  Missing artifacts are reported as
``unavailable`` and exit with status 2.

Usage:
  python tools/pass8_static_audit.py \
      --so build/android_app/release/cmake/libwc3gameactivity.so \
      --json reports/generated/pass8_static_release.json
  python tools/pass8_static_audit.py --so ... --compare baseline.so
  python tools/pass8_static_audit.py --self-test
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from icf_audit import (
    ElfError,
    SHF_EXECINSTR,
    SHN_ABS,
    SHN_COMMON,
    SHN_UNDEF,
    SHT_RELA,
    SHT_STRTAB,
    SHT_SYMTAB,
    Section,
    Symbol,
    _u16,
    _u32,
    _u64,
    read_sections,
)
from plt_audit import (
    decode_b,
    decode_bl,
    function_ranges,
    owner_name,
    plt_slot,
    plt_symbol_names,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SO = ROOT / "build" / "android_app" / "release" / "cmake" / \
    "libwc3gameactivity.so"
R_AARCH64_RELATIVE = 1027


@dataclass(frozen=True)
class Group:
    functions: int
    bytes: int
    share_percent: float


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def classify(name: str) -> str:
    """Map a symbol name to the PASS8 owner bucket.

    The prefixes are the same buckets used by PERFORMANCE_AUDIT_PASS8.md.
    Prefix matching is intentionally coarse: a linker map is required to
    resolve the small number of C++ vendor symbols that do not start with a
    known prefix.
    """
    if name.startswith("L_"):
        return "corpus_L"
    if name.startswith("wc3_dxvk3__"):
        return "dxvk_3_1_1"
    if name.startswith("wc3_dxvk1__"):
        return "dxvk_1_10_3"
    if name.startswith(("wc3_dxvk_bridge", "thin_")):
        return "dxvk_bridge"
    if name.startswith("_Z") and "thin_" in name:
        return "dxvk_bridge"
    if name.startswith("_Z"):
        return "cpp_vendor_runtime"
    if name.startswith(("x87_", "softfloat_")):
        return "cpu_helpers"
    if name.startswith("h_"):
        return "legacy_hle"
    if name.startswith(("__cxx_global_var_init", "__cxa_", "_Unwind_",
                        "__gxx_", "__emutls")):
        return "compiler_runtime"
    if name.startswith(("wc3_", "android_", "Java_", "JNI_")):
        return "host_runtime"
    return "other"


def _nearest_rank(sorted_values: Sequence[int], percent: int) -> int | None:
    if not sorted_values:
        return None
    rank = max(1, math.ceil(percent * len(sorted_values) / 100))
    return sorted_values[min(rank - 1, len(sorted_values) - 1)]


def _percentile(values: Iterable[int], percent: int) -> int | None:
    return _nearest_rank(sorted(values), percent)


def _is_text(section: Section) -> bool:
    return section.name == ".text" or section.name.startswith(".text.")


def _read_all_symbols(data: bytes, sections: Sequence[Section]
                      ) -> list[Symbol]:
    """Read defined FUNC/OBJECT/NOTYPE symbols, including data for addends."""
    table = next((s for s in sections if s.type == SHT_SYMTAB), None)
    if table is None or table.link >= len(sections):
        return []
    strings_section = sections[table.link]
    if strings_section.type != SHT_STRTAB:
        return []
    strings = data[strings_section.offset:
                   strings_section.offset + strings_section.size]
    entsize = table.entsize or 24
    if entsize < 24:
        raise ElfError("symbol entry is too small")
    symbols: list[Symbol] = []
    for i in range(table.size // entsize):
        base = table.offset + i * entsize
        if base + 24 > len(data):
            raise ElfError("symbol table is truncated")
        st_name = _u32(data, base + 0)
        st_info = data[base + 4]
        st_shndx = _u16(data, base + 6)
        st_value = _u64(data, base + 8)
        st_size = _u64(data, base + 16)
        kind = st_info & 0x0F
        if kind in (3, 4):  # STT_SECTION, STT_FILE
            continue
        if st_shndx in (SHN_UNDEF, SHN_ABS, SHN_COMMON):
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


def _text_symbols(symbols: Sequence[Symbol], sections: Sequence[Section]
                  ) -> list[Symbol]:
    executable = {s.index for s in sections
                  if s.flags & SHF_EXECINSTR and _is_text(s)}
    by_address: dict[tuple[int, int], Symbol] = {}
    for symbol in symbols:
        if symbol.section_index not in executable or symbol.size == 0:
            continue
        key = (symbol.section_index, symbol.value)
        current = by_address.get(key)
        if current is None:
            by_address[key] = symbol
            continue
        # An alias at the same VA must not inflate attributed bytes.  Prefer
        # the corpus name when both aliases exist, otherwise keep the larger
        # (usually the authoritative) symbol.
        if (symbol.name.startswith("L_") and
                not current.name.startswith("L_")):
            by_address[key] = symbol
        elif symbol.size > current.size:
            by_address[key] = symbol
    return list(by_address.values())


def _section_bytes(sections: Sequence[Section], name: str) -> int:
    return sum(s.size for s in sections if s.name == name)


def _rela_addends(data: bytes, sections: Sequence[Section]
                  ) -> list[int] | None:
    table = next((s for s in sections
                  if s.type == SHT_RELA and s.name == ".rela.dyn"), None)
    if table is None:
        return None
    if table.entsize < 24:
        raise ElfError("RELATIVE relocation entry is too small")
    count = table.size // table.entsize
    addends: list[int] = []
    for i in range(count):
        base = table.offset + i * table.entsize
        if base + 24 > len(data):
            raise ElfError("RELATIVE relocation table is truncated")
        info = _u64(data, base + 8)
        kind = info & 0xFFFFFFFF
        if kind == R_AARCH64_RELATIVE:
            addends.append(_u64(data, base + 16))
    return addends


def _name_at_addend(addend: int, symbols: Sequence[Symbol]) -> str:
    ordered = sorted((s.value, s.name) for s in symbols if s.name)
    if not ordered:
        return "?"
    addresses = [value for value, _ in ordered]
    index = bisect.bisect_right(addresses, addend) - 1
    if index < 0:
        return "?"
    value, name = ordered[index]
    delta = addend - value
    if delta == 0:
        return name
    return f"{name}+0x{delta:x}"


def _relr_offsets(values: Sequence[int]) -> list[int]:
    addresses: list[int] = []
    next_address = 0
    have_address = False
    for entry in values:
        if not (entry & 1):
            next_address = entry
            have_address = True
            addresses.append(entry)
            next_address += 8
            continue
        if not have_address:
            raise ElfError("RELR bitmap has no preceding address")
        # A bitmap always covers the full 63-word window.  It does not restart
        # at the last set bit; the next bitmap starts after all 63 words.
        for bit in range(63):
            if entry & (1 << (bit + 1)):
                addresses.append(next_address + bit * 8)
        next_address += 63 * 8
    return addresses


def _relr_addends(data: bytes, sections: Sequence[Section]
                  ) -> list[int] | None:
    """Decode RELR offsets and read the RELATIVE addend at each target.

    RELR stores only target addresses.  For ``R_AARCH64_RELATIVE`` the
    loader treats the 8-byte value already present at the target as the
    addend, so a static read of the file can recover the same values.  This is
    still a static measurement; it does not execute the loader.
    """
    table = next((s for s in sections if s.name == ".relr.dyn"), None)
    if table is None:
        return None
    if table.entsize not in (0, 8):
        raise ElfError("RELR entry is not 8 bytes")
    entries = table.size // 8
    values: list[int] = []
    for i in range(entries):
        base = table.offset + i * 8
        if base + 8 > len(data):
            raise ElfError("RELR table is truncated")
        values.append(_u64(data, base))
    addresses = _relr_offsets(values)
    loadable = [s for s in sections
                if s.type != 8 and s.addr != 0 and s.size != 0]
    addends: list[int] = []
    for address in addresses:
        section = next((s for s in loadable
                        if s.addr <= address < s.addr + s.size), None)
        if section is None:
            continue
        offset = section.offset + (address - section.addr)
        if offset + 8 > len(data):
            continue
        addends.append(_u64(data, offset))
    return addends


def _addend_summary(addends: Sequence[int] | None,
                    symbols: Sequence[Symbol], top: int,
                    source: str | None = None) -> dict[str, object]:
    if addends is None:
        return {"available": False, "reason": "no RELA/RELR table"}
    counts = Counter(addends)
    repeated = {addend: count for addend, count in counts.items()
                if count > 1}
    top_rows = []
    for addend, count in sorted(repeated.items(),
                                key=lambda item: (-item[1], item[0]))[:top]:
        top_rows.append({
            "addend": f"0x{addend:x}",
            "count": count,
            "symbol": _name_at_addend(addend, symbols),
        })
    return {
        "available": True,
        "source": source,
        "relocations": len(addends),
        "distinct": len(counts),
        "repeated": len(repeated),
        "relocations_in_repeats": sum(repeated.values()),
        "top": top_rows,
    }


def _needed_libraries(data: bytes, sections: Sequence[Section]) -> list[str]:
    dynamic = next((s for s in sections if s.name == ".dynamic"), None)
    strings_section = next((s for s in sections if s.name == ".dynstr"), None)
    if dynamic is None or strings_section is None:
        return []
    strings = data[strings_section.offset:
                   strings_section.offset + strings_section.size]
    needed: list[str] = []
    entsize = dynamic.entsize or 16
    for i in range(dynamic.size // entsize):
        base = dynamic.offset + i * entsize
        if base + 16 > len(data):
            break
        tag = _u64(data, base)
        value = _u64(data, base + 8)
        if tag != 1:  # DT_NEEDED
            continue
        if value >= len(strings):
            continue
        end = strings.find(b"\0", value)
        if end < 0:
            end = len(strings)
        needed.append(strings[value:end].decode("ascii", errors="replace"))
    return needed


def _import_sites(data: bytes, sections: Sequence[Section],
                  symbols: Sequence[Symbol],
                  targets: Sequence[str]) -> dict[str, object]:
    plt = next((s for s in sections if s.name == ".plt"), None)
    if plt is None:
        return {"available": False, "reason": "no .plt section"}
    names = plt_symbol_names(data, sections)
    if not names:
        return {"available": False, "reason": "no .rela.plt symbols"}
    ranges, starts = function_ranges(sections, _text_symbols(symbols, sections))
    defined = {symbol.value: symbol.name for symbol in symbols
               if symbol.name in targets}
    counts: Counter[str] = Counter()
    owners: dict[str, Counter[str]] = defaultdict(Counter)
    for section in sections:
        if not (section.flags & SHF_EXECINSTR) or not _is_text(section):
            continue
        body = data[section.offset:section.offset + section.size]
        for index in range(0, len(body) & ~3, 4):
            insn = _u32(body, index)
            target = decode_bl(insn, section.addr + index)
            if target is None:
                target = decode_b(insn, section.addr + index)
            if target is None:
                continue
            name: str | None = None
            slot = plt_slot(target, plt.addr, plt.size, len(names))
            if slot is not None:
                name = names[slot]
            elif target in defined:
                name = defined[target]
            if name is None or name not in targets:
                continue
            owner = owner_name(section.addr + index, ranges, starts) or "?"
            counts[name] += 1
            owners[name][owner] += 1
    result: dict[str, object] = {}
    for name in targets:
        result[name] = {
            "sites": counts.get(name, 0),
            "owners": [
                {"owner": owner, "calls": calls}
                for owner, calls in sorted(
                    owners.get(name, Counter()).items(),
                    key=lambda item: (-item[1], item[0]))
            ],
        }
    return {"available": True, "scan": "BL+B", "targets": result}


def _group_summary(symbols: Sequence[Symbol],
                   text_bytes: int) -> dict[str, object]:
    groups: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    sizes: list[int] = []
    for symbol in symbols:
        group = classify(symbol.name)
        groups[group][0] += 1
        groups[group][1] += symbol.size
        sizes.append(symbol.size)
    attributed = sum(row[1] for row in groups.values())
    rows = {
        name: {
            "functions": values[0],
            "bytes": values[1],
            "share_percent": round(
                100.0 * values[1] / text_bytes, 2) if text_bytes else 0.0,
        }
        for name, values in groups.items()
    }
    return {
        "text_bytes": text_bytes,
        "attributed_bytes": attributed,
        "functions": len(symbols),
        "groups": dict(sorted(rows.items(),
                              key=lambda item: (-item[1]["bytes"],
                                                item[0]))),
        "size_distribution": {
            "p50": _percentile(sizes, 50),
            "p90": _percentile(sizes, 90),
            "p99": _percentile(sizes, 99),
            "max": max(sizes) if sizes else None,
            "over_1k": sum(1 for size in sizes if size > 1024),
            "over_4k": sum(1 for size in sizes if size > 4096),
        },
        "top_functions": [
            {"name": symbol.name, "bytes": symbol.size}
            for symbol in sorted(symbols, key=lambda s: (-s.size, s.name))[:10]
        ],
    }


def _dxvk_intersection(symbols: Sequence[Symbol]) -> dict[str, object]:
    one: dict[str, int] = {}
    three: dict[str, int] = {}
    for symbol in symbols:
        if symbol.name.startswith("wc3_dxvk1__"):
            one[symbol.name[len("wc3_dxvk1__"):]] = symbol.size
        elif symbol.name.startswith("wc3_dxvk3__"):
            three[symbol.name[len("wc3_dxvk3__"):]] = symbol.size
    common = set(one) & set(three)
    return {
        "common_names": len(common),
        "dxvk_1_bytes": sum(one[name] for name in common),
        "dxvk_3_bytes": sum(three[name] for name in common),
        "total_bytes": sum(one[name] + three[name] for name in common),
    }


def audit(path: Path, top: int = 10) -> dict[str, object]:
    data = path.read_bytes()
    sections = read_sections(data)
    symbols = _read_all_symbols(data, sections)
    text = _text_symbols(symbols, sections)
    text_bytes = _section_bytes(sections, ".text")
    relr_bytes = _section_bytes(sections, ".relr.dyn")
    if relr_bytes:
        addends = _relr_addends(data, sections)
        addend_source = "RELR"
    else:
        addends = _rela_addends(data, sections)
        addend_source = "RELA"
    return {
        "schema": "wc3.pass8_static_audit.v1",
        "status": "report-only",
        "artifact": str(path).replace("\\", "/"),
        "artifact_sha256": sha256_bytes(data),
        "artifact_bytes": len(data),
        "sections": {
            "text_bytes": text_bytes,
            "executable_bytes": sum(s.size for s in sections
                                    if s.flags & SHF_EXECINSTR),
            "rela_dyn_bytes": _section_bytes(sections, ".rela.dyn"),
            "relr_dyn_bytes": relr_bytes,
            "symtab_bytes": _section_bytes(sections, ".symtab"),
            "plt_bytes": _section_bytes(sections, ".plt"),
        },
        "text": _group_summary(text, text_bytes),
        "dxvk_intersection": _dxvk_intersection(text),
        "addends": _addend_summary(addends, symbols, top=max(0, top),
                                    source=addend_source),
        "needed_libraries": _needed_libraries(data, sections),
        "import_sites": _import_sites(
            data, sections, symbols,
            ("dlopen", "dlsym", "dladdr", "wc3_vulkan_major_minor")),
    }


def _identity(report: dict[str, object]) -> dict[str, object]:
    return {
        "path": report.get("artifact"),
        "sha256": report.get("artifact_sha256"),
        "bytes": report.get("artifact_bytes"),
    }


def compare_reports(baseline: dict[str, object],
                    candidate: dict[str, object]) -> dict[str, object]:
    """Return static candidate-minus-baseline deltas, never a runtime claim."""
    def delta(key: str) -> int | None:
        base = baseline.get(key)
        cand = candidate.get(key)
        return None if base is None or cand is None else int(cand) - int(base)

    base_text = baseline.get("text", {})
    cand_text = candidate.get("text", {})
    assert isinstance(base_text, dict) and isinstance(cand_text, dict)
    base_groups = base_text.get("groups", {})
    cand_groups = cand_text.get("groups", {})
    assert isinstance(base_groups, dict) and isinstance(cand_groups, dict)
    base_sections = baseline.get("sections", {})
    cand_sections = candidate.get("sections", {})
    assert isinstance(base_sections, dict) and isinstance(cand_sections, dict)
    group_delta = {}
    for name in sorted(set(base_groups) | set(cand_groups)):
        b = base_groups.get(name, {}).get("bytes", 0)
        c = cand_groups.get(name, {}).get("bytes", 0)
        group_delta[name] = int(c) - int(b)
    return {
        "available": True,
        "kind": "static-pass8-delta",
        "baseline": _identity(baseline),
        "candidate": _identity(candidate),
        "delta": {
            "artifact_bytes": delta("artifact_bytes"),
            "text_bytes": (
                int(cand_sections.get("text_bytes", 0)) -
                int(base_sections.get("text_bytes", 0))
            ),
            "relr_dyn_bytes": (
                int(cand_sections.get("relr_dyn_bytes", 0)) -
                int(base_sections.get("relr_dyn_bytes", 0))
            ),
            "groups": group_delta,
        },
    }


def print_report(report: dict[str, object]) -> None:
    print(f"artifact: {report['artifact']}")
    print(f"  sha256 {report['artifact_sha256']}")
    print(f"  bytes  {report['artifact_bytes']}")
    text = report["text"]
    assert isinstance(text, dict)
    print(f"text: {text['text_bytes']} B, "
          f"{text['functions']} functions, "
          f"{text['attributed_bytes']} B attributed")
    for name, row in text["groups"].items():
        print(f"  {name:24s} {row['functions']:>7d} fn "
              f"{row['bytes']:>10d} B {row['share_percent']:>6.2f}%")
    print("size distribution:", text["size_distribution"])
    print("top functions:")
    for row in text["top_functions"]:
        print(f"  {row['bytes']:>8d} B {row['name']}")
    dxvk = report["dxvk_intersection"]
    assert isinstance(dxvk, dict)
    print("dxvk intersection: "
          f"{dxvk['common_names']} names, {dxvk['total_bytes']} B")
    addends = report["addends"]
    assert isinstance(addends, dict)
    if not addends.get("available"):
        print(f"addends: unavailable ({addends.get('reason')})")
    else:
        print(f"addends: {addends['relocations']} relocations, "
              f"{addends['distinct']} distinct, "
              f"{addends['repeated']} repeated")
        for row in addends["top"]:
            print(f"  {row['addend']:>12s} {row['count']:>6d} "
                  f"{row['symbol']}")
    needed = report.get("needed_libraries")
    if isinstance(needed, list):
        print("needed:", ", ".join(needed) if needed else "(none)")
    imports = report.get("import_sites")
    if isinstance(imports, dict) and imports.get("available"):
        print("probe imports (BL+B):")
        for name, row in imports["targets"].items():
            owners = ", ".join(
                f"{item['owner']}={item['calls']}"
                for item in row["owners"][:4]) or "-"
            print(f"  {name:28s} {row['sites']:>4d}  {owners}")


def run_self_test() -> int:
    if classify("L_00401000") != "corpus_L":
        print("pass8 self-test: L_ prefix failed")
        return 1
    if classify("wc3_dxvk3__Zfoo") != "dxvk_3_1_1":
        print("pass8 self-test: dxvk3 prefix failed")
        return 1
    if classify("_Z3foov") != "cpp_vendor_runtime":
        print("pass8 self-test: C++ prefix failed")
        return 1
    if classify("_Z10thin_countj") != "dxvk_bridge":
        print("pass8 self-test: thin bridge prefix failed")
        return 1
    if classify("wc3_boot") != "host_runtime":
        print("pass8 self-test: host prefix failed")
        return 1
    if _percentile([1, 2, 3, 4], 50) != 2:
        print("pass8 self-test: percentile failed")
        return 1
    if _percentile([], 50) is not None:
        print("pass8 self-test: empty percentile failed")
        return 1
    if _relr_offsets([0x1000, 1 | (1 << 1) | (1 << 3)]) != [
            0x1000, 0x1008, 0x1018]:
        print("pass8 self-test: RELR bitmap decode failed")
        return 1
    # After a bitmap, the next bitmap starts after the full 63-word window.
    if _relr_offsets([0x1000, 1 | (1 << 1), 1 | (1 << 1)]) != [
            0x1000, 0x1008, 0x1200 + 0x0]:
        print("pass8 self-test: RELR bitmap window advance failed")
        return 1
    try:
        _relr_offsets([1])
    except ElfError:
        pass
    else:
        print("pass8 self-test: orphan RELR bitmap accepted")
        return 1
    symbols = [Symbol("base", 0x1000, 0x10, 1),
               Symbol("other", 0x2000, 0x10, 1)]
    addends = [0x1000, 0x1000, 0x1008, 0x2000]
    summary = _addend_summary(addends, symbols, top=2)
    if (summary["relocations"] != 4 or summary["distinct"] != 3 or
            summary["repeated"] != 1 or
            summary["relocations_in_repeats"] != 2 or
            summary["top"][0]["symbol"] != "base"):
        print("pass8 self-test: addend summary failed")
        return 1
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
                        help="number of repeated addends to print")
    parser.add_argument("--self-test", action="store_true",
                        help="check pure helper logic without an ELF artifact")
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        code = run_self_test()
        print("pass8 static audit self-test: OK" if code == 0
              else "pass8 static audit self-test: FAILED")
        return code

    if not args.so.is_file():
        print(f"pass8 static audit: unavailable (missing artifact: {args.so})")
        return 2
    baseline: dict[str, object] | None = None
    try:
        if args.compare:
            if not args.compare.is_file():
                print("pass8 static audit: unavailable (missing baseline: "
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
        print(f"pass8 static audit: unavailable ({exc})")
        return 2

    print_report(report)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
