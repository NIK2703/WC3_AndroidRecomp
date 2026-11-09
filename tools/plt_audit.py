#!/usr/bin/env python3
"""Static PLT call-site audit for an already-built ELF (O-65).

``-fno-plt`` replaces external ``bl <plt>`` calls with a GOT load and ``blr``.
Whether that is faster is a runtime question (PM-8/PM-19), so this tool only
publishes the static side of the comparison: the PLT slot count, the number of
``bl`` instructions that target those slots, the contiguous ``adrp/ldr/blr``
GOT call sites, and the per-symbol call counts.  It never enables
``WC3_FNO_PLT`` and never writes to the artifact.

The parser is self-contained (ELF64 little-endian, AArch64): no NDK or
``objdump`` is required.  ``.rela.plt`` supplies the slot-to-symbol mapping;
the first 32 bytes of ``.plt`` are the resolver header and each following
16-byte slot belongs to one relocation in order.  ``.rela.dyn`` supplies the
GOT slot-to-symbol mapping for the indirect form.  The GOT scan accepts the
contiguous compiler sequence ``adrp xN, page; ldr xN, [xN, #off]; blr xN`` and
deliberately does not treat arbitrary register copies as a GOT call.

Usage:
  python tools/plt_audit.py --so build/android_app/debug/cmake/libwc3gameactivity.so \
      --json reports/generated/plt_audit.json
  python tools/plt_audit.py --so candidate.so --compare baseline.so
  python tools/plt_audit.py --self-test
"""
from __future__ import annotations

import argparse
import bisect
import json
import struct
from collections import Counter
from pathlib import Path
from typing import Sequence

from icf_audit import (
    DEFAULT_SO,
    ElfError,
    Section,
    Symbol,
    SHF_EXECINSTR,
    SHT_DYNSYM,
    SHT_RELA,
    _u32,
    _u64,
    read_sections,
    read_symbols,
    sha256_bytes,
)

AARCH64_BL_MASK = 0xFC000000
AARCH64_BL = 0x94000000
AARCH64_ADRP_MASK = 0x9F000000
AARCH64_ADRP = 0x90000000
AARCH64_LDR_UNSIGNED_MASK = 0xFFC00000
AARCH64_LDR_UNSIGNED = 0xF9400000
AARCH64_BLR_MASK = 0xFFFFFC1F
AARCH64_BLR = 0xD63F0000
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
PLT_HEADER_BYTES = 0x20
PLT_ENTRY_BYTES = 16


def decode_bl(insn: int, pc: int) -> int | None:
    """Return the branch target for an AArch64 ``BL`` instruction."""
    if (insn & AARCH64_BL_MASK) != AARCH64_BL:
        return None
    imm = insn & 0x03FFFFFF
    if imm & 0x02000000:
        imm -= 0x04000000
    return pc + (imm << 2)


def decode_adrp(insn: int, pc: int) -> tuple[int, int] | None:
    """Return ``(destination_register, page_address)`` for ``ADRP``."""
    if (insn & AARCH64_ADRP_MASK) != AARCH64_ADRP:
        return None
    immlo = (insn >> 29) & 0x3
    immhi = (insn >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    page = (pc & ~0xFFF) + (imm << 12)
    return insn & 0x1F, page & 0xFFFFFFFFFFFFFFFF


def decode_ldr_unsigned(insn: int) -> tuple[int, int, int] | None:
    """Return ``(target_register, base_register, byte_offset)`` for LDR."""
    if (insn & AARCH64_LDR_UNSIGNED_MASK) != AARCH64_LDR_UNSIGNED:
        return None
    target = insn & 0x1F
    base = (insn >> 5) & 0x1F
    offset = ((insn >> 10) & 0xFFF) * 8
    return target, base, offset


def decode_blr(insn: int) -> int | None:
    """Return the register called by ``BLR``, or ``None``."""
    if (insn & AARCH64_BLR_MASK) != AARCH64_BLR:
        return None
    return (insn >> 5) & 0x1F


def plt_slot(target: int, plt_addr: int, plt_size: int, slots: int) -> int | None:
    """Map a branch target to its ``.rela.plt`` slot, or ``None``."""
    if target < plt_addr or target >= plt_addr + plt_size:
        return None
    delta = target - (plt_addr + PLT_HEADER_BYTES)
    if delta < 0 or delta % PLT_ENTRY_BYTES:
        return None
    index = delta // PLT_ENTRY_BYTES
    return index if index < slots else None


def _symbol_names(data: bytes, sections: Sequence[Section],
                  table: Section) -> list[str]:
    if table.link >= len(sections):
        raise ElfError("symbol table has no string table link")
    strings_section = sections[table.link]
    strings = data[strings_section.offset:
                   strings_section.offset + strings_section.size]
    entsize = table.entsize or 24
    if entsize < 24:
        raise ElfError("symbol entry is too small")
    names: list[str] = []
    for i in range(table.size // entsize):
        base = table.offset + i * entsize
        if base + 24 > len(data):
            raise ElfError("symbol table is truncated")
        offset = _u32(data, base)
        if offset >= len(strings):
            names.append("")
            continue
        end = strings.find(b"\0", offset)
        if end < 0:
            end = len(strings)
        names.append(strings[offset:end].decode("ascii", errors="replace"))
    return names


def plt_symbol_names(data: bytes,
                     sections: Sequence[Section]) -> list[str]:
    """Return symbols in ``.rela.plt`` order (one per PLT slot)."""
    rela = next((s for s in sections
                 if s.name == ".rela.plt" and s.type == SHT_RELA), None)
    if rela is None:
        return []
    if rela.link >= len(sections):
        raise ElfError(".rela.plt has no symbol table link")
    table = sections[rela.link]
    if table.type != SHT_DYNSYM:
        raise ElfError(".rela.plt does not link a dynamic symbol table")
    names = _symbol_names(data, sections, table)
    entsize = rela.entsize or 24
    if entsize < 24:
        raise ElfError(".rela.plt entry is too small")
    result: list[str] = []
    for i in range(rela.size // entsize):
        base = rela.offset + i * entsize
        if base + 24 > len(data):
            raise ElfError(".rela.plt is truncated")
        info = _u64(data, base + 8)
        symbol = info >> 32
        result.append(names[symbol] if symbol < len(names)
                      else f"<sym:{symbol}>")
    return result


def got_symbol_map(data: bytes,
                   sections: Sequence[Section]) -> dict[int, str]:
    """Return GOT address -> external symbol from ``.rela.dyn``/``.rela.plt``."""
    result: dict[int, str] = {}
    for rela in sections:
        if rela.type != SHT_RELA or rela.name not in (".rela.dyn", ".rela.plt"):
            continue
        if rela.link >= len(sections):
            raise ElfError(f"{rela.name} has no symbol table link")
        table = sections[rela.link]
        if table.type != SHT_DYNSYM:
            raise ElfError(f"{rela.name} does not link a dynamic symbol table")
        names = _symbol_names(data, sections, table)
        entsize = rela.entsize or 24
        if entsize < 24:
            raise ElfError(f"{rela.name} entry is too small")
        for i in range(rela.size // entsize):
            base = rela.offset + i * entsize
            if base + 24 > len(data):
                raise ElfError(f"{rela.name} is truncated")
            offset = _u64(data, base)
            info = _u64(data, base + 8)
            reloc_type = info & 0xFFFFFFFF
            symbol = info >> 32
            if reloc_type not in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
                continue
            if symbol < len(names) and names[symbol]:
                result[offset] = names[symbol]
    return result


def function_ranges(
        sections: Sequence[Section], symbols: Sequence[Symbol]
) -> tuple[list[tuple[int, int, str]], list[int]]:
    ranges: list[tuple[int, int, str]] = []
    for symbol in symbols:
        if symbol.section_index >= len(sections):
            continue
        section = sections[symbol.section_index]
        if not (section.flags & SHF_EXECINSTR):
            continue
        if symbol.value < section.addr:
            continue
        offset = symbol.value - section.addr
        if offset + symbol.size > section.size:
            continue
        ranges.append((symbol.value, symbol.value + symbol.size, symbol.name))
    ranges.sort()
    return ranges, [start for start, _, _ in ranges]


def owner_name(address: int, ranges: Sequence[tuple[int, int, str]],
               starts: Sequence[int]) -> str | None:
    index = bisect.bisect_right(starts, address) - 1
    if index < 0:
        return None
    start, end, name = ranges[index]
    return name if start <= address < end else None


def top_calls(counts: Counter[str], in_l: Counter[str],
              limit: int) -> list[dict[str, object]]:
    ordered = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
    return [{
        "sym": name,
        "calls": calls,
        "in_L_functions": in_l.get(name, 0),
    } for name, calls in ordered[:limit]]


def top_owners(owners: Counter[str], limit: int) -> list[dict[str, object]]:
    ordered = sorted(owners.items(), key=lambda item: (-item[1], item[0]))
    return [{"owner": owner, "calls": calls}
            for owner, calls in ordered[:limit]]


def scan_got_calls(body: bytes, base: int, got: dict[int, str],
                   ranges: Sequence[tuple[int, int, str]],
                   starts: Sequence[int]
                   ) -> tuple[Counter[str], Counter[str], int, Counter[str]]:
    """Count contiguous ``adrp/ldr/blr`` calls through a mapped GOT slot.

    The compiler normally emits that exact three-instruction sequence for
    ``-fno-plt``.  Any other instruction clears the tracked registers, so a
    stale page/address cannot turn an unrelated indirect call into a match.
    """
    counts: Counter[str] = Counter()
    in_l: Counter[str] = Counter()
    owners: Counter[str] = Counter()
    total = 0
    adrp_regs: dict[int, int] = {}
    got_regs: dict[int, int] = {}
    usable = len(body) & ~3
    for index in range(0, usable, 4):
        pc = base + index
        insn = _u32(body, index)
        adrp = decode_adrp(insn, pc)
        if adrp is not None:
            register, page = adrp
            adrp_regs[register] = page
            got_regs.pop(register, None)
            continue
        ldr = decode_ldr_unsigned(insn)
        if ldr is not None:
            target, source, offset = ldr
            page = adrp_regs.get(source)
            address = page + offset if page is not None else None
            if address is not None and address in got:
                got_regs[target] = address
            else:
                got_regs.pop(target, None)
            continue
        called = decode_blr(insn)
        if called is not None:
            address = got_regs.get(called)
            if address is not None:
                name = got[address]
                counts[name] += 1
                total += 1
                owner = owner_name(pc, ranges, starts)
                if owner is not None and owner.startswith("L_"):
                    in_l[name] += 1
                owners[owner or "?"] += 1
            adrp_regs.clear()
            got_regs.clear()
            continue
        adrp_regs.clear()
        got_regs.clear()
    return counts, in_l, total, owners


def collect_calls(data: bytes, sections: Sequence[Section],
                  plt_addr: int, plt_size: int, names: Sequence[str],
                  ranges: Sequence[tuple[int, int, str]],
                  starts: Sequence[int],
                  emutls_addr: int | None,
                  got: dict[int, str],
                  top: int) -> dict[str, object]:
    counts: Counter[str] = Counter()
    in_l: Counter[str] = Counter()
    got_counts: Counter[str] = Counter()
    got_in_l: Counter[str] = Counter()
    got_owners: Counter[str] = Counter()
    emutls_owners: Counter[str] = Counter()
    total = 0
    got_total = 0
    emutls_calls = 0
    emutls_in_l = 0

    for section in sections:
        if not (section.flags & SHF_EXECINSTR):
            continue
        body = memoryview(data)[section.offset:section.offset + section.size]
        usable = len(body) & ~3
        pc = section.addr
        for (insn,) in struct.iter_unpack("<I", body[:usable]):
            target = decode_bl(insn, pc)
            caller = pc
            pc += 4
            if target is None:
                continue
            slot = plt_slot(target, plt_addr, plt_size, len(names))
            if slot is not None:
                name = names[slot]
                total += 1
                counts[name] += 1
                owner = owner_name(caller, ranges, starts)
                if owner is not None and owner.startswith("L_"):
                    in_l[name] += 1
                continue
            if emutls_addr is not None and target == emutls_addr:
                emutls_calls += 1
                owner = owner_name(caller, ranges, starts)
                if owner is not None and owner.startswith("L_"):
                    emutls_in_l += 1
                emutls_owners[owner or "?"] += 1

        section_got_counts, section_got_in_l, section_got_total, section_owners = (
            scan_got_calls(body, section.addr, got, ranges, starts))
        got_counts.update(section_got_counts)
        got_in_l.update(section_got_in_l)
        got_total += section_got_total
        got_owners.update(section_owners)

    all_counts = Counter(counts)
    all_counts.update(got_counts)
    all_in_l = Counter(in_l)
    all_in_l.update(got_in_l)
    return {
        "plt_entries": len(names),
        "plt_symbols_called": len(counts),
        "total_plt_calls": total,
        "calls_by_symbol": [{"sym": name, "calls": calls}
                            for name, calls in sorted(counts.items())],
        "top_calls": top_calls(counts, in_l, top),
        "got_entries": len(got),
        "got_symbols_called": len(got_counts),
        "total_got_calls": got_total,
        "got_calls_by_symbol": [{"sym": name, "calls": calls}
                                for name, calls in sorted(got_counts.items())],
        "top_got_calls": top_calls(got_counts, got_in_l, top),
        "top_got_owners": top_owners(got_owners, 12),
        "total_external_calls": total + got_total,
        "all_calls_by_symbol": [{"sym": name, "calls": calls}
                                for name, calls in sorted(all_counts.items())],
        "top_external_calls": top_calls(all_counts, all_in_l, top),
        "emutls_bl": emutls_calls,
        "emutls_in_L": emutls_in_l,
        "top_emutls_owners": top_owners(emutls_owners, 12),
    }


def audit(path: Path, top: int = 12) -> dict[str, object]:
    data = path.read_bytes()
    sections = read_sections(data)
    plt = next((s for s in sections if s.name == ".plt"), None)
    if plt is None:
        raise ElfError("no .plt section")
    names = plt_symbol_names(data, sections)
    got = got_symbol_map(data, sections)
    symbols = read_symbols(data, sections)
    ranges, starts = function_ranges(sections, symbols)
    emutls = next((s for s in symbols
                   if s.name == "__emutls_get_address"), None)
    emutls_addr = emutls.value if emutls is not None else None
    calls = collect_calls(data, sections, plt.addr, plt.size, names,
                          ranges, starts, emutls_addr, got, top)
    return {
        "schema": "wc3.plt_audit.v2",
        "status": "report-only",
        "artifact": str(path).replace("\\", "/"),
        "artifact_sha256": sha256_bytes(data),
        "artifact_bytes": len(data),
        "plt_bytes": plt.size,
        "plt_addr": hex(plt.addr),
        "emutls_defined_addr": hex(emutls_addr)
        if emutls_addr is not None else None,
        "limits": [
            "counts AArch64 BL instructions whose target is a .plt slot",
            "counts contiguous adrp/ldr/blr sequences whose GOT address is a "
            "GLOB_DAT/JUMP_SLOT relocation",
            "GOT-indirect calls through register copies or non-contiguous "
            "sequences are not counted",
            "static call-site counts are not runtime weights (PM-17)",
        ],
        **calls,
    }


def _delta(baseline: object, candidate: object) -> int:
    return int(candidate) - int(baseline)


def _call_map(report: dict[str, object]) -> dict[str, int]:
    raw = report.get("all_calls_by_symbol", report.get("calls_by_symbol", {}))
    if isinstance(raw, dict):
        return {str(name): int(calls) for name, calls in raw.items()}
    if isinstance(raw, list):
        return {str(item["sym"]): int(item["calls"])
                for item in raw if isinstance(item, dict)}
    return {}


def compare_reports(baseline: dict[str, object],
                    candidate: dict[str, object]) -> dict[str, object]:
    """Return a static candidate-minus-baseline external-call delta."""
    base_counts = _call_map(baseline)
    cand_counts = _call_map(candidate)
    names = sorted(set(base_counts) | set(cand_counts))
    changes = [{
        "sym": name,
        "baseline": int(base_counts.get(name, 0)),
        "candidate": int(cand_counts.get(name, 0)),
        "delta": _delta(base_counts.get(name, 0),
                        cand_counts.get(name, 0)),
    } for name in names]
    changes = [item for item in changes if item["delta"]]
    changes.sort(key=lambda item: (-abs(int(item["delta"])), str(item["sym"])))
    return {
        "available": True,
        "kind": "static-plt-delta",
        "baseline": {
            "path": baseline.get("artifact"),
            "sha256": baseline.get("artifact_sha256"),
            "bytes": baseline.get("artifact_bytes"),
        },
        "candidate": {
            "path": candidate.get("artifact"),
            "sha256": candidate.get("artifact_sha256"),
            "bytes": candidate.get("artifact_bytes"),
        },
        "delta": {
            "plt_entries": _delta(baseline.get("plt_entries", 0),
                                  candidate.get("plt_entries", 0)),
            "plt_symbols_called": _delta(
                baseline.get("plt_symbols_called", 0),
                candidate.get("plt_symbols_called", 0)),
            "total_plt_calls": _delta(baseline.get("total_plt_calls", 0),
                                      candidate.get("total_plt_calls", 0)),
            "got_symbols_called": _delta(
                baseline.get("got_symbols_called", 0),
                candidate.get("got_symbols_called", 0)),
            "total_got_calls": _delta(baseline.get("total_got_calls", 0),
                                      candidate.get("total_got_calls", 0)),
            "total_external_calls": _delta(
                baseline.get("total_external_calls", 0),
                candidate.get("total_external_calls", 0)),
            "emutls_bl": _delta(baseline.get("emutls_bl", 0),
                                candidate.get("emutls_bl", 0)),
        },
        "top_changed_symbols": changes[:20],
    }


def print_comparison(comparison: dict[str, object]) -> None:
    if not comparison.get("available"):
        return
    baseline = comparison["baseline"]  # type: ignore[index]
    candidate = comparison["candidate"]  # type: ignore[index]
    delta = comparison["delta"]  # type: ignore[index]
    print("comparison (candidate - baseline, static external calls):")
    print(f"  baseline  {baseline['sha256']} {baseline['bytes']} B")
    print(f"  candidate {candidate['sha256']} {candidate['bytes']} B")
    for key, value in delta.items():  # type: ignore[union-attr]
        print(f"  {key:20s} {int(value):+d}")
    changes = comparison.get("top_changed_symbols", [])
    if changes:
        print("  changed call sites:")
        for item in changes:  # type: ignore[union-attr]
            print(f"    {int(item['delta']):+6d} {item['sym']} "
                  f"({item['baseline']} -> {item['candidate']})")
    else:
        print("  changed call sites: none")


def print_report(report: dict[str, object]) -> None:
    print(f"artifact: {report['artifact']}")
    print(f"  sha256 {report['artifact_sha256']}")
    print(f"  bytes  {report['artifact_bytes']}")
    print(f"plt: {report['plt_entries']} slots at {report['plt_addr']}, "
          f"{report['plt_bytes']} B")
    print(f"  {report['plt_symbols_called']} symbols called, "
          f"{report['total_plt_calls']} BL sites")
    print(f"  GOT: {report['got_symbols_called']} symbols called, "
          f"{report['total_got_calls']} indirect blr sites")
    print(f"  external: {report['total_external_calls']} call sites "
          f"(BL + GOT)")
    if report["emutls_defined_addr"] is not None:
        print(f"  __emutls_get_address: {report['emutls_defined_addr']}, "
              f"{report['emutls_bl']} direct BL sites "
              f"({report['emutls_in_L']} in L_* functions)")
    print("top PLT calls:")
    for item in report["top_calls"]:  # type: ignore[union-attr]
        print(f"  {item['calls']:>6d} {item['sym']} "
              f"(L_* {item['in_L_functions']})")
    if report["top_emutls_owners"]:  # type: ignore[union-attr]
        print("top __emutls_get_address owners:")
        for item in report["top_emutls_owners"]:  # type: ignore[union-attr]
            print(f"  {item['calls']:>6d} {item['owner']}")


def run_self_test() -> int:
    if decode_bl(0x94000000, 0x1000) != 0x1000:
        print("plt audit self-test: BL +0 failed")
        return 1
    if decode_bl(0x97FFFFFC, 0x1000) != 0xFF0:
        print("plt audit self-test: BL -4 failed")
        return 1
    if decode_bl(0x14000000, 0x1000) is not None:
        print("plt audit self-test: non-BL decoded as call")
        return 1
    if plt_slot(0x1010, 0x1000, 0x40, 2) is not None:
        print("plt audit self-test: PLT header mapped as slot")
        return 1
    if plt_slot(0x1020, 0x1000, 0x40, 2) != 0:
        print("plt audit self-test: first PLT slot not mapped")
        return 1
    if plt_slot(0x1030, 0x1000, 0x40, 2) != 1:
        print("plt audit self-test: second PLT slot not mapped")
        return 1
    if plt_slot(0x1040, 0x1000, 0x40, 2) is not None:
        print("plt audit self-test: out-of-range PLT slot accepted")
        return 1

    adrp = 0x90000000 | (1 << 29) | 16
    if decode_adrp(adrp, 0x1000) != (16, 0x2000):
        print("plt audit self-test: ADRP page decode failed")
        return 1
    if decode_adrp(0xD503201F, 0x1000) is not None:
        print("plt audit self-test: non-ADRP decoded as ADRP")
        return 1
    ldr = 0xF9400000 | (16 << 5) | 16 | (2 << 10)
    if decode_ldr_unsigned(ldr) != (16, 16, 16):
        print("plt audit self-test: LDR unsigned decode failed")
        return 1
    if decode_ldr_unsigned(0xD503201F) is not None:
        print("plt audit self-test: non-LDR decoded as LDR")
        return 1
    blr = 0xD63F0000 | (16 << 5)
    if decode_blr(blr) != 16 or decode_blr(0xD503201F) is not None:
        print("plt audit self-test: BLR decode failed")
        return 1

    got = {0x2010: "memcpy"}
    got_ranges = [(0x1000, 0x100C, "L_00001000")]
    got_starts = [start for start, _, _ in got_ranges]
    body = struct.pack("<III", adrp, ldr, blr)
    got_counts, got_in_l, got_total, got_owners = scan_got_calls(
        body, 0x1000, got, got_ranges, got_starts)
    if (got_total != 1 or got_counts.get("memcpy") != 1 or
            got_in_l.get("memcpy") != 1 or got_owners.get("L_00001000") != 1):
        print("plt audit self-test: contiguous GOT call not counted")
        return 1
    body = struct.pack("<IIII", adrp, 0xD503201F, ldr, blr)
    _, _, got_total, _ = scan_got_calls(body, 0x1000, got, [], [])
    if got_total != 0:
        print("plt audit self-test: stale GOT register survived NOP")
        return 1
    if _call_map({"all_calls_by_symbol": [{"sym": "x", "calls": 3}],
                  "calls_by_symbol": [{"sym": "x", "calls": 1}]}) != {"x": 3}:
        print("plt audit self-test: combined call map not preferred")
        return 1

    ranges = [(0x2000, 0x2010, "L_00002000"),
              (0x3000, 0x3010, "host_call")]
    starts = [start for start, _, _ in ranges]
    if owner_name(0x2004, ranges, starts) != "L_00002000":
        print("plt audit self-test: L_* owner lookup failed")
        return 1
    if owner_name(0x2010, ranges, starts) is not None:
        print("plt audit self-test: owner range leaked past its end")
        return 1

    counts = Counter({"memset": 3, "memcpy": 7})
    in_l = Counter({"memcpy": 2})
    top = top_calls(counts, in_l, 2)
    if (not top or top[0]["sym"] != "memcpy" or top[0]["calls"] != 7 or
            top[0]["in_L_functions"] != 2 or top[1]["sym"] != "memset"):
        print("plt audit self-test: top-call ordering failed")
        return 1

    baseline = {
        "artifact": "baseline.so",
        "artifact_sha256": "a" * 64,
        "artifact_bytes": 100,
        "plt_entries": 2,
        "plt_symbols_called": 2,
        "total_plt_calls": 10,
        "got_symbols_called": 1,
        "total_got_calls": 4,
        "total_external_calls": 14,
        "emutls_bl": 4,
        "calls_by_symbol": [{"sym": "memset", "calls": 3},
                            {"sym": "memcpy", "calls": 7}],
    }
    candidate = {
        "artifact": "candidate.so",
        "artifact_sha256": "b" * 64,
        "artifact_bytes": 120,
        "plt_entries": 1,
        "plt_symbols_called": 1,
        "total_plt_calls": 5,
        "got_symbols_called": 2,
        "total_got_calls": 8,
        "total_external_calls": 13,
        "emutls_bl": 8,
        "calls_by_symbol": [{"sym": "memset", "calls": 5}],
    }
    comparison = compare_reports(baseline, candidate)
    delta = comparison["delta"]  # type: ignore[index]
    changes = comparison["top_changed_symbols"]  # type: ignore[index]
    if (delta["total_plt_calls"] != -5 or delta["emutls_bl"] != 4 or
            delta["total_got_calls"] != 4 or
            delta["total_external_calls"] != -1 or
            delta["got_symbols_called"] != 1 or
            delta["plt_entries"] != -1 or not changes or
            changes[0]["sym"] != "memcpy" or changes[0]["delta"] != -7 or
            changes[1]["sym"] != "memset" or changes[1]["delta"] != 2):
        print("plt audit self-test: comparison delta failed")
        return 1

    print("plt audit self-test: OK")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="static PLT call-site audit for AArch64 ELF64")
    parser.add_argument("--so", type=Path, default=DEFAULT_SO,
                        help="already-built ELF64 AArch64 shared object")
    parser.add_argument("--compare", type=Path,
                        help="baseline ELF for a static PLT delta")
    parser.add_argument("--json", type=Path,
                        help="write the full report as JSON")
    parser.add_argument("--top", type=int, default=12,
                        help="number of top callers to print")
    parser.add_argument("--self-test", action="store_true",
                        help="check instruction/PLT mapping without an artifact")
    args = parser.parse_args(list(argv) if argv is not None else None)

    if args.self_test:
        return run_self_test()

    if not args.so.is_file():
        print(f"plt audit: unavailable (missing artifact: {args.so})")
        return 2
    baseline: dict[str, object] | None = None
    try:
        if args.compare:
            if not args.compare.is_file():
                print(f"plt audit: unavailable (missing baseline: "
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
        print(f"plt audit: unavailable ({exc})")
        return 2

    print_report(report)
    comparison = report.get("comparison")
    if isinstance(comparison, dict):
        print_comparison(comparison)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
        print(f"report: {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
