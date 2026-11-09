#!/usr/bin/env python3
"""Check documentation links, the docs index, stable ids and the ban list.

This is the first, mechanical half of R11.  It does not try to understand
prose; it catches the failures that made the old tree contradict itself:
a document that is not indexed, a relative link whose target moved, an
undefined or gapped PM-n/TA-n identifier, and a forbidden render/provider
path mentioned outside a forbidding sentence.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NORMATIVE = (
    ROOT / "AGENTS.md",
    ROOT / "README.md",
    ROOT / "docs" / "PLAN_REFACTOR_1.27B.md",
    ROOT / "docs" / "ARCHITECTURE_1.27B.md",
    ROOT / "docs" / "FILE_LAYOUT.md",
    ROOT / "docs" / "ADR.md",
    ROOT / "docs" / "AS_BUILT.md",
    ROOT / "docs" / "PERFORMANCE.md",
    ROOT / "docs" / "PERFORMANCE_CONTRACT.md",
    ROOT / "docs" / "RISKS.md",
    ROOT / "docs" / "BACKLOG.md",
    ROOT / "docs" / "VERIFICATION.md",
    ROOT / "docs" / "GUEST_ABI.md",
    ROOT / "docs" / "EXACTNESS.md",
    ROOT / "docs" / "GRAPHICS_DECISION.md",
    ROOT / "docs" / "HLE_MODULES.md",
    ROOT / "docs" / "BUILD_PIPELINE.md",
    ROOT / "docs" / "BUILD_VARIANTS.md",
    ROOT / "docs" / "THREAD_AUDIT.md",
    ROOT / "docs" / "DXVK_THREADS.md",
    ROOT / "docs" / "AUDIO_ARCHITECTURE.md",
)
POLICY_TERMS = ("native_vk", "hle_gl", "hle_d3d9_vk", "D9VkApi", "win32_d3d9")
ALLOW_CONTEXT = (
    "forbidden", "запрещ", "legacy", "history", "historical", "истори",
    "removed", "удал", "absent", "отсутств", "outside", "вне ", "не входит",
    "not ", "never", "никогда", "reference", "эталон", "не является",
    "as-built", "as built", "был ", "были ", "ранее", "старый", "старая",
)
LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
TICK_RE = re.compile(r"`([^`\n]+)`")
DOC_PATH_RE = re.compile(r"^(?:docs|tools|config|runtime|tests|apps|oracle|third_party)/")

# R11, second half: every stable PM-n / TA-n / Rn reference must resolve to
# a definition in its owner document, and the defined ids must be contiguous.
# An undefined id in a normative doc is a contract nobody owns; a gap means
# a revision quietly dropped a numbered requirement.  R ids are defined by
# table rows in FILE_LAYOUT.md, the other two by bold bullets.
ID_OWNERS = {
    "PM": ROOT / "docs" / "PERFORMANCE_CONTRACT.md",
    "TA": ROOT / "docs" / "THREAD_AUDIT.md",
    "R": ROOT / "docs" / "FILE_LAYOUT.md",
    "BV": ROOT / "docs" / "BUILD_VARIANTS.md",
}
ID_DEF_RE = {
    "PM": re.compile(r"\*\*PM-(\d+)\s*\("),
    "TA": re.compile(r"\*\*TA-(\d+)\s*\("),
    "R": re.compile(r"^\|\s*R(\d+)\s*\|", re.M),
    "BV": re.compile(r"^(?:\*\*|##\s*)BV-(\d+)\b", re.M),
}
ID_REF_RE = {
    "PM": re.compile(r"\bPM-(\d+)\b"),
    "TA": re.compile(r"\bTA-(\d+)\b"),
    "R": re.compile(r"\bR(\d+)\b"),
    "BV": re.compile(r"\bBV-(\d+)\b"),
}
ID_LABEL = {"PM": "PM-", "TA": "TA-", "R": "R", "BV": "BV-"}


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT.resolve()).as_posix()
    except ValueError:
        return str(path)


def strip_anchor(target: str) -> str:
    return target.split("#", 1)[0].strip()


def is_external(target: str) -> bool:
    return (target.startswith(("http://", "https://", "mailto:"))
            or target.startswith("#"))


def normalize_target(doc: Path, target: str) -> Path:
    target = strip_anchor(target)
    if target.startswith("/"):
        return ROOT / target.lstrip("/")
    return (doc.parent / target)


def check_index(errors: list[str], warnings: list[str]) -> None:
    index = ROOT / "docs" / "README.md"
    if not index.is_file():
        errors.append("docs/README.md is missing")
        return
    text = index.read_text(encoding="utf-8")
    indexed_docs: set[str] = set()
    indexed_root: set[str] = set()
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = line.strip().strip("|").split("|")
        if not cells:
            continue
        cell = cells[0].strip()
        match = re.fullmatch(r"`([^`]+)`", cell)
        if not match:
            continue
        value = match.group(1).strip()
        if value.startswith("docs/"):
            indexed_docs.add(Path(value).name)
        elif "/" not in value and value.endswith(".md"):
            if (ROOT / "docs" / value).exists():
                indexed_docs.add(value)
            elif (ROOT / value).exists():
                indexed_root.add(value)
    for path in sorted((ROOT / "docs").glob("*.md")):
        if path.name == "README.md":
            continue
        if path.name not in indexed_docs:
            errors.append(f"docs/{path.name} is not listed in docs/README.md")
    for name in sorted(indexed_docs):
        if name == "README.md":
            continue
        if not (ROOT / "docs" / name).exists():
            errors.append(f"docs/README.md indexes missing docs/{name}")
    for name in sorted(indexed_root):
        if not (ROOT / name).is_file():
            errors.append(f"docs/README.md indexes missing root file {name}")
    for name in ("AGENTS.md", "README.md"):
        if not (ROOT / name).is_file():
            errors.append(f"root document {name} is missing")
    if "ARCHITECTURE_1.27B.md" not in indexed_docs:
        errors.append("docs/README.md does not list the normative architecture")


def check_stable_ids(errors: list[str], warnings: list[str]) -> None:
    defined: dict[str, set[int]] = {prefix: set() for prefix in ID_OWNERS}
    for prefix, owner in ID_OWNERS.items():
        if not owner.is_file():
            errors.append(f"R11: stable-id owner {rel(owner)} is missing")
            continue
        text = owner.read_text(encoding="utf-8")
        for match in ID_DEF_RE[prefix].finditer(text):
            defined[prefix].add(int(match.group(1)))
        if not defined[prefix]:
            errors.append(f"R11: no {ID_LABEL[prefix]}n definitions in {rel(owner)}")
            continue
        expected = set(range(1, max(defined[prefix]) + 1))
        gaps = sorted(expected - defined[prefix])
        if gaps:
            errors.append(
                f"R11: {rel(owner)} has gaps in {ID_LABEL[prefix]}n: "
                + ", ".join(f"{ID_LABEL[prefix]}{n}" for n in gaps)
            )
    for doc in NORMATIVE:
        if not doc.is_file():
            continue
        text = doc.read_text(encoding="utf-8")
        for prefix, pattern in ID_REF_RE.items():
            for match in pattern.finditer(text):
                number = int(match.group(1))
                if number not in defined[prefix]:
                    errors.append(
                        f"R11: {rel(doc)} references undefined "
                        f"{ID_LABEL[prefix]}{number}"
                    )


def check_links(doc: Path, errors: list[str], warnings: list[str]) -> None:
    text = doc.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        for match in LINK_RE.finditer(line):
            target = match.group(1).strip()
            if not target or is_external(target):
                continue
            if "<" in target or ">" in target or "*" in target:
                continue
            candidate = normalize_target(doc, target)
            if not candidate.exists():
                errors.append(f"{rel(doc)}:{lineno}: broken link {target}")


def check_backtick_paths(doc: Path, warnings: list[str]) -> None:
    text = doc.read_text(encoding="utf-8")
    for match in TICK_RE.finditer(text):
        value = match.group(1).strip()
        if not DOC_PATH_RE.match(value):
            continue
        if any(ch in value for ch in ("*", "<", ">", "$", " ")):
            continue
        candidate = ROOT / value
        if not candidate.exists():
            warnings.append(f"{rel(doc)}: referenced path does not exist: {value}")


def check_policy_terms(errors: list[str]) -> None:
    for doc in NORMATIVE:
        if not doc.is_file():
            continue
        lines = doc.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            low = line.lower()
            window = "\n".join(lines[max(0, index - 2):index + 3]).lower()
            if not any(word in window for word in ALLOW_CONTEXT):
                for term in POLICY_TERMS + ("opengl",):
                    if term in low:
                        errors.append(
                            f"{rel(doc)}:{index + 1}: {term!r} is mentioned "
                            "without a forbidding/legacy context"
                        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true",
                        help="treat warnings as errors")
    parser.add_argument("--paths", action="store_true",
                        help="check every backticked repository path (noisy before M6)")
    parser.add_argument("--json", type=Path,
                        help="write a machine-readable report")
    args = parser.parse_args(argv)

    errors: list[str] = []
    warnings: list[str] = []
    check_index(errors, warnings)
    check_stable_ids(errors, warnings)
    for doc in sorted((ROOT / "docs").glob("*.md")):
        check_links(doc, errors, warnings)
        if args.paths:
            check_backtick_paths(doc, warnings)
    for doc in (ROOT / "README.md",):
        if doc.is_file():
            check_links(doc, errors, warnings)
            if args.paths:
                check_backtick_paths(doc, warnings)
    check_policy_terms(errors)

    if args.strict:
        errors.extend(warnings)
        warnings = []

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "schema": 1,
            "errors": errors,
            "warnings": warnings,
            "verdict": "FAIL" if errors else ("WARN" if warnings else "OK"),
        }, indent=2) + "\n", encoding="utf-8")

    for warning in warnings:
        print(f"docs: warning: {warning}", file=sys.stderr)
    for error in errors:
        print(f"docs: error: {error}", file=sys.stderr)
    if errors:
        print(f"docs: FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(f"docs: OK ({len(warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
