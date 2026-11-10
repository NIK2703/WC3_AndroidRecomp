#!/usr/bin/env python3
"""Static architecture gates for the Android-only target."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
HLE_ABI = RUNTIME / "hle" / "abi" / "hle_abi.h"
PLATFORM = RUNTIME / "platform" / "platform.h"

# Guest-visible locking lives in HLE objects; the CPU and ABI layers only
# emulate the guest's own lock-prefixed instructions.  `--concurrency` therefore
# checks zones and call graphs, not the spelling "mutex" in the runtime.
HOST_LOCK_RE = re.compile(
    r"\b(?:pthread_mutex_(?:lock|unlock|trylock|timedlock)|pthread_cond_wait|"
    r"std::(?:recursive_)?mutex|std::lock_guard|std::unique_lock|"
    r"EnterCriticalSection|LeaveCriticalSection|TryEnterCriticalSection|"
    r"WaitForSingleObject(?:Ex)?)\b"
)
SPINLOCK_RE = re.compile(
    r"\b(?:while|for)\s*\([^;{}]*(?:__sync_lock_test_and_set|"
    r"_InterlockedCompareExchange|atomic_flag_test_and_set)"
)
ATOMIC_RE = re.compile(r"\b(?:__atomic_|__sync_|_Interlocked|std::atomic)\w*")
DISPATCH_RE = re.compile(r"\b(?:dispatch|dispatch_jmp|hle_call)\s*\(")

# `--lock-inventory` is a separate, report-only textual inventory.  It does not
# feed `--concurrency`: a global lock is a diagnostic, not a new hard failure.
LOCK_DECL_RE = re.compile(
    r"\b(?P<qual>(?:(?:static|extern|const|volatile)\s+)*)"
    r"(?P<type>pthread_mutex_t|pthread_cond_t)\b\s*"
    r"(?P<ptr>\*?)\s*(?P<name>[A-Za-z_]\w*)"
    r"(?P<array>\s*\[[^\]]*\])?[^;{}]*;"
)
LOCK_CALL_RE = re.compile(
    r"\b(?P<name>pthread_mutex_lock|pthread_mutex_trylock|"
    r"pthread_mutex_timedlock|pthread_mutex_unlock|pthread_mutex_init|"
    r"pthread_mutex_destroy|pthread_cond_wait|pthread_cond_timedwait|"
    r"pthread_cond_signal|pthread_cond_broadcast|pthread_cond_init|"
    r"EnterCriticalSection|TryEnterCriticalSection|LeaveCriticalSection)\s*\("
)
STRUCT_RE = re.compile(
    r"\b(?P<typedef>typedef\s+)?(?P<kind>struct|union)\s*"
    r"(?P<tag>[A-Za-z_]\w*)?\s*\{"
)
# Name-based callback candidates.  This deliberately does not try to resolve
# arbitrary function pointers or helper call graphs; see the report limits.
CALLBACK_RE = re.compile(
    r"\b(?P<name>dispatch|dispatch_jmp|hle_call|g_previous_\w+|g_cb_body)\s*\("
)
# A function pointer resolved from the HLE registry is another guest callback
# path.  This only follows the simple `HleFn name = find_hle_fn(...);` shape.
FN_PTR_ASSIGN_RE = re.compile(
    r"\b(?:HleFn|Wc3LegacyFn)\s+(?P<name>[A-Za-z_]\w*)\s*=\s*"
    r"(?:find_hle_fn|[A-Za-z_]\w*hle_thunk_of)\s*\("
)
GUEST_STATE_RE = re.compile(
    r"\b(?P<name>wc3_legacy_(?:guest_ok|rd8g|rd16g|rd32g|rd64g|wr8g|wr16g|"
    r"wr32g|wr64g|guest_alloc\d*|guest_free|set_last_error|"
    r"current_display_mode|default_display_mode)|hle_guest_(?:alloc\d*|free)|"
    r"rd8g|rd16g|rd32g|rd64g|wr8g|wr16g|wr32g|wr64g)\s*\("
)
_TARGET_FIELD_RE = re.compile(r"(?:->|\.)\s*([A-Za-z_]\w*)\s*$")
_TARGET_ARRAY_RE = re.compile(r"\s*\[[^\]]*\]")
# These zones must not contain host locks or spinlocks.  The CPU layer may use
# seq_cst atomics for the guest's lock-prefixed instructions; the mailbox is
# explicitly lock-free and uses acquire/release atomics.
GUEST_ZONES = (
    "runtime/cpu/",
    "runtime/guest/",
    "runtime/hle/abi/",
    "runtime/app/guest_runner.",
)
LOCK_FREE_ZONES = ("runtime/ipc/",)
HLE_PREFIX = "runtime/hle/"
PLATFORM_PREFIX = "runtime/platform/"
# The lock-trace probe owns the mutex that protects its diagnostic table.  It
# is debug-only, never calls dispatch() and never waits on a guest object.
# Allow that one explicit owner; do not turn the whole diagnostic tree into an
# implicit lock zone.
OWNED_DIAG_LOCK_FILES = ("runtime/diag/wc3_lock_trace.c",)

# NetApi is a required platform boundary (ADR-031): LAN is in scope and the
# winsock HLE owns the guest-visible socket semantics.
REQUIRED_APIS = (
    "Wc3MemApi",
    "Wc3FileApi",
    "Wc3TimeApi",
    "Wc3LocaleApi",
    "Wc3WindowApi",
    "Wc3InputApi",
    "Wc3AudioApi",
    "Wc3ClipboardApi",
    "Wc3SurfaceApi",
    "Wc3LogApi",
    "Wc3ConfigApi",
    "Wc3AssetApi",
    "Wc3NetApi",
    "Wc3ThreadApi",
    "Wc3SyncApi",
    "Wc3VulkanDriverApi",
)

def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def source_files() -> list[Path]:
    out = []
    for ext in ("*.c", "*.h", "*.cc", "*.cpp", "*.hpp"):
        out.extend(RUNTIME.rglob(ext))
    return sorted(p for p in out if "third_party" not in p.parts)


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def check_hle_abi() -> list[str]:
    errors: list[str] = []
    text = HLE_ABI.read_text(encoding="utf-8")
    if text.count("typedef struct HleEntry") != 1:
        errors.append("hle_abi.h: expected exactly one HleEntry definition")
    if "#if WC3_DEBUG" not in text or "HleDebugEntry" not in text:
        errors.append("hle_abi.h: debug names are not gated by #if WC3_DEBUG")
    if "hle_stub" in text:
        errors.append("hle_abi.h: hle_stub must not exist")
    abi_dir = HLE_ABI.parent
    for path in abi_dir.glob("*.[ch]"):
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in re.finditer(r"__atomic_|__sync_|pthread_mutex_|std::mutex", body):
            line = body[: m.start()].count("\n") + 1
            errors.append(f"{rel(path)}:{line}: lock/atomic in HLE ABI")
    call = (abi_dir / "hle_abi.c").read_text(encoding="utf-8")
    if "hle_entry_for" not in call or "stack_cleanup" not in call:
        errors.append("hle_abi.c: direct thunk lookup/cleanup missing")
    return errors


def _function_spans(body: str):
    """Yield (start, end, segment) for rough top-level function bodies."""
    depth = 0
    start = 0
    seg_start = None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                header = body[start:i]
                if "(" in header and not header.rstrip().endswith(";"):
                    seg_start = start
                else:
                    seg_start = None
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
                if depth == 0:
                    if seg_start is not None:
                        yield seg_start, i + 1, body[seg_start : i + 1]
                    start = i + 1
                    seg_start = None


def _function_segments(body: str):
    """Yield rough top-level function segments for lock/dispatch checks."""
    for _, _, segment in _function_spans(body):
        yield segment


def _mask_literals(text: str) -> str:
    """Blank comments and strings, preserving offsets and newlines."""
    out = list(text)
    i = 0
    while i < len(text):
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if text[i] == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < len(text):
                if text[i] == "*" and i + 1 < len(text) and text[i + 1] == "/":
                    out[i] = out[i + 1] = " "
                    i += 2
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        if text[i] == "/" and nxt == "/":
            out[i] = out[i + 1] = " "
            i += 2
            while i < len(text) and text[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if text[i] in ("'", '"'):
            quote = text[i]
            out[i] = " "
            i += 1
            while i < len(text):
                if text[i] == "\\":
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
                    if i < len(text):
                        if text[i] != "\n":
                            out[i] = " "
                        i += 1
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _matching(masked: str, open_pos: int, opening: str, closing: str) -> int:
    depth = 0
    for i in range(open_pos, len(masked)):
        if masked[i] == opening:
            depth += 1
        elif masked[i] == closing:
            depth -= 1
            if depth == 0:
                return i
    return -1


def _matching_brace(masked: str, open_pos: int) -> int:
    return _matching(masked, open_pos, "{", "}")


def _matching_paren(masked: str, open_pos: int) -> int:
    return _matching(masked, open_pos, "(", ")")


def _brace_depth_at(masked: str, pos: int) -> int:
    depth = 0
    for ch in masked[:pos]:
        if ch == "{":
            depth += 1
        elif ch == "}" and depth:
            depth -= 1
    return depth


def _call_args(masked: str, open_pos: int) -> list[str]:
    close = _matching_paren(masked, open_pos)
    if close < 0:
        return []
    args = []
    depth = 0
    start = open_pos + 1
    for i in range(open_pos + 1, close):
        ch = masked[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}" and depth:
            depth -= 1
        elif ch == "," and depth == 0:
            args.append(masked[start:i])
            start = i + 1
    args.append(masked[start:close])
    return args


def _normalize_lock_target(argument: str) -> str:
    target = argument.strip()
    while target:
        if target.startswith("&"):
            target = target[1:].strip()
            continue
        if not target.startswith("("):
            break
        close = _matching_paren(target, 0)
        if close < 0:
            break
        inner = target[1:close].strip()
        if close == len(target) - 1:
            target = inner
            continue
        if "*" in inner or re.fullmatch(r"[A-Za-z_]\w*", inner):
            target = target[close + 1 :].strip()
            continue
        break
    return re.sub(r"\s*(->|\.)\s*", r"\1", target).strip()


def _struct_blocks(masked: str) -> list[dict[str, object]]:
    blocks = []
    for m in STRUCT_RE.finditer(masked):
        open_pos = m.end() - 1
        close = _matching_brace(masked, open_pos)
        if close < 0:
            continue
        owner = m.group("tag") or "<anonymous>"
        if m.group("typedef"):
            semi = masked.find(";", close + 1)
            tail = masked[close + 1 : semi if semi >= 0 else close + 80]
            alias = re.search(r"\b([A-Za-z_]\w*)\b", tail)
            if alias:
                owner = alias.group(1)
        blocks.append({"open": open_pos, "close": close, "owner": owner})
    return blocks


def _field_uses(masked: str, field: str) -> list[tuple[int, bool]]:
    """Return (position, proven_object_binding) for a struct field name."""
    uses = []
    seen = set()
    pointer_re = re.compile(r"->\s*" + re.escape(field) + r"\b")
    for m in pointer_re.finditer(masked):
        if m.start() not in seen:
            seen.add(m.start())
            uses.append((m.start(), True))
    dot_re = re.compile(
        r"(?P<chain>[A-Za-z_]\w*(?:\s*\[[^\]]*\])*"
        r"(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\.\s*"
        + re.escape(field)
        + r"\b"
    )
    for m in dot_re.finditer(masked):
        if m.start() not in seen:
            seen.add(m.start())
            chain = m.group("chain")
            uses.append((m.start(), "->" in chain or "[" in chain))
    deref_re = re.compile(
        r"\*\s*[A-Za-z_]\w*\s*\)\s*\.\s*" + re.escape(field) + r"\b"
    )
    for m in deref_re.finditer(masked):
        if m.start() not in seen:
            seen.add(m.start())
            uses.append((m.start(), True))
    return uses


def _lock_declarations(
    path: Path, key: str, text: str, masked: str
) -> list[dict[str, object]]:
    spans = list(_function_spans(masked))
    blocks = _struct_blocks(masked)
    decls = []
    for m in LOCK_DECL_RE.finditer(masked):
        pos = m.start()
        owner = None
        for block in blocks:
            if block["open"] < pos < block["close"]:
                if owner is None or (
                    block["close"] - block["open"] < owner["close"] - owner["open"]
                ):
                    owner = block
        in_function = any(start < pos < end for start, end, _ in spans)
        qual = m.group("qual") or ""
        is_static = bool(re.search(r"\bstatic\b", qual))
        is_extern = bool(re.search(r"\bextern\b", qual))
        pointer = bool(m.group("ptr"))
        array = bool(m.group("array"))
        name = m.group("name")
        if owner is not None:
            scope = "field"
        elif is_extern:
            scope = "extern"
        elif in_function:
            scope = "function"
        else:
            scope = "file" if _brace_depth_at(masked, pos) == 0 else "block"
        if pointer:
            cls = "unknown"
            detail = "pointer (dynamic or aliased storage)"
        elif is_extern:
            cls = "unknown"
            detail = "extern declaration"
        elif scope == "field":
            cls = "unknown"
            detail = f"struct field {owner['owner']}.{name}"
        elif scope == "file":
            cls = "global"
            detail = "static file-scope" if is_static else "file-scope"
        elif scope == "function":
            cls = "global" if is_static else "local"
            detail = "function-static" if is_static else "stack"
        else:
            cls = "unknown"
            detail = "block-scope declaration"
        if array and cls == "global":
            detail += "; array (sharded global storage)"
        if scope == "field":
            display = f"{owner['owner']}.{name}"
        else:
            display = name
        decls.append(
            {
                "path": path,
                "key": key,
                "line": _line_of(text, pos),
                "name": name,
                "type": m.group("type"),
                "display": display,
                "scope": scope,
                "class": cls,
                "detail": detail,
                "field": name if scope == "field" else None,
                "array": array,
                "pointer": pointer,
                "ambiguous": False,
                "acquires": 0,
                "releases": 0,
                "inits": 0,
                "waits": 0,
                "signals": 0,
                "destroys": 0,
            }
        )
    return decls


def _lock_calls(masked: str) -> list[dict[str, object]]:
    calls = []
    acquire = {
        "pthread_mutex_lock",
        "pthread_mutex_trylock",
        "pthread_mutex_timedlock",
        "EnterCriticalSection",
        "TryEnterCriticalSection",
    }
    release = {"pthread_mutex_unlock", "LeaveCriticalSection"}
    waits = {"pthread_cond_wait", "pthread_cond_timedwait"}
    signals = {"pthread_cond_signal", "pthread_cond_broadcast"}
    inits = {"pthread_mutex_init", "pthread_cond_init"}
    for m in LOCK_CALL_RE.finditer(masked):
        name = m.group("name")
        args = _call_args(masked, m.end() - 1)
        if not args:
            continue
        target_arg = args[0]
        cond_target = None
        kind = "other"
        if name in acquire:
            kind = "acquire"
        elif name in release:
            kind = "release"
        elif name in waits:
            if len(args) < 2:
                continue
            kind = "wait"
            target_arg = args[1]
            cond_target = _normalize_lock_target(args[0])
        elif name in signals:
            kind = "signal"
        elif name in inits:
            kind = "init"
        elif name == "pthread_mutex_destroy":
            kind = "destroy"
        target = _normalize_lock_target(target_arg)
        if not target:
            continue
        calls.append(
            {
                "name": name,
                "kind": kind,
                "pos": m.start(),
                "line": _line_of(masked, m.start()),
                "depth": _brace_depth_at(masked, m.start()),
                "target": target,
                "identity": _lock_identity(target),
                "cond_target": cond_target,
            }
        )
    return calls


def _callback_calls(masked: str) -> list[dict[str, object]]:
    calls = []
    seen = set()
    for regex, kind in ((CALLBACK_RE, "callback"), (GUEST_STATE_RE, "guest-state")):
        for m in regex.finditer(masked):
            seen.add((m.start(), m.group("name")))
            calls.append(
                {
                    "name": m.group("name"),
                    "kind": kind,
                    "pos": m.start(),
                    "line": _line_of(masked, m.start()),
                }
            )
    for m in FN_PTR_ASSIGN_RE.finditer(masked):
        name = m.group("name")
        for call in re.finditer(r"\b" + re.escape(name) + r"\s*\(", masked):
            marker = (call.start(), name)
            if marker in seen:
                continue
            seen.add(marker)
            calls.append(
                {
                    "name": name,
                    "kind": "callback-fnptr",
                    "pos": call.start(),
                    "line": _line_of(masked, call.start()),
                }
            )
    return sorted(calls, key=lambda call: call["pos"])


def _lock_identity(target: str) -> str:
    # Array indexes are not resolved; all elements of one array share a
    # conservative identity for hold/nesting analysis.
    return _TARGET_ARRAY_RE.sub("[]", target)


def _resolve_lock_target(
    target: str,
    by_name: dict[str, list[dict[str, object]]],
    field_decls: dict[str, list[dict[str, object]]],
) -> list[dict[str, object]]:
    field_match = _TARGET_FIELD_RE.search(target)
    if field_match:
        return list(field_decls.get(field_match.group(1), []))
    bare = _TARGET_ARRAY_RE.sub("", target).strip()
    return list(by_name.get(bare, []))


def _zone_of(key: str) -> str:
    if key.startswith("runtime/hle/abi/"):
        return "hle-abi"
    if key.startswith(HLE_PREFIX):
        return "hle"
    if key.startswith(PLATFORM_PREFIX):
        return "platform"
    if any(key.startswith(prefix) for prefix in GUEST_ZONES):
        return "guest"
    if any(key.startswith(prefix) for prefix in LOCK_FREE_ZONES):
        return "lock-free"
    return "other"


def _decl_label(decl: dict[str, object] | None, target: str) -> str:
    if decl is None:
        return f"{target} [unknown]"
    return f"{decl['display']} [{decl['class']}]"


def lock_inventory_report() -> list[str]:
    """Build the report-only HLE lock inventory (never adds gate errors).

    File-scope/static storage is global.  A struct field is per-object only
    when a -> or [] member use proves an instance binding; otherwise it stays
    unknown.  Hold/nesting findings are lexical order hints, not a proof.
    """
    files = [p for p in source_files() if rel(p).startswith(HLE_PREFIX)]
    all_decls: list[dict[str, object]] = []
    unresolved: list[dict[str, object]] = []
    hold_findings: list[dict[str, object]] = []
    nesting_findings: list[dict[str, object]] = []
    seen_hold = set()
    seen_nesting = set()

    for path in files:
        key = rel(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        masked = _mask_literals(text)
        spans = list(_function_spans(masked))
        decls = _lock_declarations(path, key, text, masked)
        for decl in decls:
            if decl["scope"] != "field" or decl["pointer"]:
                continue
            uses = _field_uses(masked, str(decl["field"]))
            proven = [use for use in uses if use[1]]
            detail = str(decl["detail"])
            if proven:
                decl["class"] = "per-object"
                decl["detail"] = detail + "; bound via -> or []"
            elif uses:
                decl["class"] = "unknown"
                decl["detail"] = detail + "; dot access only, object binding not proven"
            else:
                decl["class"] = "unknown"
                decl["detail"] = detail + "; no member use found"

        by_name: dict[str, list[dict[str, object]]] = {}
        field_decls: dict[str, list[dict[str, object]]] = {}
        for decl in decls:
            if decl["scope"] == "field":
                field_decls.setdefault(str(decl["name"]), []).append(decl)
            else:
                by_name.setdefault(str(decl["name"]), []).append(decl)
        for same_name in field_decls.values():
            if len(same_name) > 1:
                for decl in same_name:
                    decl["ambiguous"] = True

        calls = _lock_calls(masked)
        for call in calls:
            call["key"] = key
            candidates = _resolve_lock_target(
                str(call["target"]), by_name, field_decls
            )
            if candidates:
                if len(candidates) > 1:
                    for decl in candidates:
                        decl["ambiguous"] = True
                decl = candidates[0]
                call["decl"] = decl
                counter = {
                    "acquire": "acquires",
                    "release": "releases",
                    "init": "inits",
                    "wait": "waits",
                    "signal": "signals",
                    "destroy": "destroys",
                }.get(str(call["kind"]))
                if counter:
                    decl[counter] = int(decl[counter]) + 1
            else:
                unresolved.append(call)
            if call["cond_target"]:
                cond_candidates = _resolve_lock_target(
                    str(call["cond_target"]), by_name, field_decls
                )
                if cond_candidates:
                    if len(cond_candidates) > 1:
                        for decl in cond_candidates:
                            decl["ambiguous"] = True
                    cond_decl = cond_candidates[0]
                    cond_decl["waits"] = int(cond_decl["waits"]) + 1
                else:
                    unresolved.append(
                        {
                            "key": key,
                            "line": call["line"],
                            "name": str(call["name"]) + " (cond)",
                            "target": call["cond_target"],
                        }
                    )

        callbacks = _callback_calls(masked)
        for start, end, segment in spans:
            body_start = start + segment.find("{")
            events = []
            for call in calls:
                if body_start <= int(call["pos"]) < end and call["kind"] in (
                    "acquire",
                    "release",
                ):
                    events.append(("call", int(call["pos"]), call))
            for callback in callbacks:
                if body_start <= int(callback["pos"]) < end:
                    events.append(("callback", int(callback["pos"]), callback))
            events.sort(key=lambda event: event[1])

            held: dict[str, list[dict[str, object]]] = {}
            for event_type, _, event in events:
                if event_type == "callback":
                    for target, stack in held.items():
                        for acquired in stack:
                            marker = (
                                key,
                                int(acquired["line"]),
                                str(event["name"]),
                                int(event["line"]),
                                target,
                            )
                            if marker not in seen_hold:
                                seen_hold.add(marker)
                                hold_findings.append(
                                    {
                                        "key": key,
                                        "acq_line": acquired["line"],
                                        "target": target,
                                        "decl": acquired.get("decl"),
                                        "cb_name": event["name"],
                                        "cb_line": event["line"],
                                        "cb_kind": event["kind"],
                                    }
                                )
                    continue
                kind = event["kind"]
                target = str(event["identity"])
                if kind == "acquire":
                    stack = held.setdefault(target, [])
                    for outer in stack:
                        marker = (key, int(event["line"]), target)
                        if marker not in seen_nesting:
                            seen_nesting.add(marker)
                            nesting_findings.append(
                                {
                                    "key": key,
                                    "inner_line": event["line"],
                                    "inner_target": target,
                                    "inner_decl": event.get("decl"),
                                    "outer_line": outer["line"],
                                    "outer_target": target,
                                    "outer_decl": outer.get("decl"),
                                }
                            )
                    stack.append(event)
                elif kind == "release":
                    stack = held.get(target)
                    if not stack:
                        continue
                    acquired = stack[-1]
                    # A release inside a nested block (usually an error path or
                    # a conditional branch) does not prove the lock is free for
                    # the later code; keep it held and report "possible".
                    if int(event["depth"]) <= int(acquired["depth"]):
                        stack.pop()
                        if not stack:
                            del held[target]

        all_decls.extend(decls)

    lines = [
        "HLE lock inventory (report-only; textual heuristic, not a gate)",
        f"scope: {len(files)} source files under runtime/hle/ (zones hle/hle-abi)",
        "classification: global = file-scope or static storage; per-object = "
        "struct field proven via -> or []; local = stack; unknown = not proven",
        "limits: pthread_mutex_t/pthread_cond_t only (WaitForSingleObject, "
        "semaphores, C++ std::mutex/RAII excluded); no preprocessor/macro "
        "expansion,",
        "        pointer-alias resolution, helper call graph or cross-function "
        "lock transfer; no path sensitivity (conditional unlocks/trylock/returns);",
        "        hold/nesting lines are possible lexical findings, not proof; "
        "callback detection is name-based and host/platform callbacks "
        "(Wc3*Api) are not guest callbacks here",
    ]
    sections = (
        ("global", "global locks (file-scope or static storage)"),
        ("per-object", "per-object locks (struct field bound to an object)"),
        ("local", "local locks (stack storage)"),
        ("unknown", "unknown locks (classification not proven)"),
    )
    for cls, heading in sections:
        selected = [decl for decl in all_decls if decl["class"] == cls]
        lines.append("")
        lines.append(heading + ":")
        if not selected:
            lines.append("  none")
            continue
        for decl in selected:
            counts = [
                f"acquire={decl['acquires']}",
                f"release={decl['releases']}",
            ]
            for field, label in (
                ("inits", "init"),
                ("waits", "wait"),
                ("signals", "signal"),
                ("destroys", "destroy"),
            ):
                if int(decl[field]):
                    counts.append(f"{label}={decl[field]}")
            detail = str(decl["detail"])
            if decl["ambiguous"]:
                detail += "; field name is ambiguous in this file"
            lines.append(
                f"  {decl['key']}:{decl['line']}: "
                f"[{_zone_of(str(decl['key']))}/{decl['class']}] "
                f"{decl['type']} {decl['display']} "
                f"({detail}; {', '.join(counts)})"
            )

    class_counts = {
        cls: sum(1 for decl in all_decls if decl["class"] == cls)
        for cls in ("global", "per-object", "local", "unknown")
    }
    acquisitions = sum(int(decl["acquires"]) for decl in all_decls)
    lines.append("")
    lines.append(
        "summary: objects={} (global={}, per-object={}, local={}, unknown={}); "
        "acquisitions={}; unresolved calls={}".format(
            len(all_decls),
            class_counts["global"],
            class_counts["per-object"],
            class_counts["local"],
            class_counts["unknown"],
            acquisitions,
            len(unresolved),
        )
    )

    lines.append("")
    lines.append("unresolved lock calls (alias/dynamic/macro targets, not classified):")
    if not unresolved:
        lines.append("  none")
    else:
        for call in unresolved[:40]:
            lines.append(
                f"  {call['key']}:{call['line']}: "
                f"{call['name']}({call['target']})"
            )
        if len(unresolved) > 40:
            lines.append(f"  ... {len(unresolved) - 40} more")

    lines.append("")
    lines.append(
        "possible lock held across dispatch/guest callback (lexical order only):"
    )
    if not hold_findings:
        lines.append("  none")
    else:
        for finding in sorted(
            hold_findings,
            key=lambda item: (
                str(item["key"]),
                int(item["acq_line"]),
                int(item["cb_line"]),
            ),
        ):
            label = _decl_label(finding.get("decl"), str(finding["target"]))
            lines.append(
                f"  {finding['key']}:{finding['acq_line']}: {label} held at "
                f"{finding['cb_kind']} call to {finding['cb_name']} "
                f"(line {finding['cb_line']})"
            )

    lines.append("")
    lines.append("possible lock nesting (lexical order only; not an order proof):")
    if not nesting_findings:
        lines.append("  none")
    else:
        for finding in sorted(
            nesting_findings,
            key=lambda item: (str(item["key"]), int(item["inner_line"])),
        ):
            inner = _decl_label(
                finding.get("inner_decl"), str(finding["inner_target"])
            )
            outer = _decl_label(
                finding.get("outer_decl"), str(finding["outer_target"])
            )
            lines.append(
                f"  {finding['key']}:{finding['inner_line']}: acquiring {inner} "
                f"while {outer} is held (line {finding['outer_line']})"
            )

    lines.append("")
    lines.append(
        "note: report-only; --concurrency behavior is unchanged, this adds no "
        "errors and does not prove the lock invariant."
    )
    return lines


def check_concurrency() -> list[str]:
    errors: list[str] = []
    for path in source_files():
        key = rel(path)
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        guest_zone = any(key.startswith(prefix) for prefix in GUEST_ZONES)
        lock_free_zone = any(key.startswith(prefix) for prefix in LOCK_FREE_ZONES)
        hle_zone = (
            key.startswith(HLE_PREFIX) and not key.startswith("runtime/hle/abi/")
        )
        platform_zone = key.startswith(PLATFORM_PREFIX)
        owned_diag_lock = key in OWNED_DIAG_LOCK_FILES

        for m in HOST_LOCK_RE.finditer(body):
            if guest_zone or lock_free_zone:
                line = body[: m.start()].count("\n") + 1
                errors.append(
                    f"{key}:{line}: host lock in guest/lock-free zone: {m.group(0)}"
                )
            elif (
                not hle_zone
                and not platform_zone
                and not owned_diag_lock
            ):
                line = body[: m.start()].count("\n") + 1
                errors.append(f"{key}:{line}: unowned host lock: {m.group(0)}")

        for m in SPINLOCK_RE.finditer(body):
            line = body[: m.start()].count("\n") + 1
            errors.append(f"{key}:{line}: spinlock is forbidden")

        # The invariant is about the call graph, not the presence of a lock:
        # a per-object HLE lock may exist, but dispatch()/hle_call() must not
        # run while it is held.
        for segment in _function_segments(body):
            lock = HOST_LOCK_RE.search(segment) or SPINLOCK_RE.search(segment)
            if lock and DISPATCH_RE.search(segment):
                line = body[: body.find(segment)].count("\n") + 1
                errors.append(
                    f"{key}:{line}: dispatch/hle_call reachable with a host lock"
                )
    return errors


def check_platform_contract() -> list[str]:
    errors: list[str] = []
    text = PLATFORM.read_text(encoding="utf-8")
    body = strip_comments(text)
    for api in REQUIRED_APIS:
        if api not in body:
            errors.append(f"platform.h: missing {api}")
    if "wc3_platform_install" not in body or "wc3_platform" not in body:
        errors.append("platform.h: missing platform installation contract")
    if "guest" not in text.lower():
        errors.append("platform.h: guest thread contract is not documented")
    return errors


def check_release() -> list[str]:
    errors: list[str] = []
    for path in source_files():
        body = path.read_text(encoding="utf-8", errors="replace")
        if "#ifdef WC3_DEBUG" in body:
            errors.append(f"{rel(path)}: use #if WC3_DEBUG, not #ifdef")
    abi = HLE_ABI.read_text(encoding="utf-8")
    if "#ifndef WC3_DEBUG" not in abi or "#define WC3_DEBUG 0" not in abi:
        errors.append("hle_abi.h: missing WC3_DEBUG default zero")
    return errors


def check_lifecycle() -> list[str]:
    errors: list[str] = []
    hdr = (RUNTIME / "app" / "guest_runner.h").read_text(encoding="utf-8")
    src = (RUNTIME / "app" / "guest_runner.c").read_text(encoding="utf-8")
    for state in (
        "CREATED",
        "RUNNING",
        "PAUSED",
        "SURFACE_LOST",
        "AUDIO_DEVICE_LOST",
        "STOPPING",
        "STOPPED",
    ):
        if f"WC3_RUNNER_{state}" not in hdr:
            errors.append(f"guest_runner.h: missing state {state}")
    for event in (
        "START",
        "PAUSE",
        "RESUME",
        "SURFACE_LOST",
        "SURFACE_RESTORED",
        "AUDIO_DEVICE_LOST",
        "AUDIO_DEVICE_RESTORED",
        "STOP",
        "FINISHED",
    ):
        if f"WC3_RUNNER_EV_{event}" not in src:
            errors.append(f"guest_runner.c: missing transition {event}")
    if "wc3_mailbox_push" not in src:
        errors.append("guest_runner.c: Android events are not routed through the mailbox")
    if "pthread_mutex" in src or "std::mutex" in src:
        errors.append("guest_runner.c: host lock added")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hle-abi", action="store_true")
    parser.add_argument("--concurrency", action="store_true")
    parser.add_argument(
        "--lock-inventory",
        action="store_true",
        help="report HLE lock owners/nesting/dispatch risk (report-only)",
    )
    parser.add_argument("--platform-contract", action="store_true")
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--lifecycle", action="store_true")
    parser.add_argument("--renderer", action="store_true")
    parser.add_argument("--policy", action="store_true")
    args = parser.parse_args(argv)

    if not any(vars(args).values()):
        parser.error("choose at least one check")

    # --lock-inventory is report-only and intentionally outside --strict.
    gate_selected = any(
        (
            args.hle_abi,
            args.concurrency,
            args.platform_contract,
            args.release,
            args.strict,
            args.lifecycle,
        )
    )
    if args.lock_inventory:
        for line in lock_inventory_report():
            print(line)

    errors: list[str] = []
    if args.hle_abi or args.strict:
        errors += check_hle_abi()
    if args.concurrency or args.strict:
        errors += check_concurrency()
    if args.platform_contract or args.strict:
        errors += check_platform_contract()
    if args.release or args.strict:
        errors += check_release()
    if args.lifecycle or args.strict:
        errors += check_lifecycle()
    if args.renderer or args.policy:
        print("not implemented yet: renderer/policy check", file=sys.stderr)
        return 2

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    if gate_selected:
        print("architecture checks OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
