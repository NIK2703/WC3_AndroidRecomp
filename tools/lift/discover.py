#!/usr/bin/env python3
"""Static function inventory for the canonical corpus.

The old tree discovered functions from runtime probes and from `entry.exe`
(int3 padding).  The canonical tree starts from the image itself:

  * every root the PE declares: entry point, exports, TLS callbacks;
  * every dword in the image that points into executable memory (vtables,
    callback tables, `mov [reg+off], imm32`);
  * every function boundary visible as int3/nop padding or an aligned
    terminator (the same inventory rule as the old tool, kept because MSVC
    emits no unwind data for these PE32 images).

The catalog is then closed after lifting: the generated C contains every
constant `dispatch`/`dispatch_jmp` target, and `close_from_c()` adds any target
that the initial inventory could not see.  This keeps the discovery and lift
stages independent and makes the closure evidence part of the generated
artefact instead of a hidden pre-pass.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from .pe_image import PeImage

TERMINATORS = ("ret", "retn", "retf", "jmp", "iret", "iretd", "hlt")
TERMINATOR_OPCODES = (0xC3, 0xCB)
ALIGN = 16
MAX_FUNCTION = 0x40000  # 256 KiB; longer extents are closed after the lift


@dataclasses.dataclass(frozen=True)
class Entry:
    va: int
    size: int
    source: str

    def line(self) -> str:
        return f"0x{self.va:08x} {self.size} {self.source}_{self.va:08x}"


def _plausible_start(byte: int) -> bool:
    return byte not in (0xCC, 0x90, 0x00)


def _ended_before(data: bytes, offset: int) -> tuple[bool, bool]:
    """Return (is_boundary, must_be_aligned)."""
    if offset <= 0:
        return False, False
    prev = data[offset - 1]
    if prev == 0xCC:
        return True, False
    if prev == 0x90 and offset >= 2 and data[offset - 2] == 0x90:
        return True, False
    if prev == 0xC3:
        return True, True
    if offset >= 3 and data[offset - 3] == 0xC2:
        return True, True
    if offset >= 2 and data[offset - 2] == 0xEB:
        return True, True
    if offset >= 5 and data[offset - 5] == 0xE9:
        return True, True
    if offset >= 6 and data[offset - 6] == 0xFF and data[offset - 5] == 0x25:
        return True, True
    return False, False


def inventory_entries(image: PeImage, min_pad: int = 0) -> dict[int, str]:
    """Every plausible function entry visible from code-section shape."""
    out: dict[int, str] = {}
    for rng in image.code_ranges():
        data = rng.data
        if not data:
            continue
        # A section can start with a function; it has no preceding padding but
        # is still a valid entry when the image root points at it.
        for off in range(1, len(data)):
            is_boundary, aligned = _ended_before(data, off)
            if not is_boundary:
                continue
            if min_pad:
                pad = 0
                while pad < off and data[off - 1 - pad] == 0xCC:
                    pad += 1
                if pad < min_pad:
                    continue
            va = (rng.va + off) & 0xFFFFFFFF
            if aligned and va % ALIGN:
                continue
            if not _plausible_start(data[off]):
                continue
            out[va] = "inventory"
    return out


def image_roots(image: PeImage) -> dict[int, str]:
    out: dict[int, str] = {}
    if image.is_code(image.entry_va):
        out[image.entry_va] = "entry"
    for va in image.exports():
        out[va] = "export"
    for va in image.tls_callbacks():
        out[va] = "tls"
    for va in image.data_refs():
        out[va] = "ref"
    return out


def build_catalog(
    image: PeImage,
    *,
    include_inventory: bool = True,
    include_refs: bool = True,
) -> list[Entry]:
    """Build the initial catalog with extents from the next known entry."""
    starts: dict[int, str] = {}
    roots = image_roots(image) if include_refs else image_roots_without_refs(image)
    starts.update(roots)
    if include_inventory:
        starts.update(inventory_entries(image))

    ordered = sorted(starts)
    code = image.code_ranges()
    out: list[Entry] = []
    for i, va in enumerate(ordered):
        rng = next((r for r in code if r.contains(va)), None)
        if rng is None:
            continue
        if i + 1 < len(ordered) and ordered[i + 1] > va:
            end = min(ordered[i + 1], rng.end)
        else:
            end = rng.end
        size = min(end - va, MAX_FUNCTION)
        if size <= 0:
            continue
        out.append(Entry(va, size, starts[va]))
    return out


def image_roots_without_refs(image: PeImage) -> dict[int, str]:
    out: dict[int, str] = {}
    if image.is_code(image.entry_va):
        out[image.entry_va] = "entry"
    for va in image.exports():
        out[va] = "export"
    for va in image.tls_callbacks():
        out[va] = "tls"
    return out


def write_catalog(entries: list[Entry], path: str | Path) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="ascii", newline="\n") as f:
        for e in entries:
            f.write(e.line() + "\n")
