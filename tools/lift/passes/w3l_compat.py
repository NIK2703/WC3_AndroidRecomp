#!/usr/bin/env python3
"""Apply the optional W3L / PvPGN / rubattle compatibility patch to lifted C.

The patch reproduces only the effects that wl27.dll applies to Game.dll:
auth_req, logon-proof inversion, plaintext account creation, the two PvPGN
logon-proof hash call sites and the latency reducer.  The ``rubattle`` profile
also adds the 0x50 trailing dword and the packet-0x83 handler.

The generated corpus is changed by address comments, not line numbers.  Every
function is found by its ``L_<VA>`` name, every site by its exact instruction
comment, and a missing site is a hard error when the containing function is
present.  ``--require-all`` additionally requires every site of the profile to
exist in the input.

The pass is idempotent: a marker in the containing function means the site is
already in its patched form.  Each run publishes a function-level change set
with before/after body hashes.  G-15 is not available yet, so the report keeps
``g15: "unavailable"`` and the ADR-028/ADR-029 risk.

Usage:
    python tools/lift/passes/w3l_compat.py --self-test
    python tools/lift/passes/w3l_compat.py --profile rubattle --latency 100 \
        --lift build/corpus-full/game/closed.c \
        --out  build/compat/game/closed.c \
        --json build/compat/game/closed.json
    python tools/lift/passes/w3l_compat.py --profile pvpgn --check --require-all \
        --lift build/corpus-full/game/seed.c
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

PASS = "w3l_compat"
PASS_REV = 3
PROFILES = ("pvpgn", "rubattle")


@dataclass(frozen=True)
class Site:
    key: str
    function: str
    va: str
    anchor: str
    profiles: frozenset[str]
    marker: str
    reason: str


def _both() -> frozenset[str]:
    return frozenset(PROFILES)


def _rubattle() -> frozenset[str]:
    return frozenset(("rubattle",))


SITES: tuple[Site, ...] = (
    Site(
        "auth_req",
        "L_6F8DDC80",
        "0x6F8DDD94",
        "/* 6F8DDD94:",
        _both(),
        "WC3_W3L_COMPAT auth_req",
        "wl27 auth_req: NOP the jne to the auth failure path",
    ),
    Site(
        "logon_proof_hash_verify",
        "L_6F8DD71D",
        "0x6F8DD732",
        "/* 6F8DD732:",
        _both(),
        "WC3_W3L_COMPAT logon_proof",
        "wl27 logon_proof_hash_verify: invert je to jne",
    ),
    Site(
        "create_account",
        "L_6F9A7DB0",
        "0x6F9A7DFB",
        "/* 6F9A7DFB:",
        _both(),
        "WC3_W3L_COMPAT create_account",
        "wl27 create_account: copy 32 plaintext password bytes",
    ),
    Site(
        "hash_56",
        "L_6F8DD290",
        "0x6F8DD2A4",
        "/* 6F8DD2A4:",
        _both(),
        "WC3_W3L_COMPAT hash_56",
        "wl27 PvPGN logon-proof hash replacement, packet 0x56",
    ),
    Site(
        "hash_54",
        "L_6F8DD62D",
        "0x6F8DD63E",
        "/* 6F8DD63E:",
        _both(),
        "WC3_W3L_COMPAT hash_54",
        "wl27 PvPGN logon-proof hash replacement, packet 0x54",
    ),
    Site(
        "delay_reducer",
        "L_6F970FA0",
        "0x6F970FA0",
        "/* 6F970FA0:",
        _both(),
        "WC3_W3L_COMPAT delay_reducer",
        "wl27 delay_reducer: replace 0xFA with WC3_W3L_LATENCY",
    ),
    Site(
        "local_auth_308",
        "L_6F094308",
        "0x6F09439C",
        "/* 6F09439C:",
        _both(),
        "WC3_W3L_COMPAT local_auth",
        "Android HLE workaround: accept SFileAuthenticateArchive status 1..4 "
        "so an unsigned version archive does not stop the host before "
        "SID_AUTH_CHECK (not a wl27 effect)",
    ),
    Site(
        "local_auth_364",
        "L_6F094364",
        "0x6F09439C",
        "/* 6F09439C:",
        _both(),
        "WC3_W3L_COMPAT local_auth",
        "Android HLE workaround: same local-auth branch in the duplicate "
        "block reachable from four callers (not a wl27 effect)",
    ),
    Site(
        "rubattle_0x50",
        "L_6F8D3EB0",
        "0x6F8D3F22",
        "/* 6F8D3F22:",
        _rubattle(),
        "WC3_W3L_COMPAT rubattle_0x50",
        "rubattle SID_AUTH_INFO: append dword 3",
    ),
    Site(
        "rubattle_0x83",
        "L_6F8DC630",
        "0x6F8DC63C",
        "/* 6F8DC63C:",
        _rubattle(),
        "WC3_W3L_COMPAT rubattle_0x83",
        "rubattle packet 0x83 handler",
    ),
)

class PassError(RuntimeError):
    pass


def normalize(body: str) -> bytes:
    """Function-level comparison form: strip comments and collapse whitespace."""
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    return re.sub(r"\s+", " ", body).strip().encode("utf-8")


def function_span(text: str, name: str) -> tuple[int, int] | None:
    """Return the body span (inside the outer braces) or None if absent.

    The generated corpus is large (hundreds of MiB per module), so do not run
    an anchored regex over the whole text five to eight times.  ``str.find``
    locates the only definition lines; the anchored form is then checked
    explicitly.  A prototype or a mention in a comment does not match because
    it lacks the ``{`` line that follows the signature.
    """
    needle = f"void {name}(CPU *c)"
    positions: list[int] = []
    search = 0
    while True:
        pos = text.find(needle, search)
        if pos < 0:
            break
        line_start = text.rfind("\n", 0, pos) + 1
        if pos == line_start:
            tail = text[pos + len(needle):]
            if (tail.startswith("\r\n{\r\n") or tail.startswith("\n{\n")):
                positions.append(pos)
        search = pos + 1

    if not positions:
        return None
    if len(positions) != 1:
        raise PassError(f"{name} appears {len(positions)} times; expected one")

    brace = text.find("{", positions[0])
    if brace < 0:  # pragma: no cover - guarded by the tail check above
        raise PassError(f"{name} has no body brace")
    start = brace + 1
    if text.startswith("\r\n", start):
        start += 2
    elif text.startswith("\n", start):
        start += 1
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
    span = function_span(text, name)
    if span is None:
        raise PassError(f"{name} not found while hashing")
    start, end = span
    return hashlib.sha256(normalize(text[start:end])).hexdigest()


def _line_info(text: str, pos: int, end: int) -> tuple[int, int, int]:
    """Return (line start, content end without CR, newline end)."""
    line_start = text.rfind("\n", 0, pos) + 1
    newline_end = text.find("\n", pos, end)
    if newline_end < 0:
        newline_end = end
    content_end = newline_end
    if content_end > line_start and text[content_end - 1] == "\r":
        content_end -= 1
    return line_start, content_end, newline_end


def _anchor_pos(text: str, start: int, end: int, site: Site) -> int | None:
    pos = text.find(site.anchor, start, end)
    if pos < 0:
        return None
    if text.find(site.anchor, pos + 1, end) >= 0:
        raise PassError(
            f"{site.key}: anchor {site.va} appears more than once in "
            f"{site.function}"
        )
    return pos


def _line_at(text: str, pos: int, end: int) -> str:
    line_start, content_end, _newline_end = _line_info(text, pos, end)
    return text[line_start:content_end]


def _nl(text: str) -> str:
    return "\r\n" if "\r\n" in text else "\n"


def _with_nl(value: str, nl: str) -> str:
    return value.replace("\n", nl)


def _validate_applied(text: str, start: int, end: int, site: Site) -> None:
    """Reject a marker that hides a still-unpatched or half-patched site."""
    anchor = _anchor_pos(text, start, end, site)
    line = _line_at(text, anchor, end) if anchor is not None else ""

    if site.key == "auth_req":
        if "0x6F8DDE5Fu" in line and "if (!c->zf)" in line:
            raise PassError(
                "auth_req marker present but the failure branch is still active"
            )
    elif site.key == "logon_proof_hash_verify":
        if "if (c->zf)" in line:
            raise PassError(
                "logon_proof marker present but je was not inverted"
            )
    elif site.key == "create_account":
        if "c->ecx = (uint32_t)(rd32((c->ebp + 0x00000010u)))" in line:
            raise PassError(
                "create_account marker present but the original mov is still active"
            )
    elif site.key in ("hash_56", "hash_54"):
        if "dispatch(c, 0x6F8E6D50u)" in line:
            raise PassError(
                f"{site.key} marker present but the original hash call is still active"
            )
    elif site.key == "delay_reducer":
        if "0xFAu" in line:
            raise PassError(
                "delay_reducer marker present but 0xFA is still active"
            )
    elif site.key.startswith("local_auth"):
        if "dispatch_jmp(c, 0x6F094739u)" in line:
            raise PassError(
                f"{site.key} marker present but the local-auth failure "
                "branch is still active"
            )
    elif site.key == "rubattle_0x50":
        body = text[start:end]
        if "push32(c, 0x00000003u);" not in body:
            raise PassError(
                "rubattle_0x50 marker present but the append-dword call is missing"
            )
    elif site.key == "rubattle_0x83":
        if "wc3_w3l_handle_0x83(c)" not in text[start:end]:
            raise PassError(
                "rubattle_0x83 marker present but the native handler call is missing"
            )


def _replace_line(
    text: str, line_start: int, content_end: int, replacement: str
) -> tuple[int, int, str]:
    return line_start, content_end, replacement


def _apply_site(
    text: str, site: Site, latency: int
) -> tuple[str, str, tuple[int, int, str] | None]:
    """Return (possibly new text, status, edit).  status is applied/already/absent."""
    span = function_span(text, site.function)
    if span is None:
        return text, "absent", None
    start, end = span

    if site.marker in text[start:end]:
        _validate_applied(text, start, end, site)
        return text, "already", None

    pos = _anchor_pos(text, start, end, site)
    if pos is None:
        raise PassError(
            f"{site.key}: {site.va} not found in {site.function}; "
            "the corpus changed or the wrong input was passed"
        )
    line_start, content_end, _newline_end = _line_info(text, pos, end)
    line = text[line_start:content_end]
    nl = _nl(text)

    if site.key == "auth_req":
        replacement = (
            "    /* WC3_W3L_COMPAT auth_req: VA 0x6F8DDD94 branch NOP "
            "(profile pvpgn/rubattle). */"
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key == "logon_proof_hash_verify":
        if "if (c->zf)" not in line:
            raise PassError(
                "logon_proof: expected 'if (c->zf)' at 0x6F8DD732"
            )
        inverted = line.replace("if (c->zf)", "if (!c->zf)", 1)
        replacement = (
            "    /* WC3_W3L_COMPAT logon_proof: VA 0x6F8DD732 je -> jne. */"
            + nl
            + inverted
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key == "create_account":
        replacement = _with_nl(
            """\
    /* WC3_W3L_COMPAT create_account: VA 0x6F9A7DFB.
     * wl27 0x10001970 copies 32 plaintext password bytes from the saved ESI
     * ([ebp-4], the caller's password string) to the account field pointed to
     * by [ebp+0x14], then reloads ecx from [ebp+0x10]. */
    {
        uint32_t _wc3_src = rd32(c->ebp - 0x00000004u);
        uint32_t _wc3_dst = rd32(c->ebp + 0x00000014u);
        uint32_t _wc3_i;
        for (_wc3_i = 0; _wc3_i < 0x20u; _wc3_i++) {
            wr8(_wc3_dst + _wc3_i, rd8(_wc3_src + _wc3_i));
        }
        c->ecx = rd32(c->ebp + 0x00000010u);
    }""",
            nl,
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key in ("hash_56", "hash_54"):
        packet = "0x56" if site.key == "hash_56" else "0x54"
        replacement = _with_nl(
            f"""\
    c->eax = (uint32_t)wc3_w3l_pvpgn_hash(c, c->ecx, c->edx); /* WC3_W3L_COMPAT {site.key}: VA {site.va}, packet {packet}. */
    c->esp += 8u; /* wl27 wrapper ret 8; the two ignored arguments are cleaned. */""",
            nl,
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key == "delay_reducer":
        replacement = (
            "    c->eax = (uint32_t)(WC3_W3L_LATENCY);"
            " /* WC3_W3L_COMPAT delay_reducer: VA 0x6F970FA0. */"
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key.startswith("local_auth"):
        replacement = (
            "    /* WC3_W3L_COMPAT local_auth: VA 0x6F09439C, accept "
            "SFileAuthenticateArchive status 1..4.  Android HLE workaround; "
            "wl27 has no such patch. */"
        )
        edit = _replace_line(text, line_start, content_end, replacement)

    elif site.key == "rubattle_0x50":
        replacement = _with_nl(
            """\
    /* WC3_W3L_COMPAT rubattle_0x50: VA 0x6F8D3F22, append dword 3 to SID_AUTH_INFO. */
    push32(c, 0x00000003u);
    c->ecx = (uint32_t)(c->edi);
    push32(c, 0x6F8D3F22u); dispatch(c, 0x6F06B3A0u);""",
            nl,
        )
        # Insert before the original mov eax,edi; the original instruction stays.
        edit = (line_start, line_start, replacement + nl)

    elif site.key == "rubattle_0x83":
        replacement = _with_nl(
            """\
    /* WC3_W3L_COMPAT rubattle_0x83: VA 0x6F8DC63C, native handler. */
    if (c->eax == 0x00000083u) {
        wc3_w3l_handle_0x83(c);
        return;
    }""",
            nl,
        )
        edit = (line_start, line_start, replacement + nl)

    else:  # pragma: no cover - SITES is the closed list above
        raise PassError(f"unknown site {site.key}")

    return text, "applied", edit


def transform(
    text: str, profile: str, latency: int, require_all: bool = False
) -> tuple[str, list[dict[str, str]]]:
    if profile not in PROFILES:
        raise PassError(f"unknown profile {profile!r}")
    if not 30 <= latency <= 255:
        raise PassError("latency must be in the wl27 range 30..255")

    sites = [site for site in SITES if profile in site.profiles]
    results: list[dict[str, str]] = []
    edits: list[tuple[int, int, str]] = []

    for site in sites:
        current, status, edit = _apply_site(text, site, latency)
        # _apply_site only returns edits against the original text; all target
        # functions are disjoint in the generated corpus.
        if status == "absent":
            if require_all:
                raise PassError(
                    f"{site.key}: required function {site.function} not found"
                )
        if edit is not None:
            edits.append(edit)
        results.append(
            {
                "key": site.key,
                "va": site.va,
                "function": site.function,
                "reason": site.reason,
                "status": status,
            }
        )
        if current is not text:
            raise PassError(
                "internal error: _apply_site returned new text instead of an edit"
            )

    # Apply from the end so earlier offsets stay valid.  Insertions and line
    # replacements never overlap for the current site table.
    for start, end, replacement in sorted(
        edits, key=lambda item: (item[0], item[1]), reverse=True
    ):
        text = text[:start] + replacement + text[end:]

    return text, results


def _function_changes(
    before: str, after: str, results: list[dict[str, str]]
) -> list[dict[str, object]]:
    by_function: dict[str, list[dict[str, str]]] = {}
    for result in results:
        if result["status"] != "absent":
            by_function.setdefault(result["function"], []).append(result)

    changes: list[dict[str, object]] = []
    for function, function_sites in by_function.items():
        changes.append(
            {
                "va": function_sites[0]["va"],
                "function": function,
                "before_sha256": function_hash(before, function),
                "after_sha256": function_hash(after, function),
                "sites": function_sites,
                "reason": "; ".join(site["reason"] for site in function_sites),
            }
        )
    return changes


def process_file(
    src: Path,
    dst: Path | None,
    json_path: Path | None,
    profile: str,
    latency: int,
    require_all: bool,
    check: bool,
    quiet: bool,
) -> int:
    # newline="" preserves the generated C byte-for-byte outside the edited
    # sites; the default Windows text mode would rewrite every LF as CRLF.
    with open(src, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        text = f.read()

    out, results = transform(text, profile, latency, require_all=require_all)
    changed = out != text
    if not check:
        if dst is None:
            raise PassError("--out is required unless --check is used")
        # Do not touch an unchanged output: CMake uses its mtime to decide
        # whether the downstream split/compile stages must run again.
        if changed or not dst.exists():
            with open(dst, "w", encoding="utf-8", errors="surrogateescape",
                      newline="") as f:
                f.write(out)

    missing = [result for result in results if result["status"] == "absent"]
    report = {
        "pass": PASS,
        "pass_rev": PASS_REV,
        "profile": profile,
        "latency": latency,
        "files": [
            {
                "path": str(src),
                "output": None if check else str(dst),
                "applied": changed,
                "sites": results,
                "functions": _function_changes(text, out, results),
            }
        ],
        "applied": changed,
        "missing": missing,
        "g15": "unavailable",
        "risk": "ADR-028/ADR-029: LiftPass change without a G-15 semantic-equivalence report",
    }
    if json_path is not None:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    if not quiet:
        verb = "check" if check else "apply"
        state = "applied" if changed else "already applied (idempotent)"
        print(
            f"{PASS}: {verb} {src} ({profile}, {len(results)} site(s), "
            f"{len(report['files'][0]['functions'])} function(s)): {state}"
        )
    return 0


def _iter_report_sites(label: str, report: object):
    if not isinstance(report, dict):
        raise PassError(f"{label}: change set is not a JSON object")
    if report.get("pass") != PASS:
        raise PassError(f"{label}: not a {PASS} change set")
    files = report.get("files")
    if not isinstance(files, list) or not files:
        raise PassError(f"{label}: change set has no files")
    for entry in files:
        if not isinstance(entry, dict):
            raise PassError(f"{label}: malformed file entry")
        sites = entry.get("sites")
        if not isinstance(sites, list):
            raise PassError(f"{label}: malformed site list")
        for site in sites:
            if not isinstance(site, dict) or "key" not in site or "status" not in site:
                raise PassError(f"{label}: malformed site entry")
            yield site


def verify_coverage_reports(
    reports: list[tuple[str, object]], profile: str
) -> list[str]:
    """Return missing/duplicate/unexpected site keys across change sets."""
    if profile not in PROFILES:
        raise PassError(f"unknown profile {profile!r}")
    expected = {site.key for site in SITES if profile in site.profiles}
    counts: dict[str, int] = {}
    for label, report in reports:
        if isinstance(report, dict) and report.get("profile") != profile:
            raise PassError(
                f"{label}: profile {report.get('profile')!r}, expected {profile!r}"
            )
        for site in _iter_report_sites(label, report):
            key = site["key"]
            status = site["status"]
            if status not in ("applied", "already", "absent"):
                raise PassError(f"{label}: unknown status {status!r} for {key!r}")
            if status != "absent":
                counts[key] = counts.get(key, 0) + 1

    problems: list[str] = []
    missing = sorted(expected - set(counts))
    duplicates = sorted(key for key, count in counts.items() if count > 1)
    unexpected = sorted(set(counts) - expected)
    if missing:
        problems.append("site(s) not found in any change set: " + ", ".join(missing))
    if duplicates:
        problems.append("site(s) found more than once: " + ", ".join(duplicates))
    if unexpected:
        problems.append(
            "site(s) outside the profile: " + ", ".join(unexpected)
        )
    return problems


def verify_coverage(paths: list[Path], profile: str) -> None:
    reports = []
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as f:
                reports.append((str(path), json.load(f)))
        except OSError as exc:
            raise PassError(f"cannot read change set {path}: {exc}") from exc
        except ValueError as exc:
            raise PassError(f"invalid change set {path}: {exc}") from exc
    problems = verify_coverage_reports(reports, profile)
    if problems:
        raise PassError("coverage: " + "; ".join(problems))


SELF_TEST_FIXTURE = """\
/* self-test */
void L_6F8DDC80(CPU *c)
{
    if (!c->zf) { dispatch_jmp(c, 0x6F8DDE5Fu); return; }        /* 6F8DDD94: jne 0x6f8dde5f */
    c->ecx += 4;
}
void L_6F8DD71D(CPU *c)
{
    if (c->zf) { dispatch_jmp(c, 0x6F8DD754u); return; }         /* 6F8DD732: je 0x6f8dd754 */
}
void L_6F9A7DB0(CPU *c)
{
    c->ecx = (uint32_t)(rd32((c->ebp + 0x00000010u)));           /* 6F9A7DFB: mov ecx, dword ptr [ebp + 0x10] */
}
void L_6F8DD290(CPU *c)
{
    push32(c, 0x6F8DD2A9u); dispatch(c, 0x6F8E6D50u);            /* 6F8DD2A4: call 0x6f8e6d50 */
}
void L_6F8DD62D(CPU *c)
{
    push32(c, 0x6F8DD643u); dispatch(c, 0x6F8E6D50u);            /* 6F8DD63E: call 0x6f8e6d50 */
}
void L_6F970FA0(CPU *c)
{
    c->eax = (uint32_t)(0xFAu);                                  /* 6F970FA0: mov eax, 0xfa */
}
void L_6F8D3EB0(CPU *c)
{
    c->eax = (uint32_t)(c->edi);                                 /* 6F8D3F22: mov eax, edi */
}
void L_6F8DC630(CPU *c)
{
    flags_sub(c, c->eax, 0x82u, 4);                              /* 6F8DC63C: cmp eax, 0x82 */
}
void L_6F094308(CPU *c)
{
    if (c->cf) { dispatch_jmp(c, 0x6F094739u); return; }         /* 6F09439C: jb 0x6f094739 */
}
void L_6F094364(CPU *c)
{
    if (c->cf) { dispatch_jmp(c, 0x6F094739u); return; }         /* 6F09439C: jb 0x6f094739 */
}
"""


def _count(text: str, needle: str) -> int:
    return text.count(needle)


def self_test() -> int:
    try:
        rubattle, results = transform(SELF_TEST_FIXTURE, "rubattle", 100)
        again, _ = transform(rubattle, "rubattle", 100)
        pvpgn, _ = transform(SELF_TEST_FIXTURE, "pvpgn", 100)
        try:
            transform(
                SELF_TEST_FIXTURE.replace("/* 6F8DDD94:", "/* 6F8DDD95:"),
                "rubattle",
                100,
            )
        except PassError:
            pass
        else:
            print("w3l_compat self-test: missing site did not fail", file=sys.stderr)
            return 1
        try:
            transform(
                SELF_TEST_FIXTURE.replace(
                    "void L_6F8DDC80(CPU *c)", "void L_NOT_PRESENT(CPU *c)"
                ),
                "pvpgn",
                100,
                require_all=True,
            )
        except PassError:
            pass
        else:
            print(
                "w3l_compat self-test: --require-all did not fail",
                file=sys.stderr,
            )
            return 1
    except PassError as exc:
        print(f"w3l_compat self-test: {exc}", file=sys.stderr)
        return 1

    problems: list[str] = []
    if len(results) != 10:
        problems.append(f"rubattle transformed {len(results)} sites, expected 10")
    if _count(rubattle, "WC3_W3L_COMPAT") != 10:
        problems.append("rubattle marker count is not 10")
    if _count(rubattle, "dispatch_jmp(c, 0x6F094739u)") != 0:
        problems.append("local_auth failure branch is still active")
    if _count(rubattle, "wc3_w3l_pvpgn_hash") != 2:
        problems.append("hash helper call count is not 2")
    if _count(rubattle, "wc3_w3l_handle_0x83") != 1:
        problems.append("0x83 handler call missing")
    if _count(rubattle, "0x00000003u") != 1:
        problems.append("0x50 append-dword call missing")
    if "WC3_W3L_LATENCY" not in rubattle:
        problems.append("delay reducer did not use WC3_W3L_LATENCY")
    if "if (!c->zf) { dispatch_jmp(c, 0x6F8DD754u)" not in rubattle:
        problems.append("logon_proof je was not inverted")
    if "if (!c->zf) { dispatch_jmp(c, 0x6F8DDE5Fu)" in rubattle:
        problems.append("auth_req branch is still active")
    if not idempotent(again, rubattle):
        problems.append("pass is not idempotent")
    if _count(pvpgn, "WC3_W3L_COMPAT") != 8:
        problems.append("pvpgn marker count is not 8")
    if _count(pvpgn, "dispatch_jmp(c, 0x6F094739u)") != 0:
        problems.append("pvpgn local_auth failure branch is still active")
    if "wc3_w3l_handle_0x83" in pvpgn:
        problems.append("pvpgn profile applied the rubattle 0x83 handler")
    if "0x00000003u" in pvpgn:
        problems.append("pvpgn profile applied the rubattle 0x50 tail")
    if len(normalize(rubattle)) == len(normalize(SELF_TEST_FIXTURE)):
        problems.append("normalizer did not see a function change")

    coverage_report = {
        "pass": PASS,
        "profile": "rubattle",
        "files": [{"sites": results}],
    }
    if verify_coverage_reports([("self", coverage_report)], "rubattle"):
        problems.append("coverage rejected a complete change set")
    short_report = {
        "pass": PASS,
        "profile": "rubattle",
        "files": [{"sites": results[:-1]}],
    }
    if not verify_coverage_reports([("self", short_report)], "rubattle"):
        problems.append("coverage did not notice a missing site")
    pvpgn_results = transform(SELF_TEST_FIXTURE, "pvpgn", 100)[1]
    if verify_coverage_reports(
        [("self", {**coverage_report, "profile": "pvpgn",
                   "files": [{"sites": pvpgn_results}]})],
        "pvpgn",
    ):
        problems.append("coverage rejected the pvpgn profile")

    if problems:
        for problem in problems:
            print(f"w3l_compat self-test: {problem}", file=sys.stderr)
        return 1
    print("w3l_compat self-test: ok")
    return 0


def idempotent(again: str, original: str) -> bool:
    return again == original


def _fromfile_arg(line: str) -> list[str]:
    line = line.strip()
    return [line] if line else []


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, fromfile_prefix_chars="@"
    )
    ap.convert_arg_line_to_args = _fromfile_arg
    ap.add_argument("--lift", help="generated C file containing the target functions")
    ap.add_argument("--out", help="output file (must differ from --lift)")
    ap.add_argument("--json", help="write the function-level change set here")
    ap.add_argument("--profile", choices=PROFILES, default="rubattle")
    ap.add_argument("--latency", type=int, default=100)
    ap.add_argument("--require-all", action="store_true")
    ap.add_argument("--check", action="store_true", help="inspect without writing")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument(
        "--verify-coverage",
        nargs="+",
        metavar="JSON",
        help="verify that every site of the profile appears in the union of "
        "per-chunk change sets (used by CMake after the split stage)",
    )
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.verify_coverage:
        try:
            verify_coverage([Path(p) for p in args.verify_coverage], args.profile)
        except PassError as exc:
            print(f"{PASS}: {exc}", file=sys.stderr)
            return 2
        if not args.quiet:
            print(
                f"{PASS}: coverage ok for {args.profile} "
                f"({len(args.verify_coverage)} change set(s))"
            )
        return 0
    if not args.lift:
        ap.error("--lift is required unless --self-test is used")
    if not args.check and not args.out:
        ap.error("--out is required unless --check is used")
    if args.out and Path(args.lift).resolve() == Path(args.out).resolve():
        ap.error("--out must differ from --lift; replace the corpus explicitly")
    if not 30 <= args.latency <= 255:
        ap.error("--latency must be in the wl27 range 30..255")

    try:
        return process_file(
            Path(args.lift),
            Path(args.out) if args.out else None,
            Path(args.json) if args.json else None,
            args.profile,
            args.latency,
            args.require_all,
            args.check,
            args.quiet,
        )
    except PassError as exc:
        print(f"{PASS}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
