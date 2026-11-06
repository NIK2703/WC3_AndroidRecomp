#!/usr/bin/env python3
"""Dispatcher-target closure for the static corpus.

The initial inventory is a static guess.  The authoritative closure evidence is
the generated C itself: every `dispatch(c, 0x...)` and `dispatch_jmp(c, 0x...)`
is a guest control transfer.  Each target that is not already a lifted entry is
added as a basic block and lifted in block mode; the loop repeats until a fixed
point.  This is the same invariant the old tree used, but the catalogue and the
closure evidence are produced here from the PE, not read from a legacy object
directory.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from .discover import Entry, MAX_FUNCTION, write_catalog
from .lift import LiftResult, lift_catalog, scan_targets
from .pe_image import PeImage


@dataclasses.dataclass
class ClosureResult:
    entries: list[Entry]
    chunks: list[Path]
    rounds: int
    added: int
    c_paths: list[Path]


def extent_for(image: PeImage, va: int, known: set[int]) -> int:
    rng = next((r for r in image.code_ranges() if r.contains(va)), None)
    if rng is None:
        return 0
    nxt = [x for x in known if x > va]
    end = min(nxt) if nxt else rng.end
    return min(max(0, end - va), MAX_FUNCTION)


def close_corpus(
    image_path: str | Path,
    initial: list[Entry],
    out_dir: str | Path,
    *,
    jobs: int = 4,
    max_rounds: int = 24,
) -> ClosureResult:
    out_dir = Path(out_dir)
    image = PeImage(image_path)
    known: dict[int, Entry] = {e.va: e for e in initial}
    all_entries: list[Entry] = list(initial)
    c_paths: list[Path] = []
    chunks: list[Path] = []

    first = lift_catalog(image_path, initial, out_dir, jobs=jobs, tag="lift")
    all_entries = first.catalog
    c_paths.append(first.c_path)
    chunks.extend(first.chunks)
    targets = scan_targets([first.c_path])
    added = 0
    rounds = 0

    pending = sorted(t for t in targets
                     if image.is_code(t) and t not in known)
    while pending and rounds < max_rounds:
        rounds += 1
        new_entries: list[Entry] = []
        for va in pending:
            if va in known:
                continue
            size = extent_for(image, va, set(known))
            if size <= 0:
                continue
            e = Entry(va, size, "closure")
            known[va] = e
            all_entries.append(e)
            new_entries.append(e)
        if not new_entries:
            break
        res = lift_catalog(image_path, new_entries, out_dir,
                           jobs=jobs, block=True, tag=f"closure{rounds:02d}")
        c_paths.append(res.c_path)
        chunks.extend(res.chunks)
        added += len(new_entries)
        targets = scan_targets([res.c_path])
        pending = sorted(t for t in targets
                         if image.is_code(t) and t not in known)

    all_entries.sort(key=lambda e: e.va)
    write_catalog(all_entries, out_dir / "functions.txt")
    # The final list header is the union of every round.  Per-round headers are
    # diagnostics; the dispatcher consumes this one.
    from .lift import write_list
    write_list([e.va for e in all_entries], out_dir / "corpus.list.h")
    (out_dir / "closure.rounds").write_text(f"{rounds}\n{added}\n",
                                            encoding="ascii")
    return ClosureResult(all_entries, chunks, rounds, added, c_paths)
