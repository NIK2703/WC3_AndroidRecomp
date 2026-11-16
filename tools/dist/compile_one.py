#!/usr/bin/env python3
"""Compile one preprocessed C/C++ translation unit received over stdin.

This is the remote half of tools/dist/wc3_dist_cc.py.  It is copied to the
helper machine and is not part of the product build.  The protocol is
length-prefixed and binary-safe:

  request  = u32 header_len | UTF-8 JSON header | u64 data_len | zlib(source)
  response = u32 header_len | UTF-8 JSON header | zlib(object)   (rc == 0)

The helper only accepts a clang/clang++ binary inside WC3_DIST_NDK_ROOT, so a
compromised client key cannot use this endpoint to run arbitrary programs.
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zlib

MAX_HEADER = 1 << 20
MAX_SOURCE = 512 << 20
NDK_ROOT = os.path.normcase(os.path.normpath(
    os.environ.get("WC3_DIST_NDK_ROOT", r"E:\Android\Sdk\ndk\29.0.14206865")))
TMP_ROOT = os.environ.get("WC3_DIST_TMP", r"E:\wc3-dist\tmp")


def read_exact(stream, size):
    data = stream.read(size)
    if data is None or len(data) != size:
        raise EOFError("short read")
    return data


def inside_ndk(path):
    path = os.path.normcase(os.path.normpath(path))
    return os.path.commonpath([path, NDK_ROOT]) == NDK_ROOT


def run_compile(header, source):
    compiler = os.path.normpath(str(header.get("compiler", "")))
    if not inside_ndk(compiler):
        raise ValueError("compiler outside WC3_DIST_NDK_ROOT")
    if not os.path.isfile(compiler):
        raise ValueError("compiler not found: %s" % compiler)

    args = [str(a) for a in header.get("args", [])]
    if any(a in ("-o", "-MF", "-MT", "-MJ", "-M", "-MM") for a in args):
        raise ValueError("client sent output/dependency flags")
    lang = header.get("lang")
    if lang not in ("cpp-output", "c++-cpp-output"):
        raise ValueError("unsupported language")

    timeout = int(header.get("timeout", 900))
    timeout = min(max(timeout, 1), 3600)
    suffix = ".ii" if lang == "c++-cpp-output" else ".i"
    name = os.path.basename(str(header.get("source_name", "input" + suffix)))
    if not name.lower().endswith(suffix):
        name = "input" + suffix

    os.makedirs(TMP_ROOT, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="wc3dist-", dir=TMP_ROOT)
    try:
        in_path = os.path.join(tmpdir, name)
        out_path = os.path.join(tmpdir, "out.o")
        with open(in_path, "wb") as handle:
            handle.write(source)

        command = [compiler] + args + ["-o", out_path, in_path]
        started = time.monotonic()
        try:
            proc = subprocess.run(
                command,
                cwd=tmpdir,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            rc = proc.returncode
            stdout = proc.stdout
            stderr = proc.stderr
        except subprocess.TimeoutExpired as exc:
            rc = 124
            stdout = exc.stdout or b""
            stderr = (exc.stderr or b"") + b"\nremote compile timed out\n"
        elapsed = time.monotonic() - started

        obj = b""
        if rc == 0:
            with open(out_path, "rb") as handle:
                obj = zlib.compress(handle.read(), 3)

        return {
            "rc": rc,
            "stdout": stdout.decode("utf-8", "replace"),
            "stderr": stderr.decode("utf-8", "replace"),
            "elapsed": round(elapsed, 3),
            "object": obj,
        }
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def send(head, obj=b""):
    raw = json.dumps(head, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(struct.pack(">I", len(raw)) + raw + obj)
    sys.stdout.buffer.flush()


def main():
    try:
        header_len = struct.unpack(">I", read_exact(sys.stdin.buffer, 4))[0]
        if header_len <= 0 or header_len > MAX_HEADER:
            raise ValueError("bad header length")
        header = json.loads(read_exact(sys.stdin.buffer, header_len).decode("utf-8"))

        data_len = struct.unpack(">Q", read_exact(sys.stdin.buffer, 8))[0]
        if data_len <= 0 or data_len > MAX_SOURCE:
            raise ValueError("bad source length")
        source = zlib.decompress(read_exact(sys.stdin.buffer, data_len))

        result = run_compile(header, source)
        obj = result.pop("object")
        send(result, obj)
    except Exception as exc:  # protocol error, not a compiler failure
        send({
            "rc": 125,
            "stdout": "",
            "stderr": "wc3-dist helper: %s\n%s" % (exc, traceback.format_exc(limit=4)),
            "elapsed": 0,
        })


if __name__ == "__main__":
    main()
