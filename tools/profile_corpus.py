#!/usr/bin/env python3
"""Static profile for the generated lift corpus (O-19, O-30).

The profile counts source-level constructs in the generated C.  It is not a
timing measurement: a site count says nothing about dynamic frequency or cost
(PM-17).  Every result is therefore report-only; the tool never prints a
speedup.

O-30 requires a published artifact identity and a cross-check against a known
reference before a count is treated as canonical.  This tool therefore:
  * hashes every input file and the concatenated profile;
  * computes a stable JSON document;
  * when --expect-full is used, compares selected counters with the recorded
    full-corpus reference and returns nonzero on a mismatch instead of silently
    deleting the field.

Usage:
  python tools/profile_corpus.py --corpus build/corpus-full \
      --out reports/generated/corpus_static_profile.json --expect-full
  python tools/profile_corpus.py --self-test
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The historical full-corpus reference from PERFORMANCE_AUDIT_PASS2.md §2.
# It describes the pre-rev2 generated corpus.  --expect-full compares against
# it only to show whether a new slice is comparable; a mismatch is expected
# when the generated corpus changed and is never reported as a pass.
FULL_REFERENCE = {
    "functions": 181_194,
    "dispatch_constant": 256_774,
    "dispatch_jmp_constant": 230_207,
    "constant_sites": 486_981,
    "direct_L_calls": 0,
    "push_then_dispatch": 256_774,
    "hle_thunk_literals": 0,
    "gva": 174_711,
    "push32": 852_684,
    "pop32": 262_190,
    "x87": 27_830,
    "sse": 21_735,
    "mm": 17_785,
    "fpu_cw": 72,
    "fpu_sw": 203,
    "xmm": 103_229,
    "atomic": 174,
}


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _iter_sources(corpus: Path) -> list[Path]:
    if corpus.is_file():
        return [corpus]
    if not corpus.is_dir():
        raise SystemExit(f"profile_corpus: not a file or directory: {corpus}")
    files = sorted(
        p for p in corpus.rglob("*.c")
        if p.is_file() and p.name in {"seed.c", "closed.c"}
    )
    if not files:
        raise SystemExit(f"profile_corpus: no seed.c/closed.c under {corpus}")
    return files


def _read_reference_list(corpus: Path) -> dict[str, object] | None:
    """Read the independent function list shipped next to the generated C."""
    if not corpus.is_dir():
        return None
    path = corpus / "wc3_all_list.h"
    if not path.is_file():
        return None
    data = path.read_bytes()
    entries = len(re.findall(rb"(?m)^\s*X\([0-9A-Fa-f]+\)", data))
    return {
        "path": "wc3_all_list.h",
        "sha256": _sha256_bytes(data),
        "entries": entries,
    }


def _count_metrics(data: bytes) -> dict[str, object]:
    dispatch_constant = data.count(b"dispatch(c, 0x")
    dispatch_jmp_constant = data.count(b"dispatch_jmp(c, 0x")
    dispatch_any = data.count(b"dispatch(c, ")
    dispatch_jmp_any = data.count(b"dispatch_jmp(c, ")

    reads = sum(data.count(name) for name in (
        b"rd8(", b"rd16(", b"rd32(", b"rd64(",
        b"rdss(", b"rdsd(", b"rdxm(", b"rd32g(",
    ))
    writes = sum(data.count(name) for name in (
        b"wr8(", b"wr16(", b"wr32(", b"wr64(",
        b"wrss(", b"wrsd(", b"wrxm(", b"wr32g(",
    ))
    flag_reads = len(re.findall(rb"c->(?:cf|zf|sf|of|pf|af)\b", data))
    direct_calls = len(re.findall(
        rb"(?<![A-Za-z0-9_])L_[0-9A-Fa-f]+\(c\)", data))
    push_then_dispatch = len(re.findall(
        rb"push32\(c, [^\n;]+\);\s*dispatch\(c, 0x", data))
    # The HLE thunk window is one exact 32-bit value.  Counting a shorter
    # 0x7f00000 prefix would double-count every 0x7f000000 literal.
    hle_literals = len(re.findall(
        rb"(?i)0x0*7f000000(?![0-9a-f])", data))
    # The audit's x87 metric is the whole x87 operation surface, not only the
    # helpers whose name starts with x87_.  This is the definition that
    # reproduces the documented 27,830 sites on the historical corpus.
    x87 = sum(data.count(name) for name in
              (b"x87_", b"fpush(", b"fpop(", b"fst("))

    return {
        "functions": len(re.findall(rb"(?m)^void L_[0-9A-Fa-f]+\(CPU \*c\)",
                                   data)),
        "dispatch_constant": dispatch_constant,
        "dispatch_jmp_constant": dispatch_jmp_constant,
        "constant_sites": dispatch_constant + dispatch_jmp_constant,
        "indirect_sites": (dispatch_any - dispatch_constant +
                           dispatch_jmp_any - dispatch_jmp_constant),
        "direct_L_calls": direct_calls,
        "push_then_dispatch": push_then_dispatch,
        "hle_thunk_literals": hle_literals,
        "flags": data.count(b"flags_"),
        "flag_reads": flag_reads,
        "gva": data.count(b"GVA("),
        "guest_reads": reads,
        "guest_writes": writes,
        "push32": data.count(b"push32("),
        "pop32": data.count(b"pop32("),
        "x87": x87,
        "sse": data.count(b"sse_"),
        "mm": data.count(b"mm_"),
        "fpu_cw": data.count(b"c->fpu_cw"),
        "fpu_sw": data.count(b"c->fpu_sw"),
        "xmm": data.count(b"c->xmm["),
        "atomic": data.count(b"atomic_"),
    }


def profile(corpus: Path) -> dict[str, object]:
    files = _iter_sources(corpus)
    file_records = []
    totals: dict[str, object] = {}
    profile_hash = hashlib.sha256()
    total_bytes = 0

    numeric_keys = None
    for path in files:
        data = path.read_bytes()
        rel = path.relative_to(corpus) if corpus.is_dir() else path.name
        digest = _sha256_bytes(data)
        total_bytes += len(data)
        metrics = _count_metrics(data)
        file_records.append({
            "path": str(rel).replace("\\", "/"),
            "bytes": len(data),
            "sha256": digest,
            "metrics": metrics,
        })
        profile_hash.update(str(rel).replace("\\", "/").encode())
        profile_hash.update(b"\0")
        profile_hash.update(digest.encode())
        profile_hash.update(b"\0")
        if numeric_keys is None:
            numeric_keys = [k for k, v in metrics.items() if isinstance(v, int)]
        for key in numeric_keys or ():
            totals[key] = int(totals.get(key, 0)) + int(metrics[key])

    manifest_sha = None
    if corpus.is_dir():
        manifest = corpus / "corpus_manifest.json"
        if manifest.is_file():
            try:
                record = json.loads(manifest.read_text(encoding="utf-8"))
                manifest_sha = (record.get("corpus_sha256") or
                                record.get("sha256"))
            except (OSError, ValueError):
                manifest_sha = None

    return {
        "schema": "wc3.corpus_static_profile.v1",
        "status": "report-only",
        "artifact_sha256": profile_hash.hexdigest(),
        "corpus_sha256": manifest_sha,
        "source_root": str(corpus).replace("\\", "/"),
        "file_count": len(files),
        "source_bytes": total_bytes,
        "reference": _read_reference_list(corpus),
        "files": file_records,
        "totals": totals,
    }


def _validate_against_full(totals: dict[str, object]) -> dict[str, object]:
    checks = {}
    ok = True
    for key, expected in FULL_REFERENCE.items():
        actual = totals.get(key)
        passed = actual == expected
        ok = ok and passed
        checks[key] = {
            "expected": expected,
            "actual": actual,
            "status": "pass" if passed else "mismatch",
        }
    return {"status": "pass" if ok else "mismatch", "checks": checks}


def _validate_identity(totals: dict[str, object],
                       reference: dict[str, object] | None) -> dict[str, object]:
    """Cross-check fields that have an independent identity source.

    The function count is compared with wc3_all_list.h, which is produced by
    the lift pipeline rather than by this scanner.  Structural relations catch
    a scanner that silently drops one dispatch form.  Missing reference data is
    reported as unavailable, never as a pass (O-30/V1.2).
    """
    checks: dict[str, dict[str, object]] = {}
    if reference is not None:
        checks["functions"] = {
            "expected": reference["entries"],
            "actual": totals.get("functions"),
            "status": ("pass" if totals.get("functions") ==
                       reference["entries"] else "mismatch"),
        }
    else:
        checks["functions"] = {
            "expected": None,
            "actual": totals.get("functions"),
            "status": "unavailable",
        }

    dispatch = totals.get("dispatch_constant")
    dispatch_jmp = totals.get("dispatch_jmp_constant")
    push_dispatch = totals.get("push_then_dispatch")
    checks["push_then_dispatch"] = {
        "expected": dispatch,
        "actual": push_dispatch,
        "status": "pass" if push_dispatch == dispatch else "mismatch",
    }
    checks["constant_sites"] = {
        "expected": (dispatch + dispatch_jmp
                     if isinstance(dispatch, int) and isinstance(dispatch_jmp, int)
                     else None),
        "actual": totals.get("constant_sites"),
        "status": ("pass" if isinstance(dispatch, int) and
                   isinstance(dispatch_jmp, int) and
                   totals.get("constant_sites") == dispatch + dispatch_jmp
                   else "mismatch"),
    }
    checks["hle_thunk_literals"] = {
        "expected": 0,
        "actual": totals.get("hle_thunk_literals"),
        "status": ("pass" if totals.get("hle_thunk_literals") == 0
                   else "mismatch"),
    }

    statuses = {check["status"] for check in checks.values()}
    if "mismatch" in statuses:
        status = "mismatch"
    elif "unavailable" in statuses:
        status = "unavailable"
    else:
        status = "pass"
    return {"status": status, "checks": checks}


def _self_test() -> int:
    fixture = b"""/* generated fixture */
void L_00401000(CPU *c)
{
    push32(c, 0x00401005u); dispatch(c, 0x00402000u);
    c->eax = (uint32_t)(GVA(0x00403000));
    c->xmm[0].f32[0] = rdss((GVA(0x00403004)));
    wrss((GVA(0x00403008)), c->xmm[0].f32[0]);
}

void L_00401010(CPU *c)
{
    { uint32_t _ct = rd32((GVA(0x00404000))); dispatch(c, _ct); }
    dispatch_jmp(c, 0x00401000u);
    c->ecx = (uint32_t)(pop32(c));
}
"""
    with tempfile.TemporaryDirectory(prefix="wc3-profile-") as tmp:
        src = Path(tmp) / "game"
        src.mkdir()
        (src / "seed.c").write_bytes(fixture)
        (src / "closed.c").write_bytes(b"")
        (Path(tmp) / "wc3_all_list.h").write_bytes(
            b"#define LIFTED_FUNCS(X) \\\n"
            b"    X(00401000) \\\n"
            b"    X(00401010)\n")
        result = profile(Path(tmp))
    totals = result["totals"]
    checks = {
        "functions": 2,
        "dispatch_constant": 1,
        "dispatch_jmp_constant": 1,
        "constant_sites": 2,
        "indirect_sites": 1,
        "direct_L_calls": 0,
        "push_then_dispatch": 1,
        "hle_thunk_literals": 0,
        "gva": 4,
        "guest_reads": 2,
        "guest_writes": 1,
        "push32": 1,
        "pop32": 1,
        "xmm": 2,
        "sse": 0,
        "x87": 0,
    }
    failures = []
    for key, expected in checks.items():
        actual = totals.get(key)
        if actual != expected:
            failures.append(f"{key}: expected {expected}, got {actual}")
    if result["file_count"] != 2 or result["source_bytes"] != len(fixture):
        failures.append("file_count/source_bytes mismatch")
    if (result.get("reference") or {}).get("entries") != 2:
        failures.append("wc3_all_list.h entry count mismatch")
    identity = _validate_identity(totals, result.get("reference"))
    if identity["status"] != "pass":
        failures.append(f"identity validation: {identity['status']}")
    # O-30: a mismatch must stay visible.  It must not disappear from the
    # document just because the scanner produced a different value.
    bad_totals = dict(totals)
    bad_totals["functions"] = int(bad_totals["functions"]) + 1
    bad_identity = _validate_identity(bad_totals, result.get("reference"))
    if bad_identity["status"] != "mismatch":
        failures.append("identity mismatch was not reported")
    if bad_identity["checks"]["functions"]["status"] != "mismatch":
        failures.append("identity mismatch field was deleted")
    bad_historical = _validate_against_full({})
    if bad_historical["status"] != "mismatch":
        failures.append("historical mismatch was not reported")
    if "functions" not in bad_historical["checks"]:
        failures.append("historical mismatch field was deleted")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    print(f"profile_corpus self-test: OK ({len(checks)} counters, "
          f"{result['file_count']} files, {result['source_bytes']} bytes)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path,
                        help="generated corpus file or directory")
    parser.add_argument("--out", type=Path,
                        help="write the JSON profile to this path")
    parser.add_argument("--expect-full", action="store_true",
                        help="compare totals with the recorded full-corpus "
                             "reference and fail on mismatch")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in counter test and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()
    if not args.corpus:
        parser.error("--corpus is required unless --self-test is used")

    result = profile(args.corpus)
    identity = _validate_identity(result["totals"], result.get("reference"))
    result["validation"] = {"identity": identity}
    if args.expect_full:
        result["validation"]["historical"] = _validate_against_full(
            result["totals"])
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8", newline="\n")
        print(f"profile_corpus: {result['file_count']} files, "
              f"{result['source_bytes']} bytes -> {args.out}")
    else:
        print(text, end="")
    if identity["status"] == "mismatch":
        print("profile_corpus: independent identity check failed",
              file=sys.stderr)
        return 1
    if args.expect_full and (
            result["validation"]["historical"]["status"] != "pass"):
        print("profile_corpus: historical full-corpus reference mismatch "
              "(expected when the generated corpus changed)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
