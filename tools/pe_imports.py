#!/usr/bin/env python3
"""Read PE32 import/export directories without third-party packages.

The licensed 1.27.1.7085 files live outside the repository
(`E:\\Games\\Warcraft III` by default).  This tool writes only names, ordinals,
IAT slot VAs, export indexes and file hashes; it never copies game code or data.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME_DIR = Path(r"E:\Games\Warcraft III")
DEFAULT_MODULES = [
    "war3.exe",
    "game.dll",
    "Storm.dll",
    "Mss32.dll",
    "ijl15.dll",
    "blizzard.ax",
    "redist/miles/Mssfast.m3d",
    "redist/miles/Mp3dec.asi",
]
# MD5 is the identity used by config/game127.json in the legacy tree.  Only
# the five required 1.27.1.7085 modules have a published identity; the Miles
# codecs and the DirectShow filter are auxiliary and are recorded by hash but
# not gated against a hard-coded value.
EXPECTED_MD5 = {
    "war3.exe": "bf41f6a99ff34640dae440ad2a8425b7",
    "game.dll": "614e3e604294eaffef8f9b78fab2b86b",
    "Storm.dll": "07d37aa7b3fdbc2ac36af6aeccb83eda",
    "Mss32.dll": "b75a8e0ddeeb4330c1dba37105244b0f",
    "ijl15.dll": "b29185440ea3348079be873db85b50f1",
}
# Fixed target bases from runtime/guest/guest_layout.h.  The PE import table
# stores RVAs, so every slot written into a mapped image must be rebased the
# same way the image is.  Keeping the original PE VA in the fixture makes a
# rebase bug visible instead of silently binding a slot in the wrong module.
TARGET_BASE = {
    "war3.exe": 0x00400000,
    "game.dll": 0x6F000000,
    "Storm.dll": 0x35000000,
    "Mss32.dll": 0x35100000,
    "redist/miles/Mssfast.m3d": 0x35200000,
    "redist/miles/Mp3dec.asi": 0x35300000,
    "ijl15.dll": 0x60000000,
}


@dataclass(frozen=True)
class Import:
    slot_va: int
    module: str
    name: str | None
    ordinal: int | None


@dataclass(frozen=True)
class Export:
    ordinal: int
    name: str


@dataclass(frozen=True)
class ExportIndex:
    """Export names (original case) and every non-zero export ordinal.

    Import binding uses GetProcAddress semantics: names are case-sensitive and
    an ordinal is valid only when its function RVA is non-zero.
    """

    names: dict[str, int]
    ordinals: frozenset[int]


def digests(path: Path) -> tuple[str, str]:
    md5 = hashlib.md5()
    sha = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            md5.update(chunk)
            sha.update(chunk)
    return md5.hexdigest(), sha.hexdigest()


def parse_export_index(path: Path) -> ExportIndex:
    """Read the export directory as a name->ordinal map plus ordinal set."""
    data = path.read_bytes()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not PE")
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        raise ValueError("not PE32")
    export_rva, export_size = struct.unpack_from("<II", data, opt + 96)
    size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
    if export_rva == 0 or export_size == 0:
        return ExportIndex(names={}, ordinals=frozenset())

    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        o = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        sections.append((vaddr, vsize, rawptr, rawsize))

    def rva_to_off(rva: int) -> int:
        if rva < size_of_headers:
            return rva
        for vaddr, vsize, rawptr, rawsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                delta = rva - vaddr
                if delta >= rawsize:
                    raise ValueError(f"RVA {rva:08X} points into zero-fill")
                return rawptr + delta
        raise ValueError(f"RVA {rva:08X} outside sections")

    def cstr(rva: int) -> str:
        o = rva_to_off(rva)
        z = data.find(b"\0", o)
        if z < 0:
            raise ValueError(f"unterminated string at RVA {rva:08X}")
        return data[o:z].decode("latin1")

    exp = rva_to_off(export_rva)
    base = struct.unpack_from("<I", data, exp + 16)[0]
    nfunc = struct.unpack_from("<I", data, exp + 20)[0]
    nnames = struct.unpack_from("<I", data, exp + 24)[0]
    funcs_rva = struct.unpack_from("<I", data, exp + 28)[0]
    names_rva = struct.unpack_from("<I", data, exp + 32)[0]
    ords_rva = struct.unpack_from("<I", data, exp + 36)[0]

    names: dict[str, int] = {}
    ordinals: set[int] = set()
    for i in range(nfunc):
        rva = struct.unpack_from("<I", data, rva_to_off(funcs_rva + i * 4))[0]
        # A non-zero RVA inside the export directory is a forwarder string;
        # wc3_pe_export() refuses those, so do not claim them as lifted.
        if rva and not (export_rva <= rva < export_rva + export_size):
            ordinals.add(base + i)
    for i in range(nnames):
        name_rva = struct.unpack_from("<I", data, rva_to_off(names_rva + i * 4))[0]
        index = struct.unpack_from("<H", data, rva_to_off(ords_rva + i * 2))[0]
        if index < nfunc:
            ordinal = base + index
            if ordinal in ordinals:
                names[cstr(name_rva)] = ordinal
    return ExportIndex(names=names, ordinals=frozenset(ordinals))


def parse_exports(path: Path) -> list[Export]:
    """Read named exports; ordinal-only exports cannot be mapped to a name."""
    index = parse_export_index(path)
    return [Export(ordinal=ordinal, name=name) for name, ordinal in index.names.items()]


def image_info(path: Path) -> tuple[int, int]:
    """Return (preferred image base, SizeOfImage) from a PE32 header."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not an MZ file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not PE")
    coff = e_lfanew + 4
    opt = coff + 20
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        raise ValueError("not PE32")
    return (
        struct.unpack_from("<I", data, opt + 28)[0],
        struct.unpack_from("<I", data, opt + 56)[0],
    )


def build_fixture(game_dir: Path, corpus: str, modules: list[str] | None = None) -> dict:
    """Return the PE-derived import fixture without writing game data."""
    modules = modules or DEFAULT_MODULES
    files = []
    raw_entries: list[tuple[str, Import, int]] = []
    export_maps: dict[str, dict[int, str]] = {}
    export_index: dict[str, dict[str, list]] = {}
    for rel in modules:
        path = game_dir / rel
        if not path.exists():
            raise FileNotFoundError(f"missing module: {path}")
        imports = parse_imports(path)
        index = parse_export_index(path)
        export_maps[path.name.lower()] = {
            ordinal: name for name, ordinal in index.names.items()
        }
        export_index[path.name.lower()] = {
            "names": sorted(index.names),
            "ordinals": sorted(index.ordinals),
        }
        pe_base, image_size = image_info(path)
        target_base = TARGET_BASE.get(rel)
        if target_base is None:
            target_base = pe_base
        md5, sha = digests(path)
        expected = EXPECTED_MD5.get(rel)
        files.append(
            {
                "name": rel,
                "md5": md5,
                "sha256": sha,
                "size": path.stat().st_size,
                "preferred_base": pe_base,
                "image_size": image_size,
                "target_base": target_base,
                "rebased": target_base != pe_base,
                "target_in_scope": rel in TARGET_BASE,
                "imports": len(imports),
                "exports": len(index.names),
                "hash_pinned": expected is not None,
                "hash_verified": expected is not None and md5 == expected,
            }
        )
        delta = target_base - pe_base
        raw_entries.extend((rel, imp, delta) for imp in imports)

    entries = []
    for rel, imp, delta in raw_entries:
        imported_by_ordinal = imp.name is None
        name = imp.name
        if name is None and imp.ordinal is not None:
            module_file = imp.module.replace("\\", "/").rsplit("/", 1)[-1].lower()
            name = export_maps.get(module_file, {}).get(imp.ordinal)
        entries.append(
            {
                "file": rel,
                "slot_va": (imp.slot_va + delta) & 0xFFFFFFFF,
                "pe_slot_va": imp.slot_va,
                "module": imp.module,
                "name": name,
                "ordinal": imp.ordinal,
                "imported_by_ordinal": imported_by_ordinal,
            }
        )
    return {
        "schema": 1,
        "corpus": corpus,
        "pe_fixture_status": "available",
        "game_dir_basename": game_dir.name,
        "files": files,
        "exports": export_index,
        "total": len(entries),
        "entries": entries,
    }


def parse_imports(path: Path) -> list[Import]:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not an MZ file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not PE")
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise ValueError(f"not PE32 (magic 0x{magic:04X})")
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    import_rva = struct.unpack_from("<I", data, opt + 104)[0]
    size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
    if import_rva == 0:
        return []

    sections = []
    sec = opt + opt_size
    for i in range(nsec):
        o = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        sections.append((vaddr, vsize, rawptr, rawsize))

    def rva_to_off(rva: int) -> int:
        if rva < size_of_headers:
            return rva
        for vaddr, vsize, rawptr, rawsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                delta = rva - vaddr
                if delta >= rawsize:
                    raise ValueError(f"RVA {rva:08X} points into zero-fill")
                return rawptr + delta
        raise ValueError(f"RVA {rva:08X} outside sections")

    def cstr(rva: int) -> str:
        o = rva_to_off(rva)
        z = data.find(b"\0", o)
        if z < 0:
            raise ValueError(f"unterminated string at RVA {rva:08X}")
        return data[o:z].decode("latin1")

    out: list[Import] = []
    desc = rva_to_off(import_rva)
    while True:
        if desc + 20 > len(data):
            raise ValueError("import descriptor past EOF")
        ilt, _ts, _fc, name_rva, iat = struct.unpack_from("<IIIII", data, desc)
        if not (ilt or name_rva or iat):
            break
        module = cstr(name_rva)
        thunk_rva = ilt or iat
        i = 0
        while True:
            o = rva_to_off(thunk_rva + i * 4)
            value = struct.unpack_from("<I", data, o)[0]
            if value == 0:
                break
            if value & 0x80000000:
                name = None
                ordinal = value & 0xFFFF
            else:
                name = cstr(value + 2)
                ordinal = None
            out.append(
                Import(
                    slot_va=image_base + iat + i * 4,
                    module=module,
                    name=name,
                    ordinal=ordinal,
                )
            )
            i += 1
        desc += 20
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR)
    parser.add_argument("--module", action="append", dest="modules")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    modules = args.modules or DEFAULT_MODULES
    try:
        payload = build_fixture(args.game_dir, args.corpus, modules)
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    bad = [f["name"] for f in payload["files"] if f["hash_pinned"] and not f["hash_verified"]]
    if bad:
        print(f"hash mismatch for required module(s): {', '.join(bad)}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    pinned = sum(1 for f in payload["files"] if f["hash_pinned"])
    verified = sum(1 for f in payload["files"] if f["hash_verified"])
    print(
        f"PE imports: {payload['total']} slots from {len(payload['files'])} files "
        f"({verified}/{pinned} pinned hashes verified) -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
