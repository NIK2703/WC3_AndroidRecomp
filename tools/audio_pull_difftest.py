#!/usr/bin/env python3
"""S-5: semantic difftest harness for the WINMM pull path (V5.9 / E11).

This is an executable harness, not a verdict.  It compares a DUT replay with
an oracle golden set and reports the three semantic invariants V5.9:

  1. WOM_DONE order            - completions must arrive in FIFO submit order
  2. WHDR_DONE moment          - the flag is raised at consumption, before the
                                callback, never after a resubmission
  3. Miles SPSC w/r contract   - 0 <= W-R <= N, stable TimerProc gid, no
                                waveOutWrite from inside waveOutProc

E11: without golden sets E3/E4 and oracle identity V3 the verdict is
``unavailable``, NEVER ``pass``.  A green PCM hash alone is not acceptance.
The harness never invents a threshold or a budget (PM-2).

Inputs are plain JSON so a device run, the headless DUT and a future oracle
capture can all feed the same comparison:

  golden.json: {"oracle_identity": "...", "wom_done": [...],
                "whdr_done": [...], "miles": [{"w":..,"r":..,"N":..}],
                "writes_per_tick": [...], "pcm_sha256": "..."}
  dut.json:    same shape, from the headless replay / device log

Usage:
  python tools/audio_pull_difftest.py --golden G --dut D --json OUT
  python tools/audio_pull_difftest.py --self-test
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = 1


def _seq(doc, key):
    value = doc.get(key)
    if not isinstance(value, list):
        return None
    return value


def compare(golden: dict, dut: dict) -> dict:
    report = {
        "schema": SCHEMA,
        "checks": [],
        "verdict": "unavailable",
        "reasons": [],
    }

    g_identity = golden.get("oracle_identity")
    d_identity = dut.get("oracle_identity")
    if not g_identity or not d_identity:
        report["reasons"].append(
            "E3/E4 golden sets or oracle identity V3 are missing")
        return report
    if g_identity != d_identity:
        report["verdict"] = "fail"
        report["reasons"].append("oracle identity differs")
        return report

    checks = (
        ("wom_done", "WOM_DONE FIFO order"),
        ("whdr_done", "WHDR_DONE moment"),
        ("writes_per_tick", "waveOutWrite per timer tick"),
    )
    for key, label in checks:
        g, d = _seq(golden, key), _seq(dut, key)
        if g is None or d is None or not g or not d:
            report["reasons"].append("%s: golden or DUT series absent" % label)
            continue
        equal = list(g) == list(d)
        report["checks"].append({"name": label, "equal": equal,
                                 "golden": len(g), "dut": len(d)})
        if not equal:
            report["verdict"] = "fail"
            report["reasons"].append("%s differs from oracle" % label)

    miles = _seq(dut, "miles") or []
    if not miles:
        report["reasons"].append("Miles w/r/N series absent in DUT")
    else:
        bad = []
        gids = set()
        writes_in_callback = 0
        for row in miles:
            if not isinstance(row, dict):
                continue
            w, r, n = row.get("w"), row.get("r"), row.get("N")
            if isinstance(w, int) and isinstance(r, int) and isinstance(n, int):
                if not (0 <= w - r <= n):
                    bad.append(row)
            if "gid" in row:
                gids.add(row["gid"])
            if row.get("ev") == "violation":
                writes_in_callback += 1
        report["checks"].append({
            "name": "Miles SPSC 0 <= W-R <= N",
            "equal": not bad, "violations": len(bad),
            "timer_gids": len(gids),
            "waveOutWrite_in_callback": writes_in_callback,
        })
        if bad or len(gids) > 1 or writes_in_callback:
            report["verdict"] = "fail"
            report["reasons"].append("V5.9 semantic invariant violated")

    g_pcm, d_pcm = golden.get("pcm_sha256"), dut.get("pcm_sha256")
    if not g_pcm or not d_pcm:
        report["reasons"].append("PCM hash missing on one side")
    else:
        report["checks"].append({"name": "PCM sha256",
                                 "equal": g_pcm == d_pcm})
        if g_pcm != d_pcm:
            report["verdict"] = "fail"
            report["reasons"].append("PCM content differs from oracle")

    if report["verdict"] != "fail":
        if report["reasons"]:
            report["verdict"] = "unavailable"
        else:
            report["verdict"] = "pass"
    return report


def self_test() -> int:
    base = {
        "oracle_identity": "1.27.1.7085/scenario-menu/sha256:abc",
        "wom_done": [1, 2, 3],
        "whdr_done": [1, 2, 3],
        "writes_per_tick": [1, 1, 1],
        "miles": [{"w": 1, "r": 0, "N": 5, "gid": 4116, "ev": "write"}],
        "pcm_sha256": hashlib.sha256(b"pcm").hexdigest(),
    }
    good = compare(base, dict(base))
    assert good["verdict"] == "pass", good

    missing = compare({}, dict(base))
    assert missing["verdict"] == "unavailable", missing

    broken = dict(base)
    broken["wom_done"] = [3, 2, 1]
    bad = compare(base, broken)
    assert bad["verdict"] == "fail", bad

    starving = dict(base)
    starving["miles"] = [{"w": 9, "r": 0, "N": 5, "gid": 4116,
                          "ev": "write"}]
    assert compare(base, starving)["verdict"] == "fail"

    print("audio_pull_difftest: pass / fail / unavailable self-test ok")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--golden", type=Path)
    ap.add_argument("--dut", type=Path)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.golden or not args.dut:
        print("need --golden and --dut (or --self-test)", file=sys.stderr)
        return 2
    golden = json.loads(args.golden.read_text(encoding="utf-8"))
    dut = json.loads(args.dut.read_text(encoding="utf-8"))
    report = compare(golden, dut)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n",
                             encoding="utf-8")
    print("audio_pull_difftest: %s" % report["verdict"])
    for reason in report["reasons"]:
        print("  - %s" % reason)
    return 1 if report["verdict"] == "fail" else 0


if __name__ == "__main__":
    raise SystemExit(main())
