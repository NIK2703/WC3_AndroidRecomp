#!/usr/bin/env python3
"""Symbolize WC3 native crash addresses from the unstripped build .symtab.

O-42 release builds default to `WC3_HIDE_DYNSYM=ON`, so the shipped .so keeps
only the JNI surface in `.dynsym` and the legacy `crash_sym` handler can no
longer resolve an `L_<va>` name through dladdr.  The unstripped build .so still
has `.symtab`;
this tool maps a crash PC back to the link-time symbol with llvm-nm.  Debug
builds keep the full `.dynsym` and can still use the on-device path.  The tool
does not run the game and does not build anything.

The crash log usually contains absolute ASLR addresses.  Pass the module load
base from /proc/self/maps or the tombstone (`--base 0x...`).  Without --base the
addresses are treated as file offsets.
"""
from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from pathlib import Path

NM_LINE_RE = re.compile(
    r"^(?P<name>\S+)\s+(?P<type>[A-Za-z])\s+"
    r"(?P<value>[0-9a-fA-F]+)(?:\s+(?P<size>[0-9a-fA-F]+))?\s*$"
)
ADDRESS_CONTEXT_RE = re.compile(
    r"(?i)\b(?:host\s+)?(?:pc|lr|bt|address|addr)\b"
)
HEX_RE = re.compile(
    r"(?i)\b(?:0x([0-9a-f]{1,16})|([0-9a-f]{8,16}))\b"
)


def load_symbols(so: Path, nm: str) -> tuple[list[int], list[tuple[int, int, str]]]:
    cmd = [nm, "--defined-only", "--format=posix", str(so)]
    try:
        proc = subprocess.run(
            cmd, check=True, text=True, capture_output=True, encoding="utf-8",
            errors="replace",
        )
    except FileNotFoundError:
        raise SystemExit(f"llvm-nm not found: {nm}")
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"llvm-nm failed ({exc.returncode}): {exc.stderr.strip()}")

    entries: list[tuple[int, int, str]] = []
    for line in proc.stdout.splitlines():
        m = NM_LINE_RE.match(line.strip())
        if not m or m.group("type") not in "TtWw":
            continue
        value = int(m.group("value"), 16)
        size = int(m.group("size"), 16) if m.group("size") else 0
        entries.append((value, size, m.group("name")))

    if not entries:
        raise SystemExit(f"no text symbols in {so}; is .symtab preserved?")

    entries.sort(key=lambda item: (item[0], item[2]))
    values = [item[0] for item in entries]
    return values, entries


def lookup(address: int, base: int, values: list[int],
           entries: list[tuple[int, int, str]]) -> tuple[str, int] | None:
    offset = address - base
    index = bisect.bisect_right(values, offset) - 1
    if index < 0:
        return None
    value, _size, name = entries[index]
    return name, offset - value


def iter_addresses(line: str) -> list[int]:
    if not ADDRESS_CONTEXT_RE.search(line):
        return []
    return [int(match.group(1) or match.group(2), 16)
            for match in HEX_RE.finditer(line)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", required=True, type=Path,
                        help="unstripped build .so with .symtab")
    parser.add_argument("--base", default="0", type=lambda text: int(text, 0),
                        help="module load base; default 0 (addresses are offsets)")
    parser.add_argument("--nm", default="llvm-nm",
                        help="llvm-nm executable (default: llvm-nm)")
    parser.add_argument("log", nargs="?", default="-",
                        help="crash log path, or - for stdin")
    args = parser.parse_args(argv)

    values, entries = load_symbols(args.so, args.nm)
    stream = sys.stdin if args.log == "-" else open(args.log, encoding="utf-8",
                                                     errors="replace")
    found = 0
    try:
        for line in stream:
            addresses = iter_addresses(line)
            if not addresses:
                continue
            print(line.rstrip())
            for address in addresses:
                result = lookup(address, args.base, values, entries)
                if result is None:
                    print(f"  {address:#018x} -> ?")
                    continue
                name, delta = result
                print(f"  {address:#018x} -> {name}+{delta:#x}")
                found += 1
    finally:
        if stream is not sys.stdin:
            stream.close()

    if not found:
        print("no crash addresses matched; use a log line with pc/lr/bt/address",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
