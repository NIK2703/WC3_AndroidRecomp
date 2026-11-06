#!/usr/bin/env python3
"""Validate the frozen guest address-space manifest against the C layout.

The manifest is not a tuning knob.  Image bases are part of the lifted corpus:
changing one changes every absolute address in the generated C.  This checker
therefore refuses a stale manifest, an unlisted overlap, or a corpus-fixed
image whose hash no longer matches the PE fixture.

The single deliberate overlap is the HLE thunk window inside VA4.  Thunks are
call targets intercepted by dispatch(); they are not committed memory.  The
manifest has to spell that out, and every other overlap is an error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HEADER = ROOT / "runtime" / "guest" / "guest_layout.h"
DEFAULT_MANIFEST = ROOT / "config" / "layout.1.27b.json"
PE_FIXTURE = ROOT / "config" / "pe_imports.1.27b.json"

FOUR_GB = 0x100000000
PAGE = 0x1000
MACRO_RE = re.compile(r"^\s*#define\s+(GUEST_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*$")

# Manifest image name -> layout macro.  The set is closed: a new image needs a
# layout decision before the checker can accept it.
IMAGE_MACROS = {
    "war3.exe": "GUEST_IMAGE_WAR3",
    "game.dll": "GUEST_IMAGE_GAME",
    "Storm.dll": "GUEST_IMAGE_STORM",
    "Mss32.dll": "GUEST_IMAGE_MSS",
    "Mssfast.m3d": "GUEST_IMAGE_MSSFAST",
    "Mp3dec.asi": "GUEST_IMAGE_MP3DEC",
    "ijl15.dll": "GUEST_IMAGE_IJL",
}


def read_macros(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = MACRO_RE.match(line)
        if not match:
            continue
        name, raw = match.groups()
        values[name] = int(raw, 0)
    return values


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as fh:
        value = json.load(fh)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: top-level JSON value is not an object")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def interval(base: int, size: int) -> tuple[int, int]:
    return base, base + size


def overlaps(a: tuple[int, int], b: tuple[int, int]) -> bool:
    return a[0] < b[1] and b[0] < a[1]


def align_ok(base: int, align: int) -> bool:
    return align > 0 and (align & (align - 1)) == 0 and base % align == 0


class Checker:
    def __init__(self, macros: dict[str, int], manifest: dict,
                 require_corpus: bool = False) -> None:
        self.macros = macros
        self.manifest = manifest
        self.require_corpus = require_corpus
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.regions: dict[str, tuple[int, int]] = {}
        self.reserved: list[tuple[str, tuple[int, int]]] = []
        self.hle: tuple[int, int] = (0, 0)

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def need(self, value: object, what: str) -> int:
        if not isinstance(value, int) or isinstance(value, bool):
            self.error(f"{what}: expected integer, got {value!r}")
            return 0
        return value

    def macro(self, name: str) -> int:
        if name not in self.macros:
            self.error(f"guest_layout.h is missing {name}")
            return 0
        return self.macros[name]

    def add_region(self, name: str, base: int, size: int, align: int = PAGE) -> None:
        if base <= 0 or size <= 0:
            self.error(f"{name}: empty region {base:#x}+{size:#x}")
            return
        if base + size > FOUR_GB:
            self.error(f"{name}: {base:#x}+{size:#x} crosses 4 GiB")
        if not align_ok(base, align):
            self.error(f"{name}: base {base:#x} is not aligned to {align:#x}")
        if size % PAGE:
            self.error(f"{name}: size {size:#x} is not page aligned")
        self.regions[name] = interval(base, size)

    def check_header(self) -> None:
        for name in ("GUEST_IMAGE_WAR3", "GUEST_IMAGE_GAME", "GUEST_IMAGE_STORM",
                     "GUEST_IMAGE_MSS", "GUEST_IMAGE_MSSFAST", "GUEST_IMAGE_MP3DEC",
                     "GUEST_IMAGE_IJL", "GUEST_HLE_BASE", "GUEST_HLE_STRIDE",
                     "GUEST_HLE_MAX", "GUEST_THREAD_MAX"):
            self.macro(name)
        if self.macro("GUEST_HLE_BASE") + self.macro("GUEST_HLE_MAX") * self.macro(
            "GUEST_HLE_STRIDE") > FOUR_GB:
            self.error("HLE thunk window crosses 4 GiB")
        if self.macro("GUEST_HLE_STRIDE") % 4:
            self.error("HLE thunk stride is not 4-byte aligned")

    def check_top_level(self) -> None:
        if self.manifest.get("target") != "android":
            self.error("manifest target must be 'android'")
        if self.manifest.get("corpus") != "1.27b":
            self.error("manifest corpus must be '1.27b'")
        if self.manifest.get("layout_rev") != 2:
            self.error("manifest layout_rev must be 2")
        if self.manifest.get("identity_below_4g") is not True:
            self.error("manifest must declare identity_below_4g=true")

    def check_reserved(self) -> None:
        reserved = self.manifest.get("android_reserved", [])
        if not isinstance(reserved, list):
            self.error("android_reserved must be a list")
            return
        self.reserved: list[tuple[str, tuple[int, int]]] = []
        for index, item in enumerate(reserved):
            if not isinstance(item, dict):
                self.error(f"android_reserved[{index}] is not an object")
                continue
            name = str(item.get("name", f"reserved[{index}]"))
            base = self.need(item.get("base"), f"{name}.base")
            size = self.need(item.get("size"), f"{name}.size")
            if size > 0:
                self.reserved.append((name, interval(base, size)))

    def check_images(self) -> None:
        images = self.manifest.get("images")
        if not isinstance(images, list) or not images:
            self.error("images must be a non-empty list")
            return
        pe = None
        if PE_FIXTURE.exists():
            try:
                pe = load_json(PE_FIXTURE)
            except (OSError, ValueError) as exc:
                self.error(f"cannot read {PE_FIXTURE}: {exc}")
        pe_files = {}
        if isinstance(pe, dict):
            pe_files = {
                str(item.get("name", "")).replace("\\", "/"): item
                for item in pe.get("files", [])
                if isinstance(item, dict)
            }
        pe_by_basename = {
            Path(key).name.lower(): item for key, item in pe_files.items()
        }
        if pe is not None and pe.get("pe_fixture_status") != "available":
            self.error("PE fixture is not available")

        seen: set[str] = set()
        for index, item in enumerate(images):
            if not isinstance(item, dict):
                self.error(f"images[{index}] is not an object")
                continue
            name = str(item.get("name", ""))
            if not name:
                self.error(f"images[{index}] has no name")
                continue
            if name in seen:
                self.error(f"image {name} is listed twice")
            seen.add(name)
            base = self.need(item.get("base"), f"image {name}.base")
            size = self.need(item.get("size"), f"image {name}.size")
            self.add_region(f"image:{name}", base, size)
            if name not in IMAGE_MACROS:
                self.error(f"image {name} has no frozen layout macro")
                continue
            expected_base = self.macro(IMAGE_MACROS[name])
            if base != expected_base:
                self.error(
                    f"image {name}: manifest base {base:#x} != guest_layout.h "
                    f"{expected_base:#x}; this requires a re-lift"
                )
            if item.get("corpus_fixed") is not True:
                self.error(f"image {name}: corpus_fixed must be true")
            sha = str(item.get("sha256", ""))
            if not re.fullmatch(r"[0-9a-f]{64}", sha):
                self.error(f"image {name}: sha256 is missing or malformed")
            source = item.get("source")
            if isinstance(source, str) and source:
                source_path = Path(source)
                if not source_path.exists():
                    self.error(f"image {name}: source does not exist: {source}")
                elif sha256_file(source_path) != sha:
                    self.error(f"image {name}: source sha256 mismatch")
            if name in pe_files:
                fixture = pe_files[name]
            else:
                fixture = pe_by_basename.get(Path(name).name.lower())
            if fixture is not None:
                if fixture.get("target_in_scope") is not True:
                    self.error(f"image {name}: PE fixture says target_in_scope=false")
                if fixture.get("sha256") != sha:
                    self.error(f"image {name}: sha256 differs from PE fixture")
                if fixture.get("image_size") != size:
                    self.error(f"image {name}: image_size differs from PE fixture")
                if fixture.get("target_base") != base:
                    self.error(f"image {name}: target_base differs from PE fixture")
            else:
                self.warn(f"image {name}: not present in the PE fixture")

        expected = set(IMAGE_MACROS)
        missing = sorted(expected - seen)
        if missing:
            self.error("manifest is missing images: " + ", ".join(missing))

    def check_out_of_scope(self) -> None:
        entries = self.manifest.get("out_of_scope", [])
        if not isinstance(entries, list):
            self.error("out_of_scope must be a list")
            return
        pe_files: dict[str, dict] = {}
        if PE_FIXTURE.exists():
            try:
                pe = load_json(PE_FIXTURE)
            except (OSError, ValueError):
                pe = {}
            pe_files = {
                str(item.get("name", "")).replace("\\", "/"): item
                for item in pe.get("files", [])
                if isinstance(item, dict)
            }
        for index, item in enumerate(entries):
            if not isinstance(item, dict):
                self.error(f"out_of_scope[{index}] is not an object")
                continue
            name = str(item.get("name", ""))
            status = item.get("status")
            if status not in ("excluded", "pending"):
                self.error(f"out_of_scope {name}: status must be excluded or pending")
            if name in IMAGE_MACROS:
                self.error(f"out_of_scope {name} is also a loadable image")
            fixture = pe_files.get(name)
            if fixture is not None and fixture.get("target_in_scope") is not False:
                self.error(f"out_of_scope {name}: PE fixture still says in scope")
            if status == "pending":
                self.warn(f"out_of_scope {name}: decision pending")

    def check_arenas(self) -> None:
        arenas = self.manifest.get("arenas")
        if not isinstance(arenas, list) or not arenas:
            self.error("arenas must be a non-empty list")
            return
        for index, item in enumerate(arenas):
            if not isinstance(item, dict):
                self.error(f"arenas[{index}] is not an object")
                continue
            name = str(item.get("name", f"arena[{index}]"))
            base = self.need(item.get("base"), f"arena {name}.base")
            size = self.need(item.get("size"), f"arena {name}.size")
            align = self.need(item.get("align", PAGE), f"arena {name}.align")
            self.add_region(name, base, size, align or PAGE)

    def check_va_arenas(self) -> None:
        arenas = self.manifest.get("va_arenas")
        if not isinstance(arenas, list) or len(arenas) != 7:
            self.error("va_arenas must contain the seven VirtualAlloc arenas")
            return
        for index, item in enumerate(arenas):
            if not isinstance(item, dict):
                self.error(f"va_arenas[{index}] is not an object")
                continue
            name = str(item.get("name", f"va[{index}]"))
            base = self.need(item.get("base"), f"va {name}.base")
            size = self.need(item.get("size"), f"va {name}.size")
            align = self.need(item.get("align", PAGE), f"va {name}.align")
            self.add_region(name, base, size, align or PAGE)

    def check_threads(self) -> None:
        stack = self.manifest.get("stack")
        if not isinstance(stack, dict):
            self.error("stack must be an object")
        else:
            base = self.need(stack.get("base"), "stack.base")
            size = self.need(stack.get("size"), "stack.size")
            align = self.need(stack.get("align", PAGE), "stack.align")
            self.add_region("stack", base, size, align or PAGE)
            if base != self.macro("GUEST_STACK_BASE"):
                self.error("stack.base differs from GUEST_STACK_BASE")
            if size != self.macro("GUEST_STACK_SIZE"):
                self.error("stack.size differs from GUEST_STACK_SIZE")

        threads = self.manifest.get("thread_stacks")
        if not isinstance(threads, dict):
            self.error("thread_stacks must be an object")
        else:
            base = self.need(threads.get("base"), "thread_stacks.base")
            size = self.need(threads.get("size"), "thread_stacks.size")
            stride = self.need(threads.get("stride"), "thread_stacks.stride")
            count = self.need(threads.get("count"), "thread_stacks.count")
            if count != self.macro("GUEST_THREAD_MAX"):
                self.error("thread_stacks.count differs from GUEST_THREAD_MAX")
            if size != self.macro("GUEST_THREAD_STACK_SIZE"):
                self.error("thread_stacks.size differs from GUEST_THREAD_STACK_SIZE")
            if base != self.macro("GUEST_THREAD_STACK_BASE"):
                self.error("thread_stacks.base differs from GUEST_THREAD_STACK_BASE")
            if stride < size:
                self.error("thread_stacks.stride is smaller than one stack")
            self.add_region("thread_stacks", base, stride * count)

        tebs = self.manifest.get("thread_tebs")
        if not isinstance(tebs, dict):
            self.error("thread_tebs must be an object")
        else:
            base = self.need(tebs.get("base"), "thread_tebs.base")
            size = self.need(tebs.get("size"), "thread_tebs.size")
            stride = self.need(tebs.get("stride"), "thread_tebs.stride")
            count = self.need(tebs.get("count"), "thread_tebs.count")
            if count != self.macro("GUEST_THREAD_MAX"):
                self.error("thread_tebs.count differs from GUEST_THREAD_MAX")
            if base != self.macro("GUEST_THREAD_TEB_BASE"):
                self.error("thread_tebs.base differs from GUEST_THREAD_TEB_BASE")
            if size != PAGE:
                self.error("thread_tebs.size must be one page")
            if stride < size:
                self.error("thread_tebs.stride is smaller than one TEB")
            self.add_region("thread_tebs", base, stride * count)

    def check_hle(self) -> None:
        base = self.macro("GUEST_HLE_BASE")
        count = self.macro("GUEST_HLE_MAX")
        slot = self.macro("GUEST_HLE_STRIDE")
        self.hle = (base, base + count * slot)
        thunks = self.manifest.get("hle_thunks")
        if not isinstance(thunks, dict):
            self.error("hle_thunks must be an object")
            return
        base = self.need(thunks.get("base"), "hle_thunks.base")
        count = self.need(thunks.get("count"), "hle_thunks.count")
        slot = self.need(thunks.get("slot_size"), "hle_thunks.slot_size")
        if base != self.macro("GUEST_HLE_BASE"):
            self.error("hle_thunks.base differs from GUEST_HLE_BASE")
        if count != self.macro("GUEST_HLE_MAX"):
            self.error("hle_thunks.count differs from GUEST_HLE_MAX")
        if slot != self.macro("GUEST_HLE_STRIDE"):
            self.error("hle_thunks.slot_size differs from GUEST_HLE_STRIDE")
        if base + count * slot > FOUR_GB:
            self.error("hle_thunks window crosses 4 GiB")

    def check_guards(self) -> None:
        guards = self.manifest.get("guard_pages", [])
        if not isinstance(guards, list):
            self.error("guard_pages must be a list")
            return
        for index, item in enumerate(guards):
            if not isinstance(item, dict):
                self.error(f"guard_pages[{index}] is not an object")
                continue
            name = str(item.get("name", f"guard[{index}]"))
            base = self.need(item.get("base"), f"{name}.base")
            size = self.need(item.get("size"), f"{name}.size")
            if size <= 0:
                self.error(f"{name}: guard size must be positive")
                continue
            span = interval(base, size)
            if base % PAGE or size % PAGE:
                self.error(f"{name}: guard is not page aligned")
            for other_name, other_span in self.regions.items():
                if overlaps(span, other_span):
                    self.error(f"{name}: guard overlaps {other_name}")
            for reserved_name, reserved_span in self.reserved:
                if overlaps(span, reserved_span):
                    self.error(f"{name}: guard overlaps reserved {reserved_name}")

    def check_overlaps(self) -> None:
        names = sorted(self.regions)
        allowed: set[frozenset[str]] = set()
        for index, item in enumerate(self.manifest.get("overlaps", [])):
            if not isinstance(item, dict):
                self.error(f"overlaps[{index}] is not an object")
                continue
            a = str(item.get("a", ""))
            b = str(item.get("b", ""))
            known = set(self.regions) | {"hle_thunks"}
            if a not in known or b not in known:
                self.error(f"overlaps[{index}] names an unknown region")
                continue
            if item.get("kind") not in ("synthetic_thunks", "guard"):
                self.error(f"overlaps[{index}]: unknown kind")
            if not item.get("reason"):
                self.error(f"overlaps[{index}]: reason is required")
            allowed.add(frozenset((a, b)))
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                if overlaps(self.regions[a], self.regions[b]):
                    if frozenset((a, b)) not in allowed:
                        self.error(f"unlisted overlap: {a} and {b}")

        # The thunk window is synthetic, but it still may not sit inside an
        # image, a stack, a heap or a TEB.  Only a VirtualAlloc arena may
        # contain it, and only when the manifest says so explicitly.
        for name, span in self.regions.items():
            if not overlaps(self.hle, span):
                continue
            if frozenset((name, "hle_thunks")) not in allowed:
                self.error(f"hle_thunks overlaps {name} without an explicit carve-out")

        for reserved_name, reserved_span in self.reserved:
            for name, span in self.regions.items():
                if overlaps(span, reserved_span):
                    self.error(f"{name} overlaps reserved {reserved_name}")
            if overlaps(self.hle, reserved_span):
                self.error(f"hle_thunks overlaps reserved {reserved_name}")

    def check_corpus(self) -> None:
        corpus = self.manifest.get("corpus_manifest")
        if not isinstance(corpus, dict):
            self.error("corpus_manifest must be an object")
            return
        if corpus.get("layout_rev") != self.manifest.get("layout_rev"):
            self.error("corpus_manifest.layout_rev differs from layout_rev")
        sha = corpus.get("sha256")
        if sha is None:
            if self.require_corpus:
                self.error("corpus_manifest.sha256 is required by --require-corpus")
            elif corpus.get("status") != "not_generated":
                self.error("corpus_manifest.sha256 may be null only when status=not_generated")
            else:
                self.warn("corpus manifest is not generated yet; images are checked against the PE fixture")
        elif not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
            self.error("corpus_manifest.sha256 is malformed")

    def run(self) -> int:
        self.check_top_level()
        self.check_header()
        self.check_reserved()
        self.check_images()
        self.check_out_of_scope()
        self.check_arenas()
        self.check_va_arenas()
        self.check_threads()
        self.check_hle()
        self.check_guards()
        self.check_overlaps()
        self.check_corpus()
        for warning in self.warnings:
            print(f"layout: warning: {warning}", file=sys.stderr)
        for error in self.errors:
            print(f"layout: error: {error}", file=sys.stderr)
        if self.errors:
            print(f"layout: FAILED ({len(self.errors)} error(s))", file=sys.stderr)
            return 1
        print(f"layout: OK ({len(self.regions)} mapped regions, "
              f"hle {self.hle[0]:#x}..{self.hle[1]:#x})")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--corpus", default="1.27b")
    parser.add_argument("--require-corpus", action="store_true",
                        help="fail when the corpus manifest has no sha256 yet")
    args = parser.parse_args(argv)
    if args.corpus != "1.27b":
        print(f"layout: error: unknown corpus {args.corpus}", file=sys.stderr)
        return 1
    try:
        macros = read_macros(args.header)
        manifest = load_json(args.manifest)
    except (OSError, ValueError) as exc:
        print(f"layout: error: {exc}", file=sys.stderr)
        return 1
    if args.require_corpus:
        manifest.setdefault("corpus_manifest", {})
    return Checker(macros, manifest, require_corpus=args.require_corpus).run()


if __name__ == "__main__":
    raise SystemExit(main())
