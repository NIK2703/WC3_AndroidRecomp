#!/usr/bin/env python3
"""General build-time patch pipeline for the lifted WC3 corpus.

The registry, stage modes, targets, assertions and provenance contract are
documented in `tools/lift/patches/README.md`.  This module only implements the
generic execution model; patch names live in registry manifests.
"""
from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = 2
DEFAULT_REGISTRY = ROOT / "tools" / "lift" / "patches" / "registry.json"
FUNC_RE = re.compile(rb"^void (L_[0-9A-Fa-f]+)\(CPU \*restrict c\)", re.M)
CHUNK_RE = re.compile(r"^([A-Za-z0-9]+)_(seed|closed)_\d+\.c$")
HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")
PATH_TOKEN_RE = re.compile(r"([A-Za-z0-9_]+)|\[(\d+)\]")


def retire_dir(path: Path) -> None:
    """Move a previous build output aside instead of bulk-deleting it.

    A patch overlay contains thousands of generated files.  Deleting them on
    every --force configure is both slow and blocked by host guards on bulk
    deletes; a rename keeps the old artifact inspectable and gives the new run
    a clean directory.
    """
    if not path.exists():
        return
    stamp = time.strftime("%Y%m%d-%H%M%S")
    candidate = path.with_name(f"{path.name}.stale-{stamp}")
    suffix = 0
    while candidate.exists():
        suffix += 1
        candidate = path.with_name(f"{path.name}.stale-{stamp}-{suffix}")
    path.rename(candidate)

class PipelineError(RuntimeError):
    """A fail-closed pipeline condition."""

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")

def canonical_sha256(value: Any) -> str:
    return sha256_bytes(canonical_bytes(value))

def norm_func(value: Any) -> str:
    if isinstance(value, int):
        text = f"{value:X}"
    else:
        text = str(value).strip()
    if text.startswith("L_"):
        text = text[2:]
    if text.startswith("0x") or text.startswith("0X"):
        text = text[2:]
    if not text or not HEX_RE.fullmatch(text):
        raise PipelineError(f"invalid function VA/name: {value!r}")
    return "L_" + text.upper()

def infer_module_kind(rel: str) -> tuple[str, str]:
    match = CHUNK_RE.match(Path(rel).name)
    if match:
        return match.group(1), match.group(2)
    return Path(rel).parent.name, ""

@dataclass(frozen=True)
class IndexEntry:
    module: str
    kind: str
    func: str
    chunk: str

@dataclass(frozen=True)
class Target:
    func: str
    sites: int
    profiles: tuple[str, ...]
    module: str | None
    kind: str | None
    why: str

@dataclass(frozen=True)
class Patch:
    id: str
    rev: int | None
    order: int
    kind: str
    mode: str
    scope: str
    engine: str
    module: str | None
    profiles: tuple[str, ...]
    after: tuple[str, ...]
    targets: tuple[Target, ...]
    expect_sites: Any
    params: dict[str, str]
    command: dict[str, Any]
    assertions: tuple[dict[str, Any], ...]
    coverage: tuple[str, ...]
    coverage_mode: str
    output_layout: str
    manifest_path: Path
    manifest_sha256: str
    enabled: bool

@dataclass(frozen=True)
class ResolvedTarget:
    target: Target
    entry: IndexEntry

@dataclass(frozen=True)
class ResolvedChunk:
    patch: Patch
    chunk: str
    targets: tuple[ResolvedTarget, ...]
    sites: int
    first: bool

@dataclass(frozen=True)
class Scope:
    sites: int
    chunk_count: int
    targets: tuple[ResolvedTarget, ...]

@dataclass(frozen=True)
class StageWork:
    patch: Patch
    chunks: tuple[ResolvedChunk, ...]
    scope: Scope

class WorkTree:
    """Private copy-on-write view of the corpus.

    Base files are hardlinked when possible.  A stage always writes to a
    separate output path, then `replace()` atomically swaps the work link, so
    the base inode is never modified.
    """

    def __init__(self, root: Path, base: Path, chunks: list[str]) -> None:
        self.root = root
        self.base = base
        self.paths: dict[str, Path] = {}
        for chunk in chunks:
            src = base / chunk
            if not src.is_file():
                raise PipelineError(f"base chunk does not exist: {chunk}")
            dest = root / chunk
            dest.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(src, dest)
            except (OSError, AttributeError):
                shutil.copyfile(src, dest)
            self.paths[chunk] = dest

    def path(self, chunk: str) -> Path:
        try:
            return self.paths[chunk]
        except KeyError as exc:
            raise PipelineError(f"work tree has no chunk: {chunk}") from exc

    def replace(self, chunk: str, src: Path) -> None:
        dest = self.path(chunk)
        tmp = dest.with_name(dest.name + ".tmp")
        shutil.copyfile(src, tmp)
        os.replace(tmp, dest)

def build_index(base: Path) -> dict[tuple[str, str, str], IndexEntry]:
    if not base.is_dir():
        raise PipelineError(f"base directory does not exist: {base}")
    entries: dict[tuple[str, str, str], IndexEntry] = {}
    chunks = sorted(base.rglob("*.c"))
    if not chunks:
        raise PipelineError(f"no .c chunks under {base}")
    for path in chunks:
        rel = path.relative_to(base).as_posix()
        module, kind = infer_module_kind(rel)
        data = path.read_bytes()
        for match in FUNC_RE.finditer(data):
            func = norm_func(match.group(1).decode("ascii"))
            key = (module, kind, func)
            prev = entries.get(key)
            if prev is not None:
                raise PipelineError(
                    "duplicate function definition "
                    f"{func} in module '{module}' kind '{kind}': "
                    f"{prev.chunk} and {rel}"
                )
            entries[key] = IndexEntry(module, kind, func, rel)
    if not entries:
        raise PipelineError(f"no lifted functions found under {base}")
    return entries

def index_chunks(entries: dict[tuple[str, str, str], IndexEntry]) -> list[str]:
    return sorted({entry.chunk for entry in entries.values()})

def write_index(
    entries: dict[tuple[str, str, str], IndexEntry], out: Path
) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = ["func_va\tmodule\tkind\tchunk\n"]
    for key in sorted(entries):
        entry = entries[key]
        lines.append(
            f"{entry.func}\t{entry.module}\t{entry.kind}\t{entry.chunk}\n"
        )
    out.write_text("".join(lines), encoding="utf-8")

def read_index(path: Path) -> dict[tuple[str, str, str], IndexEntry]:
    if not path.is_file():
        raise PipelineError(f"chunk index does not exist: {path}")
    entries: dict[tuple[str, str, str], IndexEntry] = {}
    for lineno, raw in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = raw.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] == "func_va":
            continue
        if len(parts) != 4:
            raise PipelineError(
                f"{path}:{lineno}: expected 4 tab-separated columns"
            )
        func = norm_func(parts[0])
        module, kind, chunk = parts[1], parts[2], parts[3]
        key = (module, kind, func)
        if key in entries:
            raise PipelineError(
                f"{path}:{lineno}: duplicate index entry "
                f"{module}/{kind}/{func}"
            )
        entries[key] = IndexEntry(module, kind, func, chunk)
    if not entries:
        raise PipelineError(f"chunk index is empty: {path}")
    return entries

def _as_int(value: Any, what: str) -> int:
    if isinstance(value, bool):
        raise PipelineError(f"{what} must be an integer")
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise PipelineError(f"{what} must be an integer, got {value!r}") from exc

def _str_list(value: Any, what: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or not all(
        isinstance(item, str) for item in value
    ):
        raise PipelineError(f"{what} must be a list of strings")
    return tuple(value)

def _bool(value: Any, what: str) -> bool:
    if not isinstance(value, bool):
        raise PipelineError(f"{what} must be a boolean")
    return value

def _patch_object(
    item: dict[str, Any],
    path: Path,
    digest: str,
    enabled: bool,
) -> Patch:
    if not isinstance(item, dict):
        raise PipelineError(f"{path}: stage is not an object")
    if item.get("schema") not in (None, SCHEMA):
        raise PipelineError(f"{path}: expected schema {SCHEMA}")
    patch_id = item.get("id")
    if not isinstance(patch_id, str) or not patch_id:
        raise PipelineError(f"{path}: stage id is missing")
    engine = item.get("engine")
    if not isinstance(engine, str) or not engine:
        raise PipelineError(f"{path}: {patch_id}: engine is missing")
    kind = item.get("kind", "transform")
    if kind not in ("transform", "gate", "prepare", "data", "link"):
        raise PipelineError(
            f"{path}: {patch_id}: unknown kind {kind!r}"
        )
    mode = item.get("mode")
    if mode is None:
        mode = (
            "once"
            if kind in ("gate", "prepare", "data", "link")
            else "per_chunk"
        )
    if mode not in ("per_chunk", "batch", "once"):
        raise PipelineError(f"{path}: {patch_id}: unknown mode {mode!r}")
    scope = item.get("scope")
    if scope is None:
        scope = (
            "once"
            if kind in ("gate", "prepare", "data", "link")
            else "targets"
        )
    if scope not in ("targets", "all", "once"):
        raise PipelineError(f"{path}: {patch_id}: unknown scope {scope!r}")
    if kind == "transform":
        if mode not in ("per_chunk", "batch"):
            raise PipelineError(
                f"{path}: {patch_id}: transform needs per_chunk or batch"
            )
        if scope not in ("targets", "all"):
            raise PipelineError(
                f"{path}: {patch_id}: transform needs targets or all scope"
            )
    else:
        if mode != "once" or scope not in ("once", "all"):
            raise PipelineError(
                f"{path}: {patch_id}: {kind} stage runs once"
            )
    command = item.get("command")
    if not isinstance(command, dict):
        raise PipelineError(f"{path}: {patch_id}: command is missing")
    args = command.get("args")
    if not isinstance(args, list) or not all(
        isinstance(arg, str) for arg in args
    ):
        raise PipelineError(
            f"{path}: {patch_id}: command.args must be strings"
        )
    first_args = command.get("first_chunk_args", [])
    other_args = command.get("other_chunk_args", [])
    if not isinstance(first_args, list) or not all(
        isinstance(arg, str) for arg in first_args
    ):
        raise PipelineError(
            f"{path}: {patch_id}: first_chunk_args must be strings"
        )
    if not isinstance(other_args, list) or not all(
        isinstance(arg, str) for arg in other_args
    ):
        raise PipelineError(
            f"{path}: {patch_id}: other_chunk_args must be strings"
        )
    if mode != "per_chunk" and (first_args or other_args):
        raise PipelineError(
            f"{path}: {patch_id}: chunk-specific args require per_chunk"
        )
    inputs_args = command.get("inputs_args", [])
    if not isinstance(inputs_args, list) or not all(
        isinstance(arg, str) for arg in inputs_args
    ):
        raise PipelineError(
            f"{path}: {patch_id}: command.inputs_args must be strings"
        )
    if inputs_args and mode != "once":
        raise PipelineError(
            f"{path}: {patch_id}: inputs_args require once mode"
        )
    outputs = command.get("outputs", [])
    if not isinstance(outputs, list) or not all(
        isinstance(path_text, str) for path_text in outputs
    ):
        raise PipelineError(
            f"{path}: {patch_id}: command.outputs must be strings"
        )
    if kind == "prepare" and not outputs:
        raise PipelineError(
            f"{path}: {patch_id}: prepare stage needs command.outputs"
        )
    stage_params_raw = item.get("params", {})
    if not isinstance(stage_params_raw, dict):
        raise PipelineError(
            f"{path}: {patch_id}: params must be an object"
        )
    stage_params = {
        str(key): str(value) for key, value in stage_params_raw.items()
    }

    targets_raw = item.get("targets", [])
    if not isinstance(targets_raw, list):
        raise PipelineError(f"{path}: {patch_id}: targets must be a list")
    targets: list[Target] = []
    for target_raw in targets_raw:
        if not isinstance(target_raw, dict):
            raise PipelineError(
                f"{path}: {patch_id}: target is not an object"
            )
        if "function" not in target_raw:
            raise PipelineError(
                f"{path}: {patch_id}: target has no function"
            )
        sites = _as_int(
            target_raw.get("sites", 1),
            f"{path}: {patch_id}: target sites",
        )
        if sites <= 0:
            raise PipelineError(
                f"{path}: {patch_id}: target sites must be positive"
            )
        module = target_raw.get("module")
        target_kind = target_raw.get("kind")
        if module is not None and not isinstance(module, str):
            raise PipelineError(
                f"{path}: {patch_id}: target module must be string"
            )
        if target_kind is not None and not isinstance(target_kind, str):
            raise PipelineError(
                f"{path}: {patch_id}: target kind must be string"
            )
        targets.append(
            Target(
                func=norm_func(target_raw["function"]),
                sites=sites,
                profiles=_str_list(
                    target_raw.get("profiles"),
                    f"{path}: {patch_id}: target profiles",
                ),
                module=module,
                kind=target_kind,
                why=str(target_raw.get("why", "")),
            )
        )
    if scope == "targets" and not targets:
        raise PipelineError(
            f"{path}: {patch_id}: targets scope requires targets"
        )
    if scope == "all" and targets:
        raise PipelineError(
            f"{path}: {patch_id}: all scope cannot carry targets; "
            "use assertions for expected counts"
        )

    expect_sites = item.get("expect_sites")
    if expect_sites is not None and not isinstance(expect_sites, (int, dict)):
        raise PipelineError(
            f"{path}: {patch_id}: expect_sites must be int or object"
        )
    assertions_raw = item.get("assertions", [])
    if not isinstance(assertions_raw, list) or not all(
        isinstance(assertion, dict) for assertion in assertions_raw
    ):
        raise PipelineError(
            f"{path}: {patch_id}: assertions must be objects"
        )
    coverage_raw = item.get("coverage", [])
    if isinstance(coverage_raw, list):
        coverage = _str_list(
            coverage_raw, f"{path}: {patch_id}: coverage"
        )
        coverage_mode = "args"
    elif isinstance(coverage_raw, dict):
        coverage = _str_list(
            coverage_raw.get("args"),
            f"{path}: {patch_id}: coverage.args",
        )
        coverage_mode = coverage_raw.get("reports", "args")
        if coverage_mode not in ("args", "manifest"):
            raise PipelineError(
                f"{path}: {patch_id}: coverage.reports must be "
                "args or manifest"
            )
    else:
        raise PipelineError(
            f"{path}: {patch_id}: coverage must be a list or object"
        )
    output_layout = item.get("output_layout", "preserve")
    if output_layout not in ("preserve", "flat"):
        raise PipelineError(
            f"{path}: {patch_id}: unknown output_layout {output_layout!r}"
        )
    module = item.get("module")
    if module is not None and not isinstance(module, str):
        raise PipelineError(f"{path}: {patch_id}: module must be string")
    rev = item.get("rev")
    if rev is not None:
        rev = _as_int(rev, f"{path}: {patch_id}: rev")
    return Patch(
        id=patch_id,
        rev=rev,
        order=_as_int(
            item.get("order", 1000), f"{path}: {patch_id}: order"
        ),
        kind=kind,
        mode=mode,
        scope=scope,
        engine=engine,
        module=module,
        profiles=_str_list(
            item.get("profiles"), f"{path}: {patch_id}: profiles"
        ),
        after=_str_list(
            item.get("after"), f"{path}: {patch_id}: after"
        ),
        targets=tuple(targets),
        expect_sites=expect_sites,
        params=stage_params,
        command={
            "args": args,
            "first_chunk_args": first_args,
            "other_chunk_args": other_args,
            "inputs_args": inputs_args,
            "outputs": outputs,
        },
        assertions=tuple(assertions_raw),
        coverage=coverage,
        coverage_mode=coverage_mode,
        output_layout=output_layout,
        manifest_path=path,
        manifest_sha256=digest,
        enabled=enabled,
    )

def _load_json(path: Path) -> tuple[dict[str, Any], bytes]:
    if not path.is_file():
        raise PipelineError(f"manifest does not exist: {path}")
    raw = path.read_bytes()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise PipelineError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise PipelineError(f"{path}: expected a JSON object")
    return data, raw

def load_manifest_files(paths: list[Path]) -> list[Patch]:
    patches: list[Patch] = []
    seen: set[str] = set()
    for path in paths:
        data, raw = _load_json(path)
        if data.get("schema") != SCHEMA:
            raise PipelineError(f"{path}: expected schema {SCHEMA}")
        digest = sha256_bytes(raw)
        items = data.get("patches")
        if items is None:
            items = [data]
        if not isinstance(items, list) or not items:
            raise PipelineError(f"{path}: no stages")
        for item in items:
            patch = _patch_object(item, path, digest, True)
            if patch.id in seen:
                raise PipelineError(f"duplicate stage id: {patch.id}")
            seen.add(patch.id)
            patches.append(patch)
    return patches

def load_registry(path: Path) -> tuple[list[Patch], str, dict[str, Any]]:
    data, raw = _load_json(path)
    if data.get("schema") != SCHEMA:
        raise PipelineError(f"{path}: expected schema {SCHEMA}")
    stages = data.get("stages")
    if not isinstance(stages, list) or not stages:
        raise PipelineError(f"{path}: registry has no stages")
    patches: list[Patch] = []
    seen: set[str] = set()
    for item in stages:
        if isinstance(item, str):
            manifest = path.parent / item
            enabled = True
            inline = None
        elif isinstance(item, dict):
            if "manifest" in item:
                manifest = path.parent / str(item["manifest"])
                enabled = _bool(
                    item.get("enabled", True),
                    f"{path}: stage enabled",
                )
                inline = None
            elif "id" in item:
                manifest = path
                enabled = _bool(
                    item.get("enabled", True),
                    f"{path}: stage enabled",
                )
                inline = item
            else:
                raise PipelineError(
                    f"{path}: stage needs manifest or id"
                )
        else:
            raise PipelineError(f"{path}: stage entry is not an object")
        if inline is not None:
            digest = canonical_sha256(inline)
            patch = _patch_object(inline, path, digest, enabled)
            if patch.id in seen:
                raise PipelineError(f"duplicate stage id: {patch.id}")
            seen.add(patch.id)
            patches.append(patch)
            continue
        file_data, file_raw = _load_json(manifest)
        if file_data.get("schema") != SCHEMA:
            raise PipelineError(f"{manifest}: expected schema {SCHEMA}")
        digest = sha256_bytes(file_raw)
        items = file_data.get("patches")
        if items is None:
            items = [file_data]
        if not isinstance(items, list) or not items:
            raise PipelineError(f"{manifest}: no stages")
        for patch_item in items:
            patch = _patch_object(
                patch_item, manifest, digest, enabled
            )
            if patch.id in seen:
                raise PipelineError(f"duplicate stage id: {patch.id}")
            seen.add(patch.id)
            patches.append(patch)
    return patches, sha256_bytes(raw), data

def select_patches(
    patches: list[Patch],
    ids: list[str],
    select_all: bool,
    include_disabled: bool,
) -> tuple[list[Patch], list[str]]:
    if select_all and ids:
        raise PipelineError("use either --all or --patch, not both")
    if not select_all and not ids:
        raise PipelineError("select stages with --patch ID or --all")
    by_id = {patch.id: patch for patch in patches}
    if select_all:
        selected = [
            patch
            for patch in patches
            if patch.enabled or include_disabled
        ]
    else:
        missing = [patch_id for patch_id in ids if patch_id not in by_id]
        if missing:
            raise PipelineError(
                "unknown stage id(s): " + ", ".join(sorted(missing))
            )
        selected = [by_id[patch_id] for patch_id in ids]
    if not selected:
        raise PipelineError("no stages selected")
    selected_ids = {patch.id for patch in selected}
    indegree: dict[str, int] = {patch.id: 0 for patch in selected}
    outgoing: dict[str, list[str]] = defaultdict(list)
    external: list[str] = []
    for patch in selected:
        for dep in patch.after:
            if dep in selected_ids:
                indegree[patch.id] += 1
                outgoing[dep].append(patch.id)
            elif dep:
                external.append(f"{patch.id}->{dep}")
    ready: list[tuple[int, str]] = []
    for patch in selected:
        if indegree[patch.id] == 0:
            heapq.heappush(ready, (patch.order, patch.id))
    ordered: list[Patch] = []
    while ready:
        _, patch_id = heapq.heappop(ready)
        patch = by_id[patch_id]
        ordered.append(patch)
        for child in sorted(outgoing[patch_id]):
            indegree[child] -= 1
            if indegree[child] == 0:
                heapq.heappush(
                    ready, (by_id[child].order, child)
                )
    if len(ordered) != len(selected):
        cycle = ", ".join(
            sorted(patch_id for patch_id, degree in indegree.items() if degree)
        )
        raise PipelineError(f"stage dependency cycle: {cycle}")
    return ordered, sorted(set(external))

def parse_params(items: list[str]) -> dict[str, str]:
    params: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise PipelineError(f"--param expects key=value, got {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise PipelineError(f"--param has an empty key: {item!r}")
        params[key] = value
    return params

def stage_params(patch: Patch, params: dict[str, str]) -> dict[str, str]:
    return {**patch.params, **params}

def pipeline_patch_ids(
    registry_data: dict[str, Any], name: str
) -> list[str]:
    pipelines = registry_data.get("pipelines", {})
    if not isinstance(pipelines, dict) or name not in pipelines:
        raise PipelineError(f"unknown pipeline: {name}")
    ids = pipelines[name]
    if not isinstance(ids, list) or not all(
        isinstance(item, str) for item in ids
    ):
        raise PipelineError(f"pipeline {name}: expected a list of stage ids")
    return list(ids)

def lookup_entry(
    index: dict[tuple[str, str, str], IndexEntry],
    patch: Patch,
    target: Target,
) -> IndexEntry:
    module = target.module or patch.module
    kind = target.kind or None
    matches = [
        entry
        for entry in index.values()
        if entry.func == target.func
        and (module is None or entry.module == module)
        and (kind is None or entry.kind == kind)
    ]
    if not matches:
        where = []
        if module is not None:
            where.append(f"module '{module}'")
        if kind is not None:
            where.append(f"kind '{kind}'")
        suffix = (" in " + ", ".join(where)) if where else ""
        raise PipelineError(
            f"stage {patch.id}: target {target.func} not found{suffix}"
        )
    if len(matches) > 1:
        locations = ", ".join(
            sorted(f"{entry.module}/{entry.kind}:{entry.chunk}"
                   for entry in matches)
        )
        raise PipelineError(
            f"stage {patch.id}: target {target.func} is ambiguous: "
            f"{locations}; add module/kind to the target"
        )
    return matches[0]

def _check_expect_sites(
    patch: Patch, expected: Any, total: int, profile: str
) -> None:
    if expected is None:
        return
    if isinstance(expected, dict):
        if profile not in expected:
            raise PipelineError(
                f"stage {patch.id}: expect_sites has no profile '{profile}'"
            )
        expected = _as_int(
            expected[profile], f"stage {patch.id}: expect_sites"
        )
    else:
        expected = _as_int(expected, f"stage {patch.id}: expect_sites")
    if total != expected:
        raise PipelineError(
            f"stage {patch.id}: {total} target site(s), "
            f"expect_sites is {expected}"
        )

def resolve_plan(
    patches: list[Patch],
    index: dict[tuple[str, str, str], IndexEntry],
    params: dict[str, str],
) -> tuple[list[StageWork], list[str]]:
    all_chunks = index_chunks(index)
    plan: list[StageWork] = []
    for patch in patches:
        effective = stage_params(patch, params)
        profile = effective.get("profile", "")
        if patch.profiles and profile not in patch.profiles:
            raise PipelineError(
                f"stage {patch.id} is not active for profile '{profile}'"
            )
        if patch.kind != "transform":
            plan.append(
                StageWork(
                    patch=patch,
                    chunks=(),
                    scope=Scope(0, len(all_chunks), ()),
                )
            )
            continue
        if patch.scope == "all":
            chunks = [
                ResolvedChunk(
                    patch=patch,
                    chunk=chunk,
                    targets=(),
                    sites=0,
                    first=(index == 0),
                )
                for index, chunk in enumerate(all_chunks)
            ]
            if patch.mode == "per_chunk":
                plan.append(
                    StageWork(
                        patch=patch,
                        chunks=tuple(chunks),
                        scope=Scope(0, len(all_chunks), ()),
                    )
                )
            else:
                plan.append(
                    StageWork(
                        patch=patch,
                        chunks=tuple(chunks),
                        scope=Scope(0, len(all_chunks), ()),
                    )
                )
            continue
        resolved: list[ResolvedTarget] = []
        seen: set[str] = set()
        for target in patch.targets:
            if target.profiles and profile not in target.profiles:
                continue
            if target.func in seen:
                raise PipelineError(
                    f"stage {patch.id}: duplicate target {target.func}"
                )
            seen.add(target.func)
            resolved.append(
                ResolvedTarget(target, lookup_entry(index, patch, target))
            )
        if not resolved:
            raise PipelineError(
                f"stage {patch.id}: no targets for profile '{profile}'"
            )
        total_sites = sum(item.target.sites for item in resolved)
        _check_expect_sites(
            patch, patch.expect_sites, total_sites, profile
        )
        per_chunk: dict[str, list[ResolvedTarget]] = defaultdict(list)
        for item in resolved:
            per_chunk[item.entry.chunk].append(item)
        chunk_ids = sorted(per_chunk)
        if patch.mode == "batch" and patch.output_layout == "flat":
            basenames = [Path(chunk).name for chunk in chunk_ids]
            if len(set(basenames)) != len(basenames):
                raise PipelineError(
                    f"stage {patch.id}: flat batch output has duplicate "
                    "basenames"
                )
        chunks = [
            ResolvedChunk(
                patch=patch,
                chunk=chunk,
                targets=tuple(per_chunk[chunk]),
                sites=sum(item.target.sites for item in per_chunk[chunk]),
                first=(chunk == chunk_ids[0]),
            )
            for chunk in chunk_ids
        ]
        scope = Scope(total_sites, len(chunks), tuple(resolved))
        plan.append(
            StageWork(
                patch=patch, chunks=tuple(chunks), scope=scope
            )
        )
    return plan, all_chunks

def engine_executable(
    engine_dir: Path, name: str
) -> tuple[list[str], Path]:
    candidates = [engine_dir / name]
    if not name.endswith(".exe"):
        candidates.append(engine_dir / (name + ".exe"))
    for candidate in candidates:
        if candidate.is_file():
            if candidate.suffix == ".py":
                return [sys.executable, str(candidate)], candidate
            return [str(candidate)], candidate
    raise PipelineError(
        f"engine '{name}' not found in {engine_dir}; "
        "run tools/lift/build_tools.sh or .ps1 first"
    )

def substitute(value: str, context: dict[str, str], what: str) -> str:
    try:
        return value.format_map(context)
    except KeyError as exc:
        raise PipelineError(
            f"{what}: unknown placeholder {{{exc.args[0]}}}"
        ) from exc

def run_engine(
    cmd: list[str], label: str
) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise PipelineError(
            f"{label}: cannot execute {cmd[0]}: {exc}"
        ) from exc
    if result.returncode != 0:
        raise PipelineError(
            f"{label} failed (rc={result.returncode})\n"
            f"command: {shlex.join(cmd)}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    return result

def json_path(data: Any, path: str) -> Any:
    if not path or path in (".", "/"):
        return data
    current = data
    pos = 0
    for match in PATH_TOKEN_RE.finditer(path):
        if match.start() != pos and path[pos:match.start()].strip("."):
            raise PipelineError(f"invalid report path: {path!r}")
        pos = match.end()
        key, index = match.groups()
        if key is not None:
            if not isinstance(current, dict) or key not in current:
                raise PipelineError(
                    f"report path {path!r}: missing key {key!r}"
                )
            current = current[key]
        else:
            if not isinstance(current, list):
                raise PipelineError(
                    f"report path {path!r}: expected list at {index}"
                )
            position = int(index)
            if position >= len(current):
                raise PipelineError(
                    f"report path {path!r}: index {position} out of range"
                )
            current = current[position]
    if pos != len(path) and path[pos:].strip("."):
        raise PipelineError(f"invalid report path: {path!r}")
    return current

def _compare_values(actual: Any, expected: Any) -> bool:
    if isinstance(expected, bool):
        return bool(actual) == expected
    if isinstance(expected, int):
        try:
            return int(actual) == expected
        except (TypeError, ValueError):
            return False
    return str(actual) == str(expected)

def _expected(
    assertion: dict[str, Any],
    params: dict[str, str],
    scope: Scope,
) -> Any:
    if "equals" in assertion:
        return assertion["equals"]
    if "equals_param" in assertion:
        key = assertion["equals_param"]
        if key not in params:
            raise PipelineError(
                f"assertion refers to missing param {key!r}"
            )
        return params[key]
    if assertion.get("equals_targets"):
        return scope.sites
    if assertion.get("equals_chunk_count"):
        return scope.chunk_count
    raise PipelineError(
        f"assertion {assertion.get('kind')!r} has no expected value"
    )

def _assert_field(
    data: Any,
    assertion: dict[str, Any],
    params: dict[str, str],
    scope: Scope,
) -> None:
    path = assertion.get("path")
    if not isinstance(path, str) or not path:
        raise PipelineError("field_equals assertion has no path")
    actual = json_path(data, path)
    expected = _expected(assertion, params, scope)
    if not _compare_values(actual, expected):
        raise PipelineError(
            f"report field {path!r}: got {actual!r}, expected {expected!r}"
        )

def _status_coverage(
    data: Any,
    assertion: dict[str, Any],
    scope: Scope,
) -> None:
    path = assertion.get("path")
    if not isinstance(path, str) or not path:
        raise PipelineError("status_coverage assertion has no path")
    sites = json_path(data, path)
    if not isinstance(sites, list):
        raise PipelineError(f"report path {path!r} is not a list")
    function_field = assertion.get("function", "function")
    status_field = assertion.get("status", "status")
    applied_statuses = set(
        assertion.get("applied", ["applied", "already"])
    )
    absent_status = assertion.get("absent")
    expected = {
        item.target.func: item.target.sites for item in scope.targets
    }
    if not expected:
        raise PipelineError(
            "status_coverage needs resolved targets"
        )
    seen: dict[str, int] = defaultdict(int)
    for site in sites:
        if not isinstance(site, dict):
            raise PipelineError("site entry is not an object")
        if function_field not in site or status_field not in site:
            raise PipelineError(
                "site entry has no function/status field"
            )
        func = norm_func(site[function_field])
        status = site[status_field]
        if status in applied_statuses:
            if func not in expected:
                raise PipelineError(
                    f"unexpected applied site in {func}"
                )
            seen[func] += 1
        elif absent_status is not None and status == absent_status:
            if func in expected:
                raise PipelineError(
                    f"target site in {func} is absent"
                )
        else:
            raise PipelineError(
                f"unknown site status {status!r} for {func}"
            )
    for func, count in expected.items():
        if seen.get(func, 0) != count:
            raise PipelineError(
                f"{func} has {seen.get(func, 0)} applied site(s), "
                f"expected {count}"
            )

def verify_assertions(
    patch: Patch,
    report: Path,
    scope: Scope,
    params: dict[str, str],
    allow_idempotent: bool,
) -> None:
    if not patch.assertions:
        return
    if not report.is_file():
        raise PipelineError(
            f"stage {patch.id}: report is missing: {report}"
        )
    try:
        data = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PipelineError(
            f"{report}: cannot read report: {exc}"
        ) from exc
    for assertion in patch.assertions:
        kind = assertion.get("kind")
        if kind in ("marker_count", "marker_defined_once"):
            continue
        if kind == "report_pass":
            path = assertion.get("path", "pass")
            actual = json_path(data, path)
            expected = assertion.get("equals", patch.engine)
            if not _compare_values(actual, expected):
                raise PipelineError(
                    f"{report}: {path} is {actual!r}, "
                    f"expected {expected!r}"
                )
        elif kind in ("field_equals", "report_field"):
            _assert_field(data, assertion, params, scope)
        elif kind == "site_count":
            site_assertion = dict(assertion)
            site_assertion.setdefault("equals_targets", True)
            _assert_field(data, site_assertion, params, scope)
        elif kind == "function_coverage":
            path = assertion.get("path")
            if not isinstance(path, str) or not path:
                raise PipelineError(
                    "function_coverage assertion has no path"
                )
            entries = json_path(data, path)
            if not isinstance(entries, list):
                raise PipelineError(
                    f"report path {path!r} is not a list"
                )
            field = assertion.get("field", "function")
            reported = set()
            for entry in entries:
                if not isinstance(entry, dict) or field not in entry:
                    raise PipelineError(
                        f"report path {path!r}: bad function entry"
                    )
                reported.add(norm_func(entry[field]))
            expected = {item.target.func for item in scope.targets}
            missing = expected - reported
            if missing:
                raise PipelineError(
                    "target function(s) missing from report: "
                    + ", ".join(sorted(missing))
                )
        elif kind == "status_coverage":
            _status_coverage(data, assertion, scope)
        elif kind == "counts_sum":
            path = assertion.get("path")
            if not isinstance(path, str) or not path:
                raise PipelineError(
                    "counts_sum assertion has no path"
                )
            counts = json_path(data, path)
            if not isinstance(counts, dict):
                raise PipelineError(
                    f"report path {path!r} is not an object"
                )
            total = sum(
                _as_int(value, f"{report}: counts") for value in counts.values()
            )
            expected = _expected(assertion, params, scope)
            if not _compare_values(total, expected):
                raise PipelineError(
                    f"{report}: counts sum {total}, expected {expected}"
                )
        elif kind == "array_length":
            path = assertion.get("path")
            if not isinstance(path, str) or not path:
                raise PipelineError(
                    "array_length assertion has no path"
                )
            values = json_path(data, path)
            if not isinstance(values, list):
                raise PipelineError(
                    f"report path {path!r} is not a list"
                )
            expected = _expected(assertion, params, scope)
            if not _compare_values(len(values), expected):
                raise PipelineError(
                    f"{report}: {path} length {len(values)}, "
                    f"expected {expected}"
                )
        elif kind == "output_changed":
            if not data.get("applied") and not allow_idempotent:
                raise PipelineError(
                    f"{report}: stage was not applied; base chunk is "
                    "already patched (use --allow-idempotent for an "
                    "explicit idempotent check)"
                )
        else:
            raise PipelineError(
                f"stage {patch.id}: unknown assertion {kind!r}"
            )

def run_coverage(
    patch: Patch,
    reports: list[Path],
    params: dict[str, str],
    engine_dir: Path,
) -> str | None:
    if not patch.coverage:
        return None
    if not reports:
        raise PipelineError(
            f"stage {patch.id}: coverage has no reports"
        )
    engine, _ = engine_executable(engine_dir, patch.engine)
    args = [
        substitute(arg, params, f"stage {patch.id}: coverage")
        for arg in patch.coverage
    ]
    if patch.coverage_mode == "manifest":
        manifest = reports[0].parent / f".coverage-{patch.id}.txt"
        manifest.write_text(
            "".join(str(path) + "\n" for path in reports),
            encoding="utf-8",
        )
        args.append("@" + str(manifest))
    else:
        args.extend(str(path) for path in reports)
    if "--quiet" not in args:
        args.append("--quiet")
    run_engine(engine + args, f"coverage {patch.id}")
    return "ok"

def check_markers(
    patches: list[Patch], texts: dict[str, bytes]
) -> None:
    for patch in patches:
        for assertion in patch.assertions:
            kind = assertion.get("kind")
            if kind == "marker_count":
                needle = assertion.get("string")
                if not isinstance(needle, str) or not needle:
                    raise PipelineError(
                        f"stage {patch.id}: marker_count needs string"
                    )
                expected = _as_int(
                    assertion.get("equals"),
                    f"stage {patch.id}: marker_count equals",
                )
                total = sum(
                    text.count(needle.encode()) for text in texts.values()
                )
                if total != expected:
                    raise PipelineError(
                        f"stage {patch.id}: {total} marker string(s), "
                        f"expected {expected}"
                    )
            elif kind == "marker_defined_once":
                symbol = assertion.get("symbol")
                if not isinstance(symbol, str) or not symbol:
                    raise PipelineError(
                        f"stage {patch.id}: marker_defined_once needs symbol"
                    )
                needle = f"const char {symbol}[]".encode()
                definitions = sum(
                    text.count(needle) for text in texts.values()
                )
                if definitions != 1:
                    raise PipelineError(
                        f"stage {patch.id}: marker symbol is defined "
                        f"{definitions} time(s), expected exactly once"
                    )

def compare_expected(
    expected_dir: Path,
    chunk_ids: list[str],
    texts: dict[str, bytes],
) -> int:
    matched = 0
    by_name = {
        path.name: path for path in expected_dir.rglob("*") if path.is_file()
    }
    for chunk in chunk_ids:
        candidates = [
            expected_dir / chunk,
            expected_dir / Path(chunk).name,
        ]
        expected = next(
            (candidate for candidate in candidates if candidate.is_file()),
            None,
        )
        if expected is None:
            expected = by_name.get(Path(chunk).name)
        if expected is None:
            raise PipelineError(
                f"expected chunk not found for {chunk} under {expected_dir}"
            )
        want = expected.read_bytes()
        got = texts[chunk]
        if want != got:
            offset = next(
                (
                    index
                    for index, (left, right) in enumerate(zip(want, got))
                    if left != right
                ),
                min(len(want), len(got)),
            )
            raise PipelineError(
                f"overlay differs from expected: {chunk} at byte {offset} "
                f"(expected {len(want)} bytes, got {len(got)})"
            )
        matched += 1
    return matched

def _context(
    params: dict[str, str],
    *,
    input_path: Path | None = None,
    output_path: Path | None = None,
    report: Path | None = None,
    inputs_file: Path | None = None,
    out_dir: Path | None = None,
    base: Path,
    work_root: Path,
    overlay: Path,
    sites: int = 0,
    chunk: str = "",
) -> dict[str, str]:
    context = dict(params)
    context.update(
        {
            "base": str(base),
            "root": str(ROOT),
            "work_root": str(work_root),
            "state": str(work_root / "state"),
            "overlay": str(overlay),
            "sites": str(sites),
            "chunk": chunk,
            "chunk_count": "",
        }
    )
    if input_path is not None:
        context["in"] = str(input_path)
    if output_path is not None:
        context["out"] = str(output_path)
    if report is not None:
        context["report"] = str(report)
    if inputs_file is not None:
        context["inputs"] = str(inputs_file)
    if out_dir is not None:
        context["out_dir"] = str(out_dir)
    return context

def _write_inputs(path: Path, chunks: tuple[ResolvedChunk, ...],
                  work: WorkTree) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(str(work.path(chunk.chunk)) + "\n" for chunk in chunks),
        encoding="utf-8",
    )

def _run_per_chunk(
    stage: StageWork,
    *,
    work: WorkTree,
    work_root: Path,
    report_dir: Path,
    base: Path,
    overlay: Path,
    params: dict[str, str],
    engine_dir: Path,
    allow_idempotent: bool,
    quiet: bool,
) -> list[Path]:
    patch = stage.patch
    params = stage_params(patch, params)
    reports: list[Path] = []
    for chunk in stage.chunks:
        input_path = work.path(chunk.chunk)
        output_path = work_root / "stages" / patch.id / chunk.chunk
        output_path.parent.mkdir(parents=True, exist_ok=True)
        report = report_dir / f"{chunk.chunk}.{patch.id}.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        context = _context(
            params,
            input_path=input_path,
            output_path=output_path,
            report=report,
            base=base,
            work_root=work_root,
            overlay=overlay,
            sites=chunk.sites,
            chunk=chunk.chunk,
        )
        context["chunk_count"] = str(len(stage.chunks))
        args = [
            substitute(arg, context, f"stage {patch.id}: command")
            for arg in patch.command["args"]
        ]
        extra = (
            patch.command["first_chunk_args"]
            if chunk.first
            else patch.command["other_chunk_args"]
        )
        args.extend(
            substitute(arg, context, f"stage {patch.id}: chunk args")
            for arg in extra
        )
        engine, _ = engine_executable(engine_dir, patch.engine)
        if not quiet:
            print(f"patch_pipeline: {patch.id} {chunk.chunk}")
        run_engine(engine + args, f"{patch.id} {chunk.chunk}")
        if not output_path.is_file():
            raise PipelineError(
                f"{patch.id} {chunk.chunk}: engine wrote no output"
            )
        verify_assertions(
            patch,
            report,
            Scope(chunk.sites, 1, chunk.targets),
            params,
            allow_idempotent,
        )
        work.replace(chunk.chunk, output_path)
        reports.append(report)
    return reports

def _run_batch(
    stage: StageWork,
    *,
    work: WorkTree,
    work_root: Path,
    report_dir: Path,
    base: Path,
    overlay: Path,
    params: dict[str, str],
    engine_dir: Path,
    allow_idempotent: bool,
    quiet: bool,
) -> list[Path]:
    patch = stage.patch
    params = stage_params(patch, params)
    stage_dir = work_root / "stages" / patch.id
    inputs_file = stage_dir / "inputs.txt"
    out_dir = stage_dir / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    _write_inputs(inputs_file, stage.chunks, work)
    report = report_dir / f"{patch.id}.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    context = _context(
        params,
        inputs_file=inputs_file,
        out_dir=out_dir,
        report=report,
        base=base,
        work_root=work_root,
        overlay=overlay,
        sites=stage.scope.sites,
    )
    context["chunk_count"] = str(len(stage.chunks))
    args = [
        substitute(arg, context, f"stage {patch.id}: command")
        for arg in patch.command["args"]
    ]
    engine, _ = engine_executable(engine_dir, patch.engine)
    if not quiet:
        print(
            f"patch_pipeline: {patch.id} batch "
            f"({len(stage.chunks)} chunk(s))"
        )
    run_engine(engine + args, f"{patch.id} batch")
    verify_assertions(
        patch,
        report,
        stage.scope,
        params,
        allow_idempotent,
    )
    for chunk in stage.chunks:
        if patch.output_layout == "flat":
            candidate = out_dir / Path(chunk.chunk).name
        else:
            candidate = out_dir / chunk.chunk
        if not candidate.is_file():
            raise PipelineError(
                f"{patch.id}: batch output is missing for {chunk.chunk}"
            )
        work.replace(chunk.chunk, candidate)
    return [report]

def _run_once(
    stage: StageWork,
    *,
    work: WorkTree,
    work_root: Path,
    report_dir: Path,
    base: Path,
    overlay: Path,
    params: dict[str, str],
    engine_dir: Path,
    allow_idempotent: bool,
    quiet: bool,
) -> list[Path]:
    patch = stage.patch
    params = stage_params(patch, params)
    if patch.kind in ("data", "link"):
        raise PipelineError(
            f"stage {patch.id}: kind '{patch.kind}' is reserved but not "
            "implemented by this pipeline revision"
        )
    stage_dir = work_root / "stages" / patch.id
    inputs_file = stage_dir / "inputs.txt"
    out_dir = stage_dir / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    chunks = tuple(
        ResolvedChunk(patch, chunk, (), 0, index == 0)
        for index, chunk in enumerate(sorted(work.paths))
    )
    _write_inputs(inputs_file, chunks, work)
    report = report_dir / f"{patch.id}.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    context = _context(
        params,
        inputs_file=inputs_file,
        out_dir=out_dir,
        report=report,
        base=base,
        work_root=work_root,
        overlay=overlay,
        sites=stage.scope.sites,
    )
    context["chunk_count"] = str(len(chunks))
    args = [
        substitute(arg, context, f"stage {patch.id}: command")
        for arg in patch.command["args"]
    ]
    for chunk in chunks:
        for flag in patch.command["inputs_args"]:
            args.extend(
                [
                    substitute(flag, context, f"stage {patch.id}: inputs_args"),
                    str(work.path(chunk.chunk)),
                ]
            )
    engine, _ = engine_executable(engine_dir, patch.engine)
    if not quiet:
        print(f"patch_pipeline: {patch.id} once")
    run_engine(engine + args, f"{patch.id} once")
    for output in patch.command["outputs"]:
        output_path = Path(
            substitute(output, context, f"stage {patch.id}: output")
        )
        if not output_path.is_file():
            raise PipelineError(
                f"stage {patch.id}: declared output is missing: "
                f"{output_path}"
            )
    verify_assertions(
        patch,
        report,
        Scope(0, len(chunks), ()),
        params,
        allow_idempotent,
    )
    return [report]
def reusable_overlay(out: Path, *, base: Path, index_path: Path,
                     patchset_sha256: str, params: dict[str, str],
                     selected: list[Patch], all_chunks: list[str],
                     flat: bool) -> dict[str, Any] | None:
    manifest = out / "overlay.json"
    if not manifest.is_file(): return None
    try:
        data, _ = _load_json(manifest)
    except (OSError, PipelineError):
        return None
    if (
        data.get("base") != str(base)
        or data.get("index_sha256") != sha256_file(index_path)
        or data.get("patchset_sha256") != patchset_sha256
        or data.get("params") != params
        or data.get("output_layout") != ("flat" if flat else "preserve")
    ):
        return None
    if [item.get("id") for item in data.get("stages", [])] != [p.id for p in selected]:
        return None
    chunks = data.get("chunks", [])
    if [item.get("chunk") for item in chunks] != all_chunks: return None
    for item in chunks:
        path = out / item["chunk"]
        if not path.is_file() or sha256_file(path) != item.get("overlay_sha256"):
            return None
        if sha256_file(base / item["chunk"]) != item.get("base_sha256"):
            return None
    return data

def apply_pipeline(args: argparse.Namespace) -> dict[str, Any]:
    base = Path(args.base).resolve()
    index_path = Path(args.index).resolve()
    out = Path(args.out).resolve()
    engine_dir = Path(args.engine_dir).resolve()
    if out == base or base in out.parents:
        raise PipelineError(
            "overlay output must be outside the base directory"
        )
    if args.manifest:
        patches = load_manifest_files(
            [Path(path).resolve() for path in args.manifest]
        )
        registry_sha = None
        registry_data: dict[str, Any] = {}
    else:
        registry_path = Path(args.registry or DEFAULT_REGISTRY).resolve()
        patches, registry_sha, registry_data = load_registry(registry_path)
    if args.pipeline:
        if args.manifest:
            raise PipelineError("--pipeline needs a registry, not --manifest")
        if args.all or args.patch:
            raise PipelineError(
                "use either --pipeline or --patch/--all, not both"
            )
        args.patch = pipeline_patch_ids(registry_data, args.pipeline)
    selected, external = select_patches(
        patches,
        args.patch,
        args.all,
        args.include_disabled,
    )
    params = parse_params(args.param)
    index = read_index(index_path)
    plan, all_chunks = resolve_plan(selected, index, params)

    engine_paths: dict[str, Path] = {}
    patchset_payload = {
        "schema": SCHEMA,
        "patches": [
            {
                "id": patch.id,
                "rev": patch.rev,
                "kind": patch.kind,
                "mode": patch.mode,
                "scope": patch.scope,
                "order": patch.order,
                "after": patch.after,
                "manifest_sha256": patch.manifest_sha256,
            }
            for patch in selected
        ],
        "params": params,
        "engines": {},
    }
    for patch in selected:
        _, engine_path = engine_executable(engine_dir, patch.engine)
        engine_paths[patch.id] = engine_path
        patchset_payload["engines"][patch.id] = sha256_file(engine_path)
    patchset_sha256 = canonical_sha256(patchset_payload)

    if out.exists():
        if not out.is_dir():
            raise PipelineError(f"output path is not a directory: {out}")
        if any(out.iterdir()):
            cached = None
            if args.reuse:
                cached = reusable_overlay(
                    out,
                    base=base,
                    index_path=index_path,
                    patchset_sha256=patchset_sha256,
                    params=params,
                    selected=selected,
                    all_chunks=all_chunks,
                    flat=args.flat,
                )
            if cached is not None:
                print(f"patch_pipeline: reused {out}")
                return cached
            if not args.force:
                raise PipelineError(
                    f"output directory is not empty: {out}; use --force"
                )
            retire_dir(out)
    out.mkdir(parents=True, exist_ok=True)
    work_root = out / ".work"
    report_dir = out / "reports"
    work_root.mkdir(parents=True, exist_ok=True)
    report_dir.mkdir(parents=True, exist_ok=True)
    (work_root / "state").mkdir(parents=True, exist_ok=True)
    work = WorkTree(work_root, base, all_chunks)

    reports_by_patch: dict[str, list[Path]] = defaultdict(list)
    coverage: dict[str, str] = {}
    touched: dict[str, list[str]] = defaultdict(list)
    base_hashes: dict[str, str] = {}
    for chunk in all_chunks:
        base_hashes[chunk] = sha256_file(base / chunk)

    for stage in plan:
        patch = stage.patch
        if patch.kind == "transform":
            if patch.mode == "per_chunk":
                reports = _run_per_chunk(
                    stage,
                    work=work,
                    work_root=work_root,
                    report_dir=report_dir,
                    base=base,
                    overlay=out,
                    params=params,
                    engine_dir=engine_dir,
                    allow_idempotent=args.allow_idempotent,
                    quiet=args.quiet,
                )
            else:
                reports = _run_batch(
                    stage,
                    work=work,
                    work_root=work_root,
                    report_dir=report_dir,
                    base=base,
                    overlay=out,
                    params=params,
                    engine_dir=engine_dir,
                    allow_idempotent=args.allow_idempotent,
                    quiet=args.quiet,
                )
        else:
            reports = _run_once(
                stage,
                work=work,
                work_root=work_root,
                report_dir=report_dir,
                base=base,
                overlay=out,
                params=params,
                engine_dir=engine_dir,
                allow_idempotent=args.allow_idempotent,
                quiet=args.quiet,
            )
        reports_by_patch[patch.id].extend(reports)
        for chunk in stage.chunks:
            touched[chunk.chunk].append(patch.id)
        status = run_coverage(
            patch,
            reports_by_patch[patch.id],
            stage_params(patch, params),
            engine_dir,
        )
        if status is not None:
            coverage[patch.id] = status

    texts: dict[str, bytes] = {}
    chunk_meta: list[dict[str, Any]] = []
    out_layout = "flat" if args.flat else "preserve"
    for chunk in all_chunks:
        data = work.path(chunk).read_bytes()
        texts[chunk] = data
        logical = Path(chunk)
        dest = out / (logical.name if out_layout == "flat" else chunk)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        chunk_meta.append(
            {
                "chunk": chunk,
                "base_sha256": base_hashes[chunk],
                "overlay_sha256": sha256_bytes(data),
                "stages": sorted(set(touched.get(chunk, []))),
            }
        )
    check_markers(selected, texts)
    expected_matches = 0
    if args.expected_dir:
        expected_matches = compare_expected(
            Path(args.expected_dir).resolve(), all_chunks, texts
        )

    overlay_payload = {
        "schema": SCHEMA,
        "mode": "detached",
        "base": str(base),
        "registry_sha256": registry_sha,
        "registry_id": registry_data.get("id"),
        "index_sha256": sha256_file(index_path),
        "patchset_sha256": patchset_sha256,
        "overlay_sha256": canonical_sha256(
            [
                {"chunk": item["chunk"], "sha256": item["overlay_sha256"]}
                for item in chunk_meta
            ]
        ),
        "params": params,
        "stages": [
            {
                "id": patch.id,
                "rev": patch.rev,
                "kind": patch.kind,
                "mode": patch.mode,
                "scope": patch.scope,
                "order": patch.order,
                "after": patch.after,
                "engine": patch.engine,
                "manifest_sha256": patch.manifest_sha256,
                "engine_sha256": sha256_file(engine_paths[patch.id]),
                "params": stage_params(patch, params),
                "assertions": patch.assertions,
                "outputs": patch.command["outputs"],
                "coverage": patch.coverage,
                "coverage_mode": patch.coverage_mode,
                "output_layout": patch.output_layout,
            }
            for patch in selected
        ],
        "chunks": chunk_meta,
        "totals": {
            "stages": len(selected),
            "targets": sum(
                len(item.targets) for stage in plan for item in stage.chunks
            ),
            "sites": sum(
                item.sites for stage in plan for item in stage.chunks
            ),
            "chunks": len(all_chunks),
            "batches": sum(
                1 for patch in selected if patch.mode == "batch"
            ),
            "gates": sum(
                1 for patch in selected if patch.kind == "gate"
            ),
        },
        "external_dependencies": external,
        "coverage": coverage,
        "expected_matches": expected_matches,
        "output_layout": out_layout,
    }
    (out / "overlay.json").write_text(
        json.dumps(overlay_payload, indent=2) + "\n",
        encoding="utf-8",
    )
    resolve_payload = {
        "schema": SCHEMA,
        "base": str(base),
        "index": str(index_path),
        "index_sha256": sha256_file(index_path),
        "params": params,
        "stages": [
            {
                "id": stage.patch.id,
                "mode": stage.patch.mode,
                "scope": stage.patch.scope,
                "chunks": len(stage.chunks),
                "sites": stage.scope.sites,
                "targets": [
                    {
                        "function": item.target.func,
                        "sites": item.target.sites,
                        "chunk": item.entry.chunk,
                    }
                    for item in stage.scope.targets
                ],
            }
            for stage in plan
        ],
    }
    (out / "resolve.json").write_text(
        json.dumps(resolve_payload, indent=2) + "\n",
        encoding="utf-8",
    )
    if not args.keep_work:
        retire_dir(work_root)
    return overlay_payload

def cmd_index(args: argparse.Namespace) -> int:
    base = Path(args.base).resolve()
    entries = build_index(base)
    write_index(entries, Path(args.out))
    chunks = len(index_chunks(entries))
    print(
        f"patch_pipeline: indexed {len(entries)} function(s) in "
        f"{chunks} chunk(s) -> {args.out}"
    )
    return 0

def cmd_apply(args: argparse.Namespace) -> int:
    overlay = apply_pipeline(args)
    print(
        "patch_pipeline: "
        f"{overlay['totals']['stages']} stage(s), "
        f"{overlay['totals']['chunks']} chunk(s), "
        f"{overlay['totals']['targets']} target(s), "
        f"{overlay['totals']['sites']} site(s), "
        f"patchset {overlay['patchset_sha256'][:12]}"
    )
    if overlay["expected_matches"]:
        print(
            f"patch_pipeline: {overlay['expected_matches']} chunk(s) "
            "match the expected stage"
        )
    return 0

def _load_for_introspection(args: argparse.Namespace) -> list[Patch]:
    if args.manifest:
        return load_manifest_files(
            [Path(path).resolve() for path in args.manifest]
        )
    registry_path = Path(args.registry or DEFAULT_REGISTRY).resolve()
    patches, _, _ = load_registry(registry_path)
    return patches

def cmd_list(args: argparse.Namespace) -> int:
    patches = _load_for_introspection(args)
    for patch in patches:
        state = "enabled" if patch.enabled else "disabled"
        print(
            f"{patch.order:4d} {patch.id:24s} {patch.kind:9s} "
            f"{patch.mode:9s} {patch.scope:8s} {state:8s} "
            f"{patch.engine}"
        )
    registry_path = Path(args.registry or DEFAULT_REGISTRY).resolve()
    if not args.manifest and registry_path.is_file():
        data, _ = _load_json(registry_path)
        for name, ids in data.get("pipelines", {}).items():
            print(f"  pipeline: {name} ({len(ids)} stage(s))")
        for item in data.get("reserved", []):
            if isinstance(item, dict):
                print(
                    f"  reserved: {item.get('id', '?')} "
                    f"({item.get('reason', '')})"
                )
            else:
                print(f"  reserved: {item}")
    return 0

def cmd_graph(args: argparse.Namespace) -> int:
    patches = _load_for_introspection(args)
    by_id = {patch.id: patch for patch in patches}
    print("digraph patches {")
    for patch in patches:
        print(
            f'  "{patch.id}" [label="{patch.id}\\n'
            f'{patch.kind}/{patch.mode}/{patch.scope}"];'
        )
        for dep in patch.after:
            if dep in by_id:
                print(f'  "{dep}" -> "{patch.id}";')
    print("}")
    return 0

def cmd_check(args: argparse.Namespace) -> int:
    if args.manifest:
        patches = load_manifest_files(
            [Path(path).resolve() for path in args.manifest]
        )
    else:
        registry_path = Path(args.registry or DEFAULT_REGISTRY).resolve()
        patches, _, registry_data = load_registry(registry_path)
    if args.pipeline:
        if args.manifest:
            raise PipelineError("--pipeline needs a registry, not --manifest")
        if args.all or args.patch:
            raise PipelineError(
                "use either --pipeline or --patch/--all, not both"
            )
        args.patch = pipeline_patch_ids(registry_data, args.pipeline)
    selected, external = select_patches(
        patches, args.patch, args.all, args.include_disabled
    )
    params = parse_params(args.param)
    index = read_index(Path(args.index).resolve())
    plan, all_chunks = resolve_plan(selected, index, params)
    print(
        f"patch_pipeline: check ok: {len(selected)} stage(s), "
        f"{len(all_chunks)} chunk(s), {len(external)} external dep(s)"
    )
    for stage in plan:
        print(
            f"  {stage.patch.id}: {stage.patch.mode}/"
            f"{stage.patch.scope}, chunks={len(stage.chunks)}, "
            f"sites={stage.scope.sites}"
        )
    return 0

def cmd_self_test(_args: argparse.Namespace) -> int:
    from patch_pipeline_selftest import run

    return run()

def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--registry")
    parser.add_argument("--manifest", action="append", default=[])
    parser.add_argument("--patch", action="append", default=[])
    parser.add_argument("--pipeline")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--include-disabled", action="store_true")
    parser.add_argument("--param", action="append", default=[])

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="patch_pipeline.py",
        description="General build-time patch pipeline for the lifted corpus.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    index = sub.add_parser("index", help="scan chunks and write a function index")
    index.add_argument("--base", required=True)
    index.add_argument("--out", required=True)
    index.set_defaults(func=cmd_index)

    apply = sub.add_parser("apply", help="apply the selected stages")
    apply.add_argument("--base", required=True)
    apply.add_argument("--index", required=True)
    _add_common(apply)
    apply.add_argument(
        "--engine-dir", default=str(ROOT / "build" / "lift-tools")
    )
    apply.add_argument("--out", required=True)
    apply.add_argument("--force", action="store_true")
    apply.add_argument("--reuse", action="store_true")
    apply.add_argument("--keep-work", action="store_true")
    apply.add_argument("--allow-idempotent", action="store_true")
    apply.add_argument("--expected-dir")
    apply.add_argument("--flat", action="store_true")
    apply.add_argument("--quiet", action="store_true")
    apply.set_defaults(func=cmd_apply)

    check = sub.add_parser("check", help="resolve stages without running them")
    check.add_argument("--index", required=True)
    _add_common(check)
    check.set_defaults(func=cmd_check)

    listing = sub.add_parser("list", help="list registry stages")
    listing.add_argument("--registry")
    listing.add_argument("--manifest", action="append", default=[])
    listing.set_defaults(func=cmd_list)

    graph = sub.add_parser("graph", help="print the stage dependency graph")
    graph.add_argument("--registry")
    graph.add_argument("--manifest", action="append", default=[])
    graph.set_defaults(func=cmd_graph)

    test = sub.add_parser("self-test", help="run the synthetic pipeline test")
    test.set_defaults(func=cmd_self_test)
    return parser

def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except PipelineError as exc:
        print(f"patch_pipeline: error: {exc}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
