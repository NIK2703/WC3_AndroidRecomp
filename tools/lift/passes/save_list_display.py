#!/usr/bin/env python3
"""Display-only fix for the saved-game list: use the slot basename.

The PC load list shows the save file's basename without the ``.w3z``
extension (for example ``0``, ``2njeeije`` or ``Быстрое сохранение``).  The
Android save description in ``[ebp-0x118]`` is not the same string: it comes
from the save object and additionally carries the player name and the
localized type label, so stripping ``.w3z`` still leaves a different name.

The load/save dialog builds the real slot path at ``[ebp-0x21c]`` and keeps it
with the extension.  The item text is a separate buffer at ``[ebp-0x118]``.
The save-file branch ``L_6F2A950C`` copies the description there and then
falls through to the code at VA ``0x6F2A953D`` inside its own lifted body;
other branches jump to the separate ``L_6F2A953D`` function.  At the shared
tail, rebuild the display buffer from the basename of the real path, dropping
the ``.w3z`` extension.  The real path itself stays untouched.

The pass also replaces an older revision of its own block, so re-running it
after a block change is a real update rather than a silent no-op.  It is
idempotent when the current block is already present.  The block uses the
generated C guest-memory accessors, so it stays inside the frozen guest ABI:
no CPU fields are added and no signatures change.

G-15 is not available yet; the pass is applied explicitly and the risk is
recorded in the report (ADR-028/ADR-029).

Usage:
    python tools/lift/passes/save_list_display.py --self-test
    python tools/lift/passes/save_list_display.py \
        --lift build/corpus-full/game/closed.c \
        --out  build/corpus-full/game/closed.save-list.c \
        --json reports/generated/save_list_display.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

PASS = "save_list_display"
PASS_REV = 5
TARGET_VA = "0x6F2A953D"
MARKER = "WC3_SAVE_LIST_STRIP_W3Z"

# Function headers in the generated corpus.  The body starts after the
# opening brace and its trailing newline.
FUNC_START_RE = re.compile(r"(?m)^void (L_[0-9A-F]+)\(CPU \*c\)\r?\n\{\r?\n")

# Every occurrence of the instruction at 0x6F2A953D, including the copy that
# the lifter inlines into a caller when the call returns to that address.
SITE_RE = re.compile(r"(?m)^[^\n]*/\* 6F2A953D:")

# The PC list uses the file basename, while the Android description can carry
# the slot path, the player name and the type label.  Rebuild the display text
# from the real path's basename at [ebp-0x21c] and drop the ".w3z" extension;
# the real path itself stays untouched.
BLOCK = """\
    /* WC3_SAVE_LIST_STRIP_W3Z: display-only basename, keeps [ebp-0x21c] intact.
     * VA 0x6F2A953D, pass save_list_display rev 5. */
    {
        uint32_t _wc3_path = c->ebp + 0xFFFFFDE4u; /* [ebp - 0x21c] */
        uint32_t _wc3_dst = c->ebp + 0xFFFFFEE8u;  /* [ebp - 0x118] */
        uint32_t _wc3_len = 0;
        uint32_t _wc3_base = 0;
        uint32_t _wc3_i;
        while (rd8(_wc3_path + _wc3_len) != 0) {
            uint8_t _wc3_ch = rd8(_wc3_path + _wc3_len);
            if (_wc3_ch == 92u || _wc3_ch == 47u) { /* backslash or slash */
                _wc3_base = _wc3_len + 1;
            }
            _wc3_len++;
        }
        if (_wc3_len >= _wc3_base + 4 &&
            rd8(_wc3_path + _wc3_len - 4) == '.' &&
            (rd8(_wc3_path + _wc3_len - 3) | 0x20) == 'w' &&
            rd8(_wc3_path + _wc3_len - 2) == '3' &&
            (rd8(_wc3_path + _wc3_len - 1) | 0x20) == 'z') {
            uint32_t _wc3_n = _wc3_len - _wc3_base - 4;
            if (_wc3_n > 0x103u) {
                _wc3_n = 0x103u;
            }
            for (_wc3_i = 0; _wc3_i < _wc3_n; _wc3_i++) {
                wr8(_wc3_dst + _wc3_i, rd8(_wc3_path + _wc3_base + _wc3_i));
            }
            wr8(_wc3_dst + _wc3_n, 0);
        }
    }
"""


class PassError(RuntimeError):
    pass


def normalize(body: str) -> bytes:
    """Function-level comparison form: strip comments and collapse whitespace."""
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    return re.sub(r"\s+", " ", body).strip().encode("utf-8")


def function_span(text: str, name: str) -> tuple[int, int]:
    """Return the span of the body (inside the outer braces) for ``name``."""
    pattern = re.compile(
        r"(?m)^void " + re.escape(name) + r"\(CPU \*c\)\r?\n\{\r?\n"
    )
    m = pattern.search(text)
    if not m:
        raise PassError(f"{name} not found")
    start = m.end()
    depth = 1
    i = start
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return start, i
        i += 1
    raise PassError(f"unterminated body for {name}")


def function_hash(text: str, name: str) -> str:
    start, end = function_span(text, name)
    return hashlib.sha256(normalize(text[start:end])).hexdigest()


def find_sites(text: str) -> list[tuple[int, str, bool, int]]:
    """Return (line_start, function, already_marked, function_start)."""
    funcs = list(FUNC_START_RE.finditer(text))
    if not funcs:
        raise PassError("no lifted functions found")
    sites: list[tuple[int, str, bool, int]] = []
    for m in SITE_RE.finditer(text):
        func = None
        for f in reversed(funcs):
            if f.start() < m.start():
                func = f
                break
        if func is None:
            raise PassError(f"site {TARGET_VA} has no containing function")
        name = func.group(1)
        prefix = text[func.start() : m.start()]
        sites.append((m.start(), name, MARKER in prefix, func.start()))
    if not sites:
        raise PassError(f"{TARGET_VA} not found")
    return sites


def _old_block_start(text: str, site_pos: int, func_start: int) -> int | None:
    """Start of our block immediately before ``site_pos``, if it is still ours."""
    marker_pos = text.rfind(MARKER, func_start, site_pos)
    if marker_pos < 0:
        return None
    block_start = text.rfind("\n", 0, marker_pos) + 1
    old = text[block_start:site_pos]
    # The inserted block always ends with the closing brace and a newline.
    if not old.rstrip().endswith("}"):
        return None
    return block_start


def transform(text: str) -> tuple[str, bool]:
    sites = find_sites(text)
    edits: list[tuple[int, int, str]] = []
    for pos, _name, marked, func_start in sites:
        if marked:
            block_start = _old_block_start(text, pos, func_start)
            if block_start is not None and text[block_start:pos] != BLOCK:
                edits.append((block_start, pos, BLOCK))
        else:
            line_start = text.rfind("\n", 0, pos) + 1
            edits.append((line_start, line_start, BLOCK))
    if not edits:
        return text, False
    # Apply from the end so earlier offsets stay valid.
    for start, end, replacement in sorted(edits, key=lambda e: e[0], reverse=True):
        text = text[:start] + replacement + text[end:]
    return text, True


def process_file(src: Path, dst: Path, json_path: Path | None) -> int:
    # newline="" keeps the generated C byte-for-byte outside the inserted
    # block; the default Windows text mode would rewrite every LF as CRLF.
    with open(src, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        text = f.read()
    sites = find_sites(text)
    names: list[str] = []
    for _pos, name, _marked, _func_start in sites:
        if name not in names:
            names.append(name)
    before_hashes = {name: function_hash(text, name) for name in names}
    out, changed = transform(text)
    after_hashes = {name: function_hash(out, name) for name in names}
    with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(out)

    report = {
        "pass": PASS,
        "pass_rev": PASS_REV,
        "files": [
            {
                "path": str(src),
                "sites": len(sites),
                "functions": [
                    {
                        "va": TARGET_VA,
                        "function": name,
                        "before_sha256": before_hashes[name],
                        "after_sha256": after_hashes[name],
                        "reason": "use the slot basename from [ebp-0x21c] as the save-list display text (PC list parity)",
                    }
                    for name in names
                ],
            }
        ],
        "applied": changed,
        "g15": "unavailable",
        "risk": "ADR-028/ADR-029: LiftPass change without a G-15 semantic-equivalence report",
    }
    if json_path:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(
        f"{PASS}: {src} -> {dst}: "
        + ("applied" if changed else "already applied (idempotent)")
        + f" ({len(sites)} site(s), {len(names)} function(s))"
    )
    return 0


SELF_TEST_FIXTURE = """\
/* header */
void L_6F2A950C(CPU *c)
{
    push32(c, 0x6F2A953Du); dispatch(c, 0x6F07C6DEu);
    c->ebx = (uint32_t)(rd32((c->ebx + 0x000001ECu)));           /* 6F2A953D: mov ebx, dword ptr [ebx + 0x1ec] */
    flags_logicz(c, c->ebx & c->ebx, 4);                         /* 6F2A9543: test ebx, ebx */
    dispatch_jmp(c, 0x6F2A9550u); return;
}

void L_6F2A953D(CPU *c)
{
    c->ebx = (uint32_t)(rd32((c->ebx + 0x000001ECu)));           /* 6F2A953D: mov ebx, dword ptr [ebx + 0x1ec] */
    flags_logicz(c, c->ebx & c->ebx, 4);                         /* 6F2A9543: test ebx, ebx */
    dispatch_jmp(c, 0x6F2A9550u); return;
}
"""


def self_test() -> int:
    out, changed = transform(SELF_TEST_FIXTURE)
    if not changed or out.count(MARKER) != 2:
        print(
            "save_list_display self-test: block not inserted at both sites",
            file=sys.stderr,
        )
        return 1
    again, changed_again = transform(out)
    if changed_again or again != out:
        print("save_list_display self-test: not idempotent", file=sys.stderr)
        return 1
    if (
        "0xFFFFFDE4u" not in out
        or "0xFFFFFEE8u" not in out
        or "rd8(" not in out
        or "wr8(" not in out
    ):
        print("save_list_display self-test: malformed block", file=sys.stderr)
        return 1
    if len(normalize(out)) == len(normalize(SELF_TEST_FIXTURE)):
        print(
            "save_list_display self-test: normalizer did not see a change",
            file=sys.stderr,
        )
        return 1
    # An older revision of our own block must be replaced, not ignored.
    old = out.replace("rev 5", "rev 2", 1)
    if old == out:
        print("save_list_display self-test: replacement fixture is unchanged", file=sys.stderr)
        return 1
    updated, changed_again = transform(old)
    if not changed_again or updated != out:
        print("save_list_display self-test: old block was not replaced", file=sys.stderr)
        return 1
    print("save_list_display self-test: ok")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lift", help="generated C file containing the target function")
    ap.add_argument("--out", help="output file (must differ from --lift)")
    ap.add_argument("--json", help="write the function-level change set here")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.lift or not args.out:
        ap.error("--lift and --out are required unless --self-test is used")
    if Path(args.lift).resolve() == Path(args.out).resolve():
        ap.error("--out must differ from --lift; replace the corpus explicitly")
    try:
        return process_file(
            Path(args.lift), Path(args.out), Path(args.json) if args.json else None
        )
    except PassError as exc:
        print(f"save_list_display: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
