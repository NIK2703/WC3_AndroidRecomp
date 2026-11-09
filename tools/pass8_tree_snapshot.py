#!/usr/bin/env python3
"""Capture the PASS8/O-84 tree and artifact snapshot.

The snapshot is intentionally report-only.  It records which source paths are
dirty, the current build artifacts, and whether the stripped `.so` packaged in
the release APK still has the same loaded sections as the canonical unstripped
release `.so`.  It does not build, install, or execute anything.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from datetime import datetime
from pathlib import Path

from icf_audit import read_sections

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "reports" / "generated" / "pass8_tree_snapshot.json"
ARTIFACTS = [
    "build/android_app/release/cmake/libwc3gameactivity.so",
    "build/android_app/debug/cmake/libwc3gameactivity.so",
    "build/android_app/jniLibs/arm64-v8a/libwc3gameactivity.so",
    "build/android_app/wc3-release.apk",
    "build/android_app/wc3-debug.apk",
    "build/android_app/debug/cmake/libwc3_corpus_relifted.a",
]
EVIDENCE = [
    "reports/generated/pass8_static_release.json",
    "reports/generated/pass8_static_debug.json",
    "reports/generated/pass8_static_compare.json",
    "reports/generated/pass8_icf_release.json",
    "reports/generated/pass8_icf_debug.json",
    "reports/generated/pass8_plt_release.json",
    "reports/generated/pass8_plt_debug.json",
    "reports/generated/pass8_t2_device.json",
    "reports/generated/pass8_t2_menu.png",
    "reports/generated/pass8_t2_cold.png",
    "reports/generated/pass8_t2_logcat_displayed.txt",
    "reports/generated/pass7_static_measures.json",
]
CODE_SECTIONS = (".text", ".rodata", ".data.rel.ro", ".data")
RELOC_SECTIONS = (".relr.dyn", ".rela.dyn")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _file_row(path: Path) -> dict[str, object]:
    row: dict[str, object] = {
        "path": path.as_posix(),
        "bytes": path.stat().st_size,
        "mtime": datetime.fromtimestamp(path.stat().st_mtime).isoformat(
            timespec="seconds"),
        "sha256": _sha256(path.read_bytes()),
    }
    return row


def _git_changes() -> list[dict[str, object]]:
    process = subprocess.run(
        ["git", "status", "--porcelain=v1", "-z", "-uall"],
        cwd=ROOT, check=True, capture_output=True)
    names = process.stdout.decode("utf-8", errors="replace").split("\0")
    rows: list[dict[str, object]] = []
    index = 0
    while index < len(names):
        entry = names[index]
        index += 1
        if not entry:
            continue
        status = entry[:2]
        path = entry[3:]
        if status[0] in "RC" and index < len(names):
            index += 1  # the rename source is not the file to hash
        full = ROOT / path
        if not full.exists():
            rows.append({"path": path, "status": status.strip(), "present": False})
            continue
        if full.is_dir():
            rows.append({"path": path, "status": status.strip(),
                         "present": True, "kind": "directory"})
            continue
        row = _file_row(full)
        row["status"] = status.strip()
        rows.append(row)
    rows.sort(key=lambda row: str(row["path"]))
    return rows


def _compare_sections(packaged: bytes,
                      candidate: bytes) -> dict[str, object]:
    psections = {section.name: section for section in read_sections(packaged)}
    csections = {section.name: section for section in read_sections(candidate)}
    sections: dict[str, object] = {}
    same_code = True
    for name in CODE_SECTIONS:
        packaged_section = psections.get(name)
        candidate_section = csections.get(name)
        if packaged_section is None or candidate_section is None:
            sections[name] = {"same": False, "reason": "section missing"}
            same_code = False
            continue
        packaged_bytes = packaged[
            packaged_section.offset:packaged_section.offset + packaged_section.size]
        candidate_bytes = candidate[
            candidate_section.offset:candidate_section.offset + candidate_section.size]
        same = packaged_bytes == candidate_bytes
        sections[name] = {
            "same": same,
            "bytes": packaged_section.size,
            "sha256": _sha256(packaged_bytes),
        }
        same_code = same_code and same
    relocations: dict[str, object] = {}
    same_relocations = True
    for name in RELOC_SECTIONS:
        packaged_section = psections.get(name)
        candidate_section = csections.get(name)
        if packaged_section is None and candidate_section is None:
            continue
        if packaged_section is None or candidate_section is None:
            relocations[name] = {
                "same": False,
                "reason": "section present in only one artifact",
                "packaged_present": packaged_section is not None,
                "candidate_present": candidate_section is not None,
            }
            same_relocations = False
            continue
        packaged_bytes = packaged[
            packaged_section.offset:packaged_section.offset + packaged_section.size]
        candidate_bytes = candidate[
            candidate_section.offset:candidate_section.offset + candidate_section.size]
        same = packaged_bytes == candidate_bytes
        relocations[name] = {
            "same": same,
            "bytes": packaged_section.size,
            "sha256": _sha256(packaged_bytes),
        }
        same_relocations = same_relocations and same
    return {
        "same_loaded_sections": same_code,
        "same_code_sections": same_code,
        "same_relocation_sections": same_relocations,
        "sections": sections,
        "relocation_sections": relocations,
    }


def _packaged_sections() -> dict[str, object]:
    apk = ROOT / "build/android_app/wc3-release.apk"
    so = ROOT / "build/android_app/release/cmake/libwc3gameactivity.so"
    jnilibs = ROOT / "build/android_app/jniLibs/arm64-v8a/libwc3gameactivity.so"
    entry = "lib/arm64-v8a/libwc3gameactivity.so"
    result: dict[str, object] = {"available": False}
    if not apk.exists():
        result["reason"] = "release APK is missing"
        return result
    with zipfile.ZipFile(apk) as archive:
        try:
            packaged = archive.read(entry)
        except KeyError:
            result["reason"] = "release APK has no arm64-v8a libwc3gameactivity.so"
            return result
    result = {
        "available": True,
        "apk_entry": entry,
        "packaged_bytes": len(packaged),
        "packaged_sha256": _sha256(packaged),
    }
    if jnilibs.exists():
        comparison = _compare_sections(packaged, jnilibs.read_bytes())
        result["jnilibs"] = {
            "path": jnilibs.as_posix(),
            "sha256": _sha256(jnilibs.read_bytes()),
            **comparison,
        }
    if so.exists():
        comparison = _compare_sections(packaged, so.read_bytes())
        result["unstripped_release"] = {
            "path": so.as_posix(),
            "sha256": _sha256(so.read_bytes()),
            "mtime": datetime.fromtimestamp(so.stat().st_mtime).isoformat(
                timespec="seconds"),
            **comparison,
        }
    return result


def _frozen_pass8() -> dict[str, object]:
    def read(name: str) -> dict[str, object] | None:
        path = ROOT / "reports" / "generated" / name
        if not path.exists():
            return None
        return json.loads(path.read_text(encoding="utf-8"))

    static_release = read("pass8_static_release.json")
    static_debug = read("pass8_static_debug.json")
    t2 = read("pass8_t2_device.json")
    frozen: dict[str, object] = {
        "available": bool(static_release and static_debug and t2),
        "source": "reports/generated/pass8_static_*.json + pass8_t2_device.json",
    }
    if static_release:
        frozen["release_so_sha256"] = static_release.get("artifact_sha256")
        frozen["release_so_bytes"] = static_release.get("artifact_bytes")
        frozen["release_so_path_at_measurement"] = static_release.get("artifact")
    if static_debug:
        frozen["debug_so_sha256"] = static_debug.get("artifact_sha256")
        frozen["debug_so_bytes"] = static_debug.get("artifact_bytes")
        frozen["debug_so_path_at_measurement"] = static_debug.get("artifact")
    if t2:
        apk = t2.get("artifacts", {}).get("local_release_apk", {})
        frozen["release_apk_sha256"] = apk.get("sha256")
        frozen["release_apk_bytes"] = apk.get("bytes")
        frozen["installed_apk_sha256"] = t2.get("identity", {}).get(
            "installed_apk_sha256")
        frozen["captured_at_local"] = t2.get("captured_at_local")
    current_apk = ROOT / "build/android_app/wc3-release.apk"
    current_so = ROOT / "build/android_app/release/cmake/libwc3gameactivity.so"
    if current_apk.exists():
        frozen["current_release_apk_sha256"] = _sha256(
            current_apk.read_bytes())
        frozen["current_release_apk_bytes"] = current_apk.stat().st_size
        frozen["current_release_apk_matches_frozen"] = (
            frozen.get("release_apk_sha256") ==
            frozen["current_release_apk_sha256"])
    if current_so.exists():
        frozen["current_release_so_sha256"] = _sha256(
            current_so.read_bytes())
        frozen["current_release_so_bytes"] = current_so.stat().st_size
        frozen["current_release_so_matches_frozen"] = (
            frozen.get("release_so_sha256") ==
            frozen["current_release_so_sha256"])
    return frozen


def _t2_summary() -> dict[str, object]:
    path = ROOT / "reports" / "generated" / "pass8_t2_device.json"
    if not path.exists():
        return {"available": False, "reason": "pass8_t2_device.json is missing"}
    report = json.loads(path.read_text(encoding="utf-8"))
    measurements = report.get("measurements", {})
    return {
        "available": True,
        "status": report.get("status"),
        "cold_activity_total_p50_ms":
            measurements.get("cold_activity_total_p50_ms"),
        "cold_activity_total_p95_ms":
            measurements.get("cold_activity_total_p95_ms"),
        "cold_first_frame_ms":
            report.get("unavailable", {}).get("cold_first_frame_ms",
                                              "unavailable"),
        "menu_screenshot": measurements.get("screenshots", {}).get("menu"),
    }


def capture() -> dict[str, object]:
    artifacts = []
    for name in ARTIFACTS:
        path = ROOT / name
        if path.exists():
            artifacts.append(_file_row(path))
    evidence = []
    for name in EVIDENCE:
        path = ROOT / name
        if path.exists():
            evidence.append(_file_row(path))
    changes = _git_changes()
    newest = max(
        (str(row["mtime"]) for row in changes if "mtime" in row),
        default=None)
    release = next(
        (row for row in artifacts
         if row["path"].endswith("release/cmake/libwc3gameactivity.so")), None)
    debug = next(
        (row for row in artifacts
         if row["path"].endswith("debug/cmake/libwc3gameactivity.so")), None)
    packaged = _packaged_sections()
    t2 = _t2_summary()
    return {
        "schema": "wc3.pass8_tree_snapshot.v2",
        "kind": "pass8_tree_snapshot",
        "captured_at_local": datetime.now().isoformat(timespec="seconds"),
        "purpose": (
            "PASS8/O-84: dirty sources, current release/debug artifacts, "
            "packaged stripped .so identity, and available T2 metrics. "
            "Report-only; no build or device action is performed by this tool."
        ),
        "changed_sources": changes,
        "artifacts": artifacts,
        "evidence_reports": evidence,
        "verdicts": {
            "release_so_sha256": release["sha256"] if release else None,
            "debug_so_sha256": debug["sha256"] if debug else None,
            "newest_source_mtime": newest,
            "pass8_frozen": _frozen_pass8(),
            "packaged_stripped_so": packaged,
            "t2": t2,
            "frame_time_measurement": (
                "unavailable (V1.2); G-10 declared, PM-13 missing"
            ),
            "gb5_matrix": (
                "unavailable; ADR-004/007 provisional, no A/B/C/D or "
                "cold/warm page-cache run"
            ),
            "dual_dxvk_status": (
                "ADR-004/007 provisional; GB-5 measurement required, "
                "backend removal forbidden"
            ),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Capture the PASS8/O-84 tree snapshot without building.")
    parser.add_argument("--json", default=str(DEFAULT_OUT))
    args = parser.parse_args(argv)
    report = capture()
    output = Path(args.json)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8")
    print(f"tree snapshot: {output}")
    print(f"  changed sources: {len(report['changed_sources'])}")
    print(f"  artifacts: {len(report['artifacts'])}")
    print(f"  newest source: {report['verdicts']['newest_source_mtime']}")
    frozen = report["verdicts"]["pass8_frozen"]
    print(f"  frozen release APK: {frozen.get('release_apk_sha256')}")
    print(f"  frozen release .so: {frozen.get('release_so_sha256')}")
    print(f"  current APK matches frozen: "
          f"{frozen.get('current_release_apk_matches_frozen')}")
    print(f"  current release .so matches frozen: "
          f"{frozen.get('current_release_so_matches_frozen')}")
    packaged = report["verdicts"]["packaged_stripped_so"]
    jnilibs = packaged.get("jnilibs", {})
    release = packaged.get("unstripped_release", {})
    print(f"  packaged == jniLibs code sections: "
          f"{jnilibs.get('same_code_sections')}")
    print(f"  packaged == jniLibs relocation sections: "
          f"{jnilibs.get('same_relocation_sections')}")
    print(f"  packaged == unstripped release code sections: "
          f"{release.get('same_code_sections')}")
    print(f"  packaged == unstripped release relocation sections: "
          f"{release.get('same_relocation_sections')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
