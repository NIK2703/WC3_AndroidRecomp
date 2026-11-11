#!/usr/bin/env python3
"""PASS14 evidence: reproducible snapshot of steady-state load inputs.

Read-only.  Collects (a) tree/artifact identity, (b) ELF section and dynamic
tag profile of the currently present .so, (c) lexical source inventory of
runtime/platform/app code, (d) dynamics of the newest device logs, (e) a
small set of constants parsed back out of the sources.  No device command is
issued and no measurement of frame time, CPU, GPU, battery or RSS is claimed.

    python tools/pass14_steady_load.py --json reports/generated/pass14_steady_load.json
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import struct
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "reports/generated/pass14_steady_load.json"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def now_local() -> str:
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def file_meta(rel: str) -> dict:
    p = ROOT / rel
    if not p.is_file():
        return {"path": rel, "status": "absent"}
    st = p.stat()
    return {
        "path": rel,
        "status": "present",
        "bytes": st.st_size,
        "sha256": sha256(p),
        "mtime_local": dt.datetime.fromtimestamp(st.st_mtime)
        .astimezone()
        .isoformat(timespec="seconds"),
    }


def git_output(*args: str) -> str:
    try:
        return subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, timeout=20
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unavailable"


def elf_profile(path: Path) -> dict:
    """Minimal ELF64 LE reader: section table + .dynamic tags."""
    with path.open("rb") as f:
        hdr = f.read(64)
        if hdr[:4] != b"\x7fELF" or hdr[4] != 2:
            return {"error": "not an ELF64 file"}
        e_shoff = struct.unpack_from("<Q", hdr, 0x28)[0]
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", hdr, 0x3A)
        f.seek(e_shoff)
        shbuf = f.read(e_shentsize * e_shnum)
        shs = []
        for i in range(e_shnum):
            off = i * e_shentsize
            name, stype = struct.unpack_from("<II", shbuf, off)
            flags, addr, offset, size = struct.unpack_from("<QQQQ", shbuf, off + 8)
            shs.append(
                {
                    "name_off": name,
                    "type": stype,
                    "offset": offset,
                    "size": size,
                    "addr": addr,
                }
            )
        strtab = shs[e_shstrndx]
        f.seek(strtab["offset"])
        strdata = f.read(strtab["size"])
        for sh in shs:
            end = strdata.find(b"\0", sh["name_off"])
            sh["name"] = strdata[sh["name_off"] : end].decode("ascii", "replace")

        by_name = {sh["name"]: sh for sh in shs}
        sections = sorted(
            (
                {"name": sh["name"], "bytes": sh["size"]}
                for sh in shs
                if sh["size"] and sh["type"] != 8
            ),
            key=lambda s: -s["bytes"],
        )

        dynamic = {}
        if ".dynamic" in by_name:
            dyn = by_name[".dynamic"]
            f.seek(dyn["offset"])
            dynbuf = f.read(dyn["size"])
            dynstr = b""
            if ".dynstr" in by_name:
                ds = by_name[".dynstr"]
                f.seek(ds["offset"])
                dynstr = f.read(ds["size"])
            needed, counts = [], Counter()
            for off in range(0, len(dynbuf) - 15, 16):
                tag, val = struct.unpack_from("<QQ", dynbuf, off)
                if tag == 0:
                    break
                counts[tag] += 1
                if tag == 1 and val < len(dynstr):  # DT_NEEDED
                    end = dynstr.find(b"\0", val)
                    needed.append(dynstr[val:end].decode("ascii", "replace"))
                if tag == 0x6FFFFFF9:  # DT_RELACOUNT
                    dynamic["relacount"] = val
                if tag == 7:  # DT_RELA
                    dynamic["rela_addr"] = val
                if tag == 8:  # DT_RELASZ
                    dynamic["rela_bytes"] = val
                if tag == 30:  # DT_FLAGS
                    dynamic["dt_flags"] = val
            dynamic["needed"] = needed
            dynamic["dt_relr_present"] = 36 in counts or 0x6FFFE000 in counts
            dynamic["dt_android_rela_present"] = 0x60000011 in counts
    return {"sections": sections[:24], "dynamic": dynamic}


SUBSYSTEM_GLOBS = {
    "guest_cpu_boot": ["runtime/guest/**/*", "runtime/cpu/**/*", "runtime/boot/**/*"],
    "hle_abi_bridge": ["runtime/hle/abi/**/*", "runtime/hle/hle_bridge.*"],
    "hle_android_legacy": ["runtime/hle/android/**/*"],
    "hle_target_modules": ["runtime/hle/win32/**/*", "runtime/hle/msvcr80/**/*"],
    "platform_android": ["runtime/platform/android/**/*", "runtime/platform/platform.*"],
    "platform_headless": ["runtime/platform/headless/**/*"],
    "app_native": ["apps/android_gameactivity/src/main/cpp/**/*"],
    "app_java": ["apps/android_gameactivity/app/src/main/java/**/*"],
    "diag_ipc_gfx": ["runtime/diag/**/*", "runtime/ipc/**/*", "runtime/gfx/**/*", "runtime/compat/**/*"],
}

PATTERNS = {
    "pthread_create": r"\bpthread_create\s*\(",
    "mutex_lock": r"\bpthread_mutex_lock\s*\(",
    "cond_timedwait": r"\bpthread_cond_timedwait\s*\(",
    "cond_broadcast": r"\bpthread_cond_broadcast\s*\(",
    "sem_timedwait": r"\bsem_timedwait\s*\(",
    "sleep_usleep": r"\b(?:usleep|nanosleep|sleep)\s*\(",
    "sched_yield": r"\bsched_yield\s*\(",
    "clock_gettime": r"\bclock_gettime\s*\(",
    "getenv": r"\bgetenv\s*\(",
    "log_print": r"\b(?:fprintf|LOGI|LOGE|__android_log_print|MSS_LOG)\s*\(",
    "heap_alloc": r"\b(?:malloc|calloc|realloc|free)\s*\(",
    "snprintf": r"\bsnprintf\s*\(",
    "atomic": r"(?:\b_Atomic\b|\batomic_\w+\s*\(|__atomic_\w+\s*\()",
}

SOURCE_EXTS = {".c", ".h", ".cpp", ".java", ".kt"}


def source_inventory() -> dict:
    result = {}
    for subsystem, globs in SUBSYSTEM_GLOBS.items():
        files = sorted(
            {
                p
                for pattern in globs
                for p in ROOT.glob(pattern)
                if p.is_file() and p.suffix in SOURCE_EXTS
            }
        )
        texts = [p.read_text(encoding="utf-8", errors="replace") for p in files]
        joined = "\n".join(texts)
        counts = {k: len(re.findall(v, joined)) for k, v in PATTERNS.items()}
        result[subsystem] = {
            "files": len(files),
            "lines": sum(t.count("\n") + 1 for t in texts),
            "patterns": counts,
        }
    return result


LOG_PATTERNS = {
    "presenter_blocks": r"Presenter: Actual swap chain properties:",
    "dxvk_fifo": r"VK_PRESENT_MODE_FIFO_KHR",
    "dxvk_mailbox": r"VK_PRESENT_MODE_MAILBOX_KHR",
    "swapchain_recreate_failed": r"Failed to recreate swap chain",
    "guest_thread_create": r"\[hle\] (?:CreateThread|_beginthreadex)",
    "peek_message": r"PeekMessage call",
    "get_message": r"\bGetMessage\b",
    "wait_for_multiple": r"WaitForMultipleObjects",
    "wait_for_single": r"WaitForSingleObject",
    "file_open": r"\[file\] open ",
    "file_read": r"\[file\] read ",
    "err_lines": r"(?m)^\s*err:",
    "warn_lines": r"(?m)^\s*warn:",
}

LOG_FILES = [
    "build/run_latest_utf8.txt",
    "build/run_new.txt",
    "build/debug_run_current.txt",
    "build/debug_run_latest.txt",
    "build/verify_run.txt",
]


def decode_log(path: Path) -> str:
    raw = path.read_bytes()
    if b"\x00" in raw[:512]:
        return raw.decode("utf-16", errors="replace").lstrip("\ufeff")
    return raw.decode("utf-8-sig", errors="replace")


def log_inventory() -> dict:
    out = {}
    for rel in LOG_FILES:
        p = ROOT / rel
        if not p.is_file():
            out[rel] = {"status": "absent"}
            continue
        text = decode_log(p)
        lines = text.count("\n") + (0 if text.endswith("\n") else 1)
        counts = {k: len(re.findall(v, text)) for k, v in LOG_PATTERNS.items()}
        tags = Counter(re.findall(r"\[([a-z0-9_]+)\]", text))
        sizes = Counter(re.findall(r"Buffer size:\s+(\d+)x(\d+)", text))
        images = Counter(re.findall(r"Image count:\s+(\d+)", text))
        timeouts = [int(x) for x in re.findall(r"timed out after (\d+) ms", text)]
        blocks = counts["presenter_blocks"]
        out[rel] = {
            **file_meta(rel),
            "lines": lines,
            "counts": counts,
            "presenter_block_lines_estimate": blocks * 6,
            "presenter_block_line_share": round(blocks * 6 / lines, 4) if lines else None,
            "top_tags": dict(tags.most_common(8)),
            "swapchain_sizes": {f"{w}x{h}": n for (w, h), n in sizes.most_common(4)},
            "swapchain_image_counts": dict(images.most_common(4)),
            "wait_timeout_ms": {
                "count": len(timeouts),
                "sum": sum(timeouts),
                "max": max(timeouts) if timeouts else None,
            },
            "has_artifact_sha256": "artifact_sha256" in text.lower(),
        }
    return out


CONST_SPECS = [
    ("runtime/guest/guest_layout.h", r"#define\s+GUEST_VA_SIZE\s+(\S+)"),
    ("runtime/guest/guest_layout.h", r"#define\s+GUEST_HLE_MAX\s+(\S+)"),
    ("runtime/guest/guest_memory.c", r"#define\s+GUEST_PAGE_COUNT\s+(\S+)"),
    ("runtime/guest/guest_memory.c", r"#define\s+GUEST_RANGE_MAX\s+(\S+)"),
    ("runtime/hle/abi/hle_abi.h", r"#define\s+HLE_MAX\s+(\S+)"),
    ("runtime/hle/abi/hle_abi.c", r"#define\s+HLE_FAST_MAX\s+(\S+)"),
    ("runtime/hle/win32/dsound/dsound_hle.c", r"#define\s+DS_METHODS\s+(\S+)"),
    ("runtime/hle/win32/dsound/dsound_hle.c", r"#define\s+DS_OBJ_MAX\s+(\S+)"),
    ("runtime/hle/win32/winmm/mss_timer_hle.c", r"#define\s+MSS_TIMER_MAX\s+(\S+)"),
    ("runtime/platform/audio_mixer.h", r"#define\s+WC3_MIXER_VOICES\s+(\S+)"),
    ("runtime/platform/audio_mixer.h", r"#define\s+WC3_VOICE_SLOT_BYTES\s+(\S+)"),
    ("runtime/platform/audio_mixer.h", r"#define\s+WC3_MIX_CHUNK\s+(\S+)"),
    ("apps/android_gameactivity/src/main/cpp/host_shell.c", r"#define\s+OVERLAY_SNAPSHOTS\s+(\S+)"),
    ("runtime/platform/android/controls/controls.c", r"g_timer_interval_ms\s*=\s*(\d+)"),
    ("runtime/hle/android/hle_android.c", r"#define\s+WAIT_CAP_MS\s+(\S+)"),
    ("cmake/Wc3ReliftedCorpus.cmake", r"splitcorpus[^\n]*"),
]


def constants() -> dict:
    out = {}
    for rel, pattern in CONST_SPECS:
        p = ROOT / rel
        if not p.is_file():
            out[f"{rel}|{pattern}"] = {"status": "absent"}
            continue
        m = re.search(pattern, p.read_text(encoding="utf-8", errors="replace"))
        out[f"{rel}|{pattern}"] = {
            "status": "found" if m else "not_found",
            "value": m.group(1) if m and m.groups() else (m.group(0) if m else None),
        }
    hash_hdr = ROOT / "build/android_app/debug/cmake/legacy_boot_generated/boot_dispatch_hash.h"
    if hash_hdr.is_file():
        text = hash_hdr.read_text(encoding="utf-8", errors="replace")
        bits = re.search(r"#define\s+LIFT_HASH_BITS\s+(\d+)", text)
        rows = len(re.findall(r"(?m)^\s*\[0x[0-9A-Fa-f]+\]\s*=\s*\{", text))
        out["dispatch_hash"] = {
            "header": str(hash_hdr.relative_to(ROOT)).replace("\\", "/"),
            "hash_bits": int(bits.group(1)) if bits else None,
            "slots": (1 << int(bits.group(1))) if bits else None,
            "nonempty_entries": rows,
            "load_factor": round(rows / (1 << int(bits.group(1))), 4) if bits else None,
            "sha256": sha256(hash_hdr),
        }
    return out


def build() -> dict:
    head = git_output("rev-parse", "--short", "HEAD")
    status = git_output("status", "--porcelain")
    artifacts = {
        k: file_meta(v)
        for k, v in {
            "debug_so": "build/android_app/debug/cmake/libwc3gameactivity.so",
            "release_so": "build/android_app/release/cmake/libwc3gameactivity.so",
            "debug_apk": "build/android_app/wc3-debug.apk",
            "release_apk": "build/android_app/wc3-release.apk",
            "corpus_manifest": "build/corpus-full/corpus_manifest.json",
        }.items()
    }
    # A parallel build may remove the primary artifact between two reads.
    # Pin whichever .so is newest at snapshot time and label its role.
    candidates = sorted(
        (
            p
            for p in (ROOT / "build").glob("**/libwc3gameactivity.so")
            if p.is_file()
        ),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    fallback = candidates[0] if candidates else None
    artifacts["newest_so_anywhere"] = (
        file_meta(str(fallback.relative_to(ROOT)).replace("\\", "/"))
        if fallback
        else {"status": "absent"}
    )
    elf = {}
    so = ROOT / "build/android_app/debug/cmake/libwc3gameactivity.so"
    if not so.is_file():
        so = fallback
    if so and so.is_file():
        elf["profiled_so"] = file_meta(str(so.relative_to(ROOT)).replace("\\", "/"))
        elf["profile"] = elf_profile(so)
    cache = ROOT / "build/android_app/debug/cmake/CMakeCache.txt"
    options = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("WC3_") and ":" in line:
                key, value = line.split(":", 1)
                options[key] = value.split("=", 1)[-1]
    return {
        "head": head,
        "dirty_paths": len(status.splitlines()) if status != "unavailable" else None,
        "artifacts": artifacts,
        "elf": elf,
        "debug_cmake_options": options,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=str(DEFAULT_OUT))
    args = ap.parse_args()
    logs = log_inventory()
    report = {
        "schema": "wc3.pass14_steady_load.v1",
        "status": "report-only",
        "captured_at_local": now_local(),
        "method": "static ELF/source/log parsing; no device, no WC3_PERF, no frame-time sampling",
        "tree_and_artifacts": build(),
        "source_inventory": source_inventory(),
        "device_logs": logs,
        "constants": constants(),
        "derived": {
            "overlay_idle_wakes_per_s": 10.0,
            "input_tick_hz_from_16ms": 62.5,
            "audio_supervisor_hz_from_100ms": 10.0,
            "known_periodic_wakes_per_s_excluding_audio_and_guest": 82.5,
            "swapchain_rgba8_bytes_2400x1080_x5": 2400 * 1080 * 4 * 5,
        },
        "limits": [
            "No device is attached in this environment; T2/PM-12/PM-13 = unavailable.",
            "Device logs have no artifact_sha256, no calibration (PM-11) and no timestamps; rates cannot be derived.",
            "Debug build has WC3_DEBUG=ON and all debug tags enabled; it is not a proxy for release steady state.",
            "Lexical pattern counts are static sites, not dynamic frequencies (PM-17).",
            "Release .so/APK may be absent while the tree is being rebuilt; hashes pin whatever was present.",
            "Generated corpus C is lifter output and was not edited or counted as hand-maintained code (O-104).",
        ],
    }
    out = Path(args.json)
    if not out.is_absolute():
        out = ROOT / out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    print(f"sha256={sha256(out)}")
    for rel, info in logs.items():
        if info.get("status") == "present":
            print(f"  {rel}: lines={info['lines']} presenter_blocks={info['counts']['presenter_blocks']} share={info['presenter_block_line_share']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
