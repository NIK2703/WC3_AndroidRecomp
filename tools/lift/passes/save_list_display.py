#!/usr/bin/env python3
"""Display-only fix for the saved-game list: drop the `.w3z` extension.

The load/save dialog builds the real slot path at ``[ebp-0x21c]`` and keeps it
with the extension.  The item text is a separate buffer at ``[ebp-0x118]``;
all list-item branches (save file, directory, replay, early exit) converge on
``L_6F2A953D`` before the text is copied into the list control.  The extension
is visible there because the save reader returns the slot file name as the
description (``gdh.w3z``, ``12.w3zzz``).  The original game's list shows the
user-visible name without the extension, so this pass removes the first
literal ``.w3z`` (case-insensitive) from the display buffer only.

This is a LiftPass, not a hand edit of generated C.  It changes one guest
function (VA 0x6F2A953D) and publishes the function-level before/after hashes
in the JSON change set.  G-15 is not available yet; the pass is applied
explicitly and the risk is recorded in the report (ADR-028/ADR-029).

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
PASS_REV = 1
TARGET_VA = "0x6F2A953D"
TARGET_FUNC = "L_6F2A953D"
MARKER = "WC3_SAVE_LIST_STRIP_W3Z"

FUNC_RE = re.compile(
    r"(?m)^void " + re.escape(TARGET_FUNC) + r"\(CPU \*c\)\r?\n\{\r?\n"
)

# The block runs at the start of the common tail.  It uses the generated C
# guest-memory accessors, so it stays inside the frozen guest ABI: no CPU
# fields are added and no signature changes.
BLOCK = """\
    /* WC3_SAVE_LIST_STRIP_W3Z: display-only, keeps [ebp-0x21c] intact.
     * VA 0x6F2A953D, pass save_list_display rev 1. */
    {
        uint32_t _wc3_p = c->ebp + 0xFFFFFEE8u; /* [ebp - 0x118] */
        uint32_t _wc3_i = 0;
        while (rd8(_wc3_p + _wc3_i) != 0) {
            if (rd8(_wc3_p + _wc3_i) == '.' &&
                (rd8(_wc3_p + _wc3_i + 1) | 0x20) == 'w' &&
                rd8(_wc3_p + _wc3_i + 2) == '3' &&
                (rd8(_wc3_p + _wc3_i + 3) | 0x20) == 'z') {
                uint32_t _wc3_j = _wc3_i;
                do {
                    wr8(_wc3_p + _wc3_j, rd8(_wc3_p + _wc3_j + 4));
                    _wc3_j++;
                } while (rd8(_wc3_p + _wc3_j - 1) != 0);
                break;
            }
            _wc3_i++;
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


def function_span(text: str) -> tuple[int, int]:
    """Return the span of the target body (inside the outer braces)."""
    m = FUNC_RE.search(text)
    if not m:
        raise PassError(f"{TARGET_FUNC} (VA {TARGET_VA}) not found")
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
    raise PassError(f"unterminated body for {TARGET_FUNC}")


def transform(text: str) -> tuple[str, bool]:
    if MARKER in text:
        return text, False
    start, end = function_span(text)
    body = text[start:end]
    patched = BLOCK + body
    return text[:start] + patched + text[end:], True


def process_file(src: Path, dst: Path, json_path: Path | None) -> int:
    # newline="" keeps the generated C byte-for-byte outside the inserted
    # block; the default Windows text mode would rewrite every LF as CRLF.
    with open(src, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        text = f.read()
    m = FUNC_RE.search(text)
    if not m:
        raise PassError(f"{TARGET_FUNC} not found in {src}")
    before_start, before_end = function_span(text)
    before = text[before_start:before_end]
    out, changed = transform(text)
    after_start, after_end = function_span(out)
    after = out[after_start:after_end]
    with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(out)

    report = {
        "pass": PASS,
        "pass_rev": PASS_REV,
        "files": [
            {
                "path": str(src),
                "functions": [
                    {
                        "va": TARGET_VA,
                        "function": TARGET_FUNC,
                        "before_sha256": hashlib.sha256(normalize(before)).hexdigest(),
                        "after_sha256": hashlib.sha256(normalize(after)).hexdigest(),
                        "reason": "strip the .w3z slot extension from the save-list display text",
                    }
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
    )
    if not changed:
        print(
            f"{PASS}: {TARGET_VA} unchanged; existing marker kept",
            file=sys.stderr,
        )
    return 0


SELF_TEST_FIXTURE = """\
/* header */
void L_6F2A953D(CPU *c)
{
    c->ebx = (uint32_t)(rd32((c->ebx + 0x000001ECu)));
    dispatch_jmp(c, 0x6F2A9550u); return;
}
"""


def self_test() -> int:
    out, changed = transform(SELF_TEST_FIXTURE)
    if not changed or MARKER not in out:
        print("save_list_display self-test: block not inserted", file=sys.stderr)
        return 1
    again, changed_again = transform(out)
    if changed_again or again != out:
        print("save_list_display self-test: not idempotent", file=sys.stderr)
        return 1
    if "0xFFFFFEE8u" not in out or "rd8(" not in out or "wr8(" not in out:
        print("save_list_display self-test: malformed block", file=sys.stderr)
        return 1
    if len(normalize(out)) == len(normalize(SELF_TEST_FIXTURE)):
        print("save_list_display self-test: normalizer did not see a change", file=sys.stderr)
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
        return process_file(Path(args.lift), Path(args.out), Path(args.json) if args.json else None)
    except PassError as exc:
        print(f"save_list_display: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
