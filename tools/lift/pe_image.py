#!/usr/bin/env python3
"""PE32 image reader used by the canonical lifter.

This module owns the bytes the lifter sees.  It does not link a PE, execute
it, or import any host-side Windows API: it maps a file-backed image at its
declared VA, exposes the executable sections, and can rewrite the image for a
new preferred base exactly the way the loader would.

The base-rewrite is a build input, not a runtime trick.  GVA() in generated C
is only correct when the image handed to the lifter has already been relocated
to the guest layout's base; `rebase_verify()` proves the transformation is an
involution before the lifter is allowed to use it.
"""

from __future__ import annotations

import dataclasses
import hashlib
import struct
from pathlib import Path
from typing import Iterable

import pefile

PE32_MAGIC = 0x10B
IMAGE_DIR_BASERELOC = 5
IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_SCN_MEM_EXECUTE = 0x20000000
MAX_SECTIONS = 96


class PeError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class Section:
    name: str
    va: int
    vsize: int
    rawsize: int
    rawptr: int
    chars: int

    @property
    def span(self) -> int:
        return max(self.vsize, self.rawsize)

    @property
    def executable(self) -> bool:
        return bool(self.chars & IMAGE_SCN_MEM_EXECUTE)


@dataclasses.dataclass(frozen=True)
class CodeRange:
    name: str
    va: int
    data: bytes

    @property
    def end(self) -> int:
        return self.va + len(self.data)

    def contains(self, va: int) -> bool:
        return self.va <= va < self.end


class PeImage:
    """A read-only PE32 view at its preferred base."""

    def __init__(self, path: str | Path, data: bytes | None = None) -> None:
        self.path = Path(path)
        self.data = bytes(self.path.read_bytes() if data is None else data)
        self._parse_headers()
        # pefile is used only for directory parsing (exports/TLS/relocs).  The
        # section table above stays authoritative because it is the same table
        # the rebase writer edits.
        self.pe = pefile.PE(data=self.data, fast_load=False)
        self.pe.parse_data_directories()
        if self.pe.OPTIONAL_HEADER.Magic != PE32_MAGIC:
            raise PeError(f"{self.path}: not PE32")
        if int(self.pe.OPTIONAL_HEADER.ImageBase) != self.base:
            raise PeError(
                f"{self.path}: section parser base {self.base:#x} differs from "
                f"pefile {int(self.pe.OPTIONAL_HEADER.ImageBase):#x}"
            )

    # ---- headers ---------------------------------------------------------
    def _parse_headers(self) -> None:
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            raise PeError(f"{self.path}: not MZ")
        lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if lfanew + 24 > len(d) or d[lfanew : lfanew + 4] != b"PE\0\0":
            raise PeError(f"{self.path}: not PE")
        nsec = struct.unpack_from("<H", d, lfanew + 6)[0]
        sizeopt = struct.unpack_from("<H", d, lfanew + 20)[0]
        if not 1 <= nsec <= MAX_SECTIONS:
            raise PeError(f"{self.path}: invalid section count {nsec}")
        opt = lfanew + 24
        if opt + 2 > len(d) or struct.unpack_from("<H", d, opt)[0] != PE32_MAGIC:
            raise PeError(f"{self.path}: not PE32 magic")
        if sizeopt < 96 or opt + sizeopt > len(d):
            raise PeError(f"{self.path}: invalid optional header size")
        self.lfanew = lfanew
        self.opt = opt
        self.nsec = nsec
        self.sizeopt = sizeopt
        self.base = struct.unpack_from("<I", d, opt + 28)[0]
        self.image_size = struct.unpack_from("<I", d, opt + 56)[0]
        entry_rva = struct.unpack_from("<I", d, opt + 16)[0]
        self.entry_va = (self.base + entry_rva) & 0xFFFFFFFF
        self.sections: list[Section] = []
        sec_off = opt + sizeopt
        for i in range(nsec):
            o = sec_off + i * 40
            if o + 40 > len(d):
                raise PeError(f"{self.path}: truncated section table")
            raw_name = d[o : o + 8].split(b"\0", 1)[0]
            name = raw_name.decode("latin-1", "replace")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append(
                Section(name, va, vsize, rawsize, rawptr, chars)
            )
        reloc_rva, reloc_size = struct.unpack_from(
            "<II", d, opt + 96 + IMAGE_DIR_BASERELOC * 8
        )
        self.reloc_rva = reloc_rva
        self.reloc_size = reloc_size

    def section_for_va(self, va: int) -> Section | None:
        for s in self.sections:
            start = (self.base + s.va) & 0xFFFFFFFF
            if start <= va < start + s.span:
                return s
        return None

    def code_ranges(self) -> list[CodeRange]:
        out: list[CodeRange] = []
        for s in self.sections:
            if not s.executable or not s.rawsize:
                continue
            start = s.rawptr
            end = start + s.rawsize
            if start < 0 or end > len(self.data):
                raise PeError(f"{self.path}: section {s.name} is outside file")
            out.append(CodeRange(s.name, (self.base + s.va) & 0xFFFFFFFF,
                                 self.data[start:end]))
        return out

    def is_code(self, va: int) -> bool:
        return any(r.contains(va) for r in self.code_ranges())

    def read_va(self, va: int, size: int) -> bytes:
        """Read guest VA bytes, zero-filling the tail of a virtual section."""
        va &= 0xFFFFFFFF
        if va < self.base or va + size > self.base + self.image_size:
            raise PeError(f"{self.path}: VA {va:#x}+{size:#x} outside image")
        s = self.section_for_va(va)
        if s is None:
            return b"\0" * size
        start = (self.base + s.va) & 0xFFFFFFFF
        off = va - start
        raw = self.data[s.rawptr : s.rawptr + s.rawsize]
        if off >= len(raw):
            return b"\0" * size
        out = raw[off : off + size]
        if len(out) < size:
            out += b"\0" * (size - len(out))
        return out

    def read_u32(self, va: int) -> int:
        return struct.unpack("<I", self.read_va(va, 4))[0]

    # ---- directories -----------------------------------------------------
    def reloc_vas(self) -> set[int]:
        out: set[int] = set()
        for block in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for ent in block.entries:
                if ent.type == IMAGE_REL_BASED_HIGHLOW:
                    out.add((self.base + ent.rva) & 0xFFFFFFFF)
        return out

    def exports(self) -> list[int]:
        out: list[int] = []
        directory = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        if directory is None:
            return out
        for sym in directory.symbols:
            if getattr(sym, "forwarder", None):
                continue
            va = (self.base + int(sym.address)) & 0xFFFFFFFF
            if self.is_code(va):
                out.append(va)
        return out

    def tls_callbacks(self) -> list[int]:
        directory = getattr(self.pe, "DIRECTORY_ENTRY_TLS", None)
        if directory is None:
            return []
        va = int(getattr(directory.struct, "AddressOfCallBacks", 0) or 0)
        if not va:
            return []
        out: list[int] = []
        for _ in range(64):
            try:
                cb = self.read_u32(va)
            except PeError:
                break
            if cb == 0:
                break
            if self.is_code(cb):
                out.append(cb)
            va += 4
        return out

    def data_refs(self) -> set[int]:
        """Every dword in the image that points into an executable section.

        Address-taken functions are the closure class a call-graph walk cannot
        see: vtables, descriptor tables, and `mov [eax+off], imm32` callbacks.
        Scanning the raw file is intentional; the loader relocates these dwords
        and the original tool's discovery did the same.
        """
        ranges = self.code_ranges()
        if not ranges:
            return set()
        lo = min(r.va for r in ranges)
        hi = max(r.end for r in ranges)
        out: set[int] = set()
        for s in self.sections:
            if not s.rawsize:
                continue
            raw = self.data[s.rawptr : s.rawptr + s.rawsize]
            # The original discovery scanned every byte, including unaligned
            # pointers.  Keep that behavior: a packed struct can hold one.
            for off in range(0, max(0, len(raw) - 3)):
                v = struct.unpack_from("<I", raw, off)[0]
                if lo <= v < hi:
                    out.add(v)
        return out

    # ---- identity -------------------------------------------------------
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    # ---- rebase ---------------------------------------------------------
    def _rva_to_off(self, rva: int) -> int | None:
        for s in self.sections:
            if s.va <= rva < s.va + s.span:
                return s.rawptr + (rva - s.va)
        return None

    def _relocs(self) -> Iterable[tuple[int, int]]:
        if not self.reloc_rva or not self.reloc_size:
            return
        off = self._rva_to_off(self.reloc_rva)
        if off is None:
            raise PeError(f"{self.path}: relocation directory is not mapped")
        end = off + self.reloc_size
        if end > len(self.data):
            raise PeError(f"{self.path}: truncated relocation directory")
        while off + 8 <= end:
            page, bsize = struct.unpack_from("<II", self.data, off)
            if bsize < 8 or off + bsize > end:
                break
            for i in range((bsize - 8) // 2):
                ent = struct.unpack_from("<H", self.data, off + 8 + i * 2)[0]
                yield page + (ent & 0xFFF), ent >> 12
            off += bsize


def rebase_bytes(data: bytes, new_base: int) -> tuple[bytes, int, int]:
    """Return (relocated bytes, patched dwords, skipped non-HIGHLOW relocs)."""
    img = PeImage.__new__(PeImage)
    img.path = Path("<memory>")
    img.data = bytes(data)
    img._parse_headers()
    delta = (new_base - img.base) & 0xFFFFFFFF
    out = bytearray(img.data)
    patched = 0
    skipped = 0
    for rva, typ in img._relocs():
        if typ == 0:
            continue
        if typ != IMAGE_REL_BASED_HIGHLOW:
            skipped += 1
            continue
        off = img._rva_to_off(rva)
        if off is None or off + 4 > len(out):
            raise PeError(f"{img.path}: relocation RVA {rva:#x} is not mapped")
        v = struct.unpack_from("<I", out, off)[0]
        struct.pack_into("<I", out, off, (v + delta) & 0xFFFFFFFF)
        patched += 1
    struct.pack_into("<I", out, img.opt + 28, new_base)
    return bytes(out), patched, skipped


def rebase_verify(data: bytes, new_base: int) -> tuple[bytes, int, int]:
    """Rebase and prove that rebasing back reproduces the original bytes."""
    original_base = PeImage.__new__(PeImage)
    original_base.path = Path("<memory>")
    original_base.data = bytes(data)
    original_base._parse_headers()
    rebased, patched, skipped = rebase_bytes(data, new_base)
    back, _p, _s = rebase_bytes(rebased, original_base.base)
    if back != data:
        diff = sum(1 for a, b in zip(back, data) if a != b)
        raise PeError(f"rebase verification failed: {diff} bytes differ")
    return rebased, patched, skipped


def rebase_file(src: str | Path, dst: str | Path, new_base: int) -> dict:
    raw = Path(src).read_bytes()
    out, patched, skipped = rebase_verify(raw, new_base)
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(out)
    img = PeImage(dst)
    return {
        "source": str(Path(src).resolve()),
        "output": str(dst.resolve()),
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "sha256": img.sha256(),
        "source_base": PeImage(src).base,
        "base": img.base,
        "size": img.image_size,
        "relocations_patched": patched,
        "relocations_skipped": skipped,
    }
