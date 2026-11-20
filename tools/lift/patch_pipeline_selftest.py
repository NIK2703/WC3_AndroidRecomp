#!/usr/bin/env python3
"""Synthetic tests for the general patch pipeline."""
from __future__ import annotations

import argparse
import json
import tempfile
from pathlib import Path

from patch_pipeline import (
    PipelineError,
    apply_pipeline,
    build_index,
    cmd_index,
)


FAKE_ENGINE = r'''#!/usr/bin/env python3
import json
import re
import sys

def value(name, default=None):
    try:
        index = sys.argv.index(name)
    except ValueError:
        return default
    if index + 1 >= len(sys.argv):
        return default
    return sys.argv[index + 1]

lift = value("--lift")
out = value("--out")
report = value("--json")
tag = value("--tag", "?")
expect = int(value("--expect-sites", "-1"))
if not lift or not out or not report:
    print("fake_engine: missing path", file=sys.stderr)
    sys.exit(2)
text = open(lift, encoding="utf-8").read()
sites = text.count("SITE")
if sites != expect:
    print(f"fake_engine: {sites} site(s), expected {expect}", file=sys.stderr)
    sys.exit(3)
text += f"\n/* {tag} */\n"
if "--marker" in sys.argv and "FAKE_MARKER" not in text:
    text += '\nconst char fake_marker[] = "FAKE_MARKER";\n'
open(out, "w", encoding="utf-8").write(text)
funcs = re.findall(r"void (L_[0-9A-F]+)\(CPU \*restrict c\)", text)
data = {
    "pass": "fake_engine.py",
    "tag": tag,
    "files": [{"sites": sites, "applied": True,
               "functions": [{"function": f} for f in funcs]}],
    "applied": True,
    "marker_count": text.count("FAKE_MARKER"),
}
json.dump(data, open(report, "w", encoding="utf-8"), indent=2)
'''

FAKE_BATCH = r'''#!/usr/bin/env python3
import json
import sys

def value(name, default=None):
    try:
        index = sys.argv.index(name)
    except ValueError:
        return default
    if index + 1 >= len(sys.argv):
        return default
    return sys.argv[index + 1]

inputs = value("--inputs")
out_dir = value("--out-dir")
report = value("--json")
tag = value("--tag", "BATCH")
if not inputs or not out_dir or not report:
    print("fake_batch: missing path", file=sys.stderr)
    sys.exit(2)
paths = [line.strip() for line in open(inputs, encoding="utf-8") if line.strip()]
for path in paths:
    text = open(path, encoding="utf-8").read()
    text += f"\n/* {tag} */\n"
    name = path.replace("\\", "/").rsplit("/", 1)[-1]
    open(out_dir + "/" + name, "w", encoding="utf-8").write(text)
json.dump({"verdict": "pass", "inputs": len(paths)},
          open(report, "w", encoding="utf-8"), indent=2)
'''



FAKE_PREPARE = r'''#!/usr/bin/env python3
import sys

def value(name):
    try:
        return sys.argv[sys.argv.index(name) + 1]
    except (ValueError, IndexError):
        return None

out = value("--index-out")
paths = []
i = 1
while i < len(sys.argv):
    if sys.argv[i] == "--input" and i + 1 < len(sys.argv):
        paths.append(sys.argv[i + 1])
        i += 2
    else:
        i += 1
if not out or not paths:
    print("fake_prepare: missing input", file=sys.stderr)
    sys.exit(2)
open(out, "w", encoding="utf-8").write(
    str(len(paths)) + "\n" + "\n".join(paths) + "\n")
'''

FAKE_GATE = r'''#!/usr/bin/env python3
import json
import sys

def value(name):
    try:
        return sys.argv[sys.argv.index(name) + 1]
    except (ValueError, IndexError):
        return None

report = value("--json")
paths = []
i = 1
while i < len(sys.argv):
    if sys.argv[i] == "--file" and i + 1 < len(sys.argv):
        paths.append(sys.argv[i + 1])
        i += 2
    else:
        i += 1
if not report or not paths:
    print("fake_gate: missing input", file=sys.stderr)
    sys.exit(2)
json.dump({"verdict": "pass", "files": len(paths)},
          open(report, "w", encoding="utf-8"), indent=2)
'''

def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="wc3_patch_pipeline_") as tmp:
        root = Path(tmp)
        base = root / "base"
        engine_dir = root / "engine"
        base.mkdir()
        engine_dir.mkdir()
        per_chunk = engine_dir / "fake_engine.py"
        batch_engine = engine_dir / "fake_batch.py"
        prepare_engine = engine_dir / "fake_prepare.py"
        gate_engine = engine_dir / "fake_gate.py"
        per_chunk.write_text(FAKE_ENGINE, encoding="utf-8")
        batch_engine.write_text(FAKE_BATCH, encoding="utf-8")
        prepare_engine.write_text(FAKE_PREPARE, encoding="utf-8")
        gate_engine.write_text(FAKE_GATE, encoding="utf-8")
        per_chunk.chmod(0o755)
        batch_engine.chmod(0o755)
        prepare_engine.chmod(0o755)
        gate_engine.chmod(0o755)
        chunk1 = base / "game" / "closed" / "game_closed_0001.c"
        chunk2 = base / "game" / "closed" / "game_closed_0002.c"
        chunk1.parent.mkdir(parents=True)
        chunk1.write_text(
            '#include "cpu.h"\n'
            "void L_00000001(CPU *restrict c)\n"
            "{\n    /* SITE */\n}\n",
            encoding="utf-8",
        )
        chunk2.write_text(
            '#include "cpu.h"\n'
            "void L_00000002(CPU *restrict c)\n"
            "{\n    /* SITE */\n}\n",
            encoding="utf-8",
        )

        def stage(
            patch_id: str,
            target: str,
            order: int,
            tag: str,
            *,
            marker: bool = False,
            after: list[str] | None = None,
        ) -> dict:
            return {
                "schema": 2,
                "id": patch_id,
                "engine": "fake_engine.py",
                "kind": "transform",
                "mode": "per_chunk",
                "scope": "targets",
                "order": order,
                "after": after or [],
                "targets": [{"function": target, "sites": 1}],
                "expect_sites": 1,
                "command": {
                    "args": [
                        "--lift", "{in}", "--out", "{out}",
                        "--json", "{report}", "--expect-sites", "{sites}",
                        "--tag", tag,
                    ],
                    "first_chunk_args": ["--marker"] if marker else [],
                },
                "assertions": [
                    {"kind": "report_pass"},
                    {"kind": "site_count", "path": "files[0].sites"},
                    {"kind": "function_coverage",
                     "path": "files[0].functions"},
                    {"kind": "output_changed"},
                ],
            }

        a = stage("A", "L_00000001", 10, "A", marker=True)
        a["assertions"].extend([
            {"kind": "marker_count", "string": "FAKE_MARKER", "equals": 1},
            {"kind": "marker_defined_once", "symbol": "fake_marker"},
        ])
        b = stage("B", "L_00000002", 20, "B", after=["A"])
        b["targets"] = [
            {"function": "L_00000001", "sites": 1},
            {"function": "L_00000002", "sites": 1},
        ]
        b["expect_sites"] = 2
        c = {
            "schema": 2,
            "id": "C",
            "engine": "fake_batch.py",
            "kind": "transform",
            "mode": "batch",
            "scope": "all",
            "order": 30,
            "after": ["B"],
            "output_layout": "flat",
            "command": {
                "args": [
                    "--inputs", "{inputs}", "--out-dir", "{out_dir}",
                    "--json", "{report}", "--tag", "BATCH",
                ]
            },
            "assertions": [
                {"kind": "field_equals", "path": "verdict",
                 "equals": "pass"},
                {"kind": "field_equals", "path": "inputs",
                 "equals_chunk_count": True},
            ],
        }
        prep = {
            "schema": 2,
            "id": "PREP",
            "engine": "fake_prepare.py",
            "kind": "prepare",
            "mode": "once",
            "scope": "once",
            "order": 1,
            "command": {
                "args": ["--index-out", "{state}/fake.index"],
                "inputs_args": ["--input"],
                "outputs": ["{state}/fake.index"],
            },
        }
        gate = {
            "schema": 2,
            "id": "GATE",
            "engine": "fake_gate.py",
            "kind": "gate",
            "mode": "once",
            "scope": "once",
            "order": 40,
            "after": ["C"],
            "command": {
                "args": ["--json", "{report}"],
                "inputs_args": ["--file"],
            },
            "assertions": [
                {"kind": "field_equals", "path": "verdict",
                 "equals": "pass"},
                {"kind": "field_equals", "path": "files",
                 "equals_chunk_count": True},
            ],
        }
        manifest = root / "stages.json"
        manifest.write_text(
            json.dumps(
                {"schema": 2, "patches": [prep, a, b, c, gate]}, indent=2
            ),
            encoding="utf-8",
        )
        index_path = root / "index.tsv"
        cmd_index(argparse.Namespace(base=str(base), out=str(index_path)))
        out = root / "overlay"
        apply_args = argparse.Namespace(
            base=str(base),
            index=str(index_path),
            registry=None,
            manifest=[str(manifest)],
            patch=[],
            pipeline=None,
            reuse=False,
            all=True,
            include_disabled=False,
            param=[],
            engine_dir=str(engine_dir),
            out=str(out),
            force=False,
            keep_work=False,
            allow_idempotent=False,
            expected_dir=None,
            flat=False,
            quiet=True,
        )
        overlay = apply_pipeline(apply_args)
        if overlay["totals"]["stages"] != 5:
            raise PipelineError(
                f"self-test: bad stage count {overlay['totals']['stages']}"
            )
        text1 = (out / "game" / "closed" / "game_closed_0001.c").read_text(
            encoding="utf-8"
        )
        text2 = (out / "game" / "closed" / "game_closed_0002.c").read_text(
            encoding="utf-8"
        )
        for tag in ("/* A */", "/* B */", "/* BATCH */"):
            if tag not in text1:
                raise PipelineError(
                    f"self-test: {tag} missing from chunk 1"
                )
        if text1.index("/* A */") > text1.index("/* B */"):
            raise PipelineError("self-test: stage order was not preserved")
        if "/* A */" in text2 or "/* B */" not in text2:
            raise PipelineError("self-test: target chunk set is wrong")
        if text1.count("FAKE_MARKER") != 1:
            raise PipelineError("self-test: marker was not emitted once")
        bad_args = argparse.Namespace(**vars(apply_args))
        bad = root / "bad.json"
        bad.write_text(
            json.dumps(
                {
                    "schema": 2,
                    "id": "bad",
                    "engine": "fake_engine.py",
                    "targets": [{"function": "L_DEADBEEF"}],
                    "command": {"args": ["--lift", "{in}", "--out",
                                         "{out}"]},
                }
            ),
            encoding="utf-8",
        )
        bad_args.manifest = [str(bad)]
        bad_args.all = True
        bad_args.out = str(root / "bad_overlay")
        try:
            apply_pipeline(bad_args)
        except PipelineError:
            pass
        else:
            raise PipelineError(
                "self-test: missing target did not fail"
            )
        dup = root / "dup"
        (dup / "game" / "closed").mkdir(parents=True)
        for name in ("game_closed_0001.c", "game_closed_0002.c"):
            (dup / "game" / "closed" / name).write_text(
                "void L_00000003(CPU *restrict c)\n{\n}\n",
                encoding="utf-8",
            )
        try:
            build_index(dup)
        except PipelineError:
            pass
        else:
            raise PipelineError(
                "self-test: duplicate function was accepted"
            )
    print("patch_pipeline self-test: ok")
    return 0


def run() -> int:
    return self_test()


if __name__ == "__main__":
    raise SystemExit(run())
