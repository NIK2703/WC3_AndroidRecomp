#!/usr/bin/env python3
"""Normalize the compiler path for the MSYS2 ccache binary (BP-12).

cmake/Wc3CompilerCache.cmake installs this script between CMake and ccache on
Windows.  CMake writes the NDK compiler as
``E:\\Android\\...\\clang.exe``; MSYS2 ccache treats backslashes as part of a
single executable name and fails with "Could not find compiler ... in PATH".
The wrapper changes only that path separator and then execs ccache normally.
"""
from __future__ import annotations

import subprocess
import sys


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: ccache_wrapper.py <ccache> <compiler> [args...]",
              file=sys.stderr)
        return 2
    ccache = argv[0].replace("\\", "/")
    compiler = argv[1].replace("\\", "/")
    return subprocess.run([ccache, compiler, *argv[2:]]).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
