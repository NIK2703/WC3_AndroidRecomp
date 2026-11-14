#!/usr/bin/env python3
"""Pinned game-state transition hooks for the 1.27.1.7085 lift.

The JSON table is the single source of truth for both lifters.  This module
validates it instead of trusting the addresses: a typo or a stale store must
fail before a corpus is produced.
"""

from __future__ import annotations

import dataclasses
import json
import re
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TABLE = ROOT / "config" / "state_hooks.1.27b.json"

_EXPECTED_KINDS = {
    0: "cgameui_created",
    1: "cgameui_seen",
    2: "cgameui_destroyed",
    3: "pause",
    4: "gameover",
    5: "gameover_cleared",
}

_EXPR = re.compile(r"^(?:c->[a-z][a-z0-9]*|0u)$")


class StateHookError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class StateHook:
    name: str
    kind: int
    store_va: int
    after_va: int
    instruction: bytes
    width: int
    value: str
    guard: str | None
    cache_source: str

    @property
    def line(self) -> str:
        if self.guard is not None:
            return (f"if (!{self.guard}) "
                    f"wc3_game_state_hook({self.kind}u, 0u);")
        return f"wc3_game_state_hook({self.kind}u, {self.value});"


def _parse_hex(value: object, field: str) -> int:
    if not isinstance(value, str) or not value.startswith("0x"):
        raise StateHookError(f"{field}: expected 0x-prefixed string")
    try:
        return int(value, 16)
    except ValueError as exc:
        raise StateHookError(f"{field}: not hexadecimal") from exc


def _check_expr(value: object, field: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or not _EXPR.match(value):
        raise StateHookError(
            f"{field}: only c-><reg> or 0u is allowed, got {value!r}")
    return value


def load_hooks(path: Path | str | None = None) -> tuple[StateHook, ...]:
    table_path = Path(path) if path is not None else DEFAULT_TABLE
    try:
        data = json.loads(table_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise StateHookError(f"missing table: {table_path}") from exc
    except json.JSONDecodeError as exc:
        raise StateHookError(f"{table_path}: invalid JSON: {exc}") from exc

    if data.get("schema") != 1:
        raise StateHookError("schema must be 1")
    if data.get("image") != "game.dll":
        raise StateHookError("image must be game.dll")
    if data.get("version") != "1.27.1.7085":
        raise StateHookError("version must be 1.27.1.7085")
    if data.get("image_base") != "0x6F000000":
        raise StateHookError("image_base must be 0x6F000000")

    raw_hooks = data.get("hooks")
    if not isinstance(raw_hooks, list) or len(raw_hooks) != len(_EXPECTED_KINDS):
        raise StateHookError(
            f"hooks must contain {len(_EXPECTED_KINDS)} entries")

    hooks: list[StateHook] = []
    seen_va: set[int] = set()
    seen_kind: set[int] = set()
    for index, raw in enumerate(raw_hooks):
        if not isinstance(raw, dict):
            raise StateHookError(f"hooks[{index}] must be an object")
        name = raw.get("name")
        if not isinstance(name, str) or not name:
            raise StateHookError(f"hooks[{index}].name missing")
        kind = raw.get("kind")
        if not isinstance(kind, int) or kind not in _EXPECTED_KINDS:
            raise StateHookError(f"{name}.kind must be 0..5")
        if _EXPECTED_KINDS[kind] != name:
            raise StateHookError(
                f"{name}.kind={kind} does not match "
                f"{_EXPECTED_KINDS[kind]}")
        if kind in seen_kind:
            raise StateHookError(f"duplicate kind {kind}")
        seen_kind.add(kind)

        store_va = _parse_hex(raw.get("store_va"), f"{name}.store_va")
        after_va = _parse_hex(raw.get("after_va"), f"{name}.after_va")
        instruction = raw.get("bytes")
        if not isinstance(instruction, str):
            raise StateHookError(f"{name}.bytes missing")
        try:
            insn = bytes.fromhex(instruction)
        except ValueError as exc:
            raise StateHookError(f"{name}.bytes is not hex") from exc
        if not insn:
            raise StateHookError(f"{name}.bytes is empty")
        if after_va != store_va + len(insn):
            raise StateHookError(
                f"{name}: after_va must be store_va + {len(insn)} bytes")
        if store_va in seen_va:
            raise StateHookError(f"duplicate store_va {store_va:#x}")
        seen_va.add(store_va)

        width = raw.get("width")
        if width not in (2, 4):
            raise StateHookError(f"{name}.width must be 2 or 4")
        value = _check_expr(raw.get("value"), f"{name}.value")
        if value is None:
            raise StateHookError(f"{name}.value missing")
        guard = _check_expr(raw.get("guard"), f"{name}.guard")
        cache_source = raw.get("cache_source")
        if not isinstance(cache_source, str) or not cache_source.strip():
            raise StateHookError(f"{name}.cache_source missing")

        # Pause is the only hook that watches a field write with a register
        # value; a constant guard belongs only to the getter.
        if name == "cgameui_seen" and guard is None:
            raise StateHookError("cgameui_seen must be conditional")
        if name != "cgameui_seen" and guard is not None:
            raise StateHookError(f"{name}: unexpected guard")

        hooks.append(StateHook(name=name, kind=kind, store_va=store_va,
                               after_va=after_va, instruction=insn,
                               width=width, value=value, guard=guard,
                               cache_source=cache_source))

    hooks.sort(key=lambda h: h.kind)
    if [h.kind for h in hooks] != list(range(len(_EXPECTED_KINDS))):
        raise StateHookError("hook kinds must cover 0..5 exactly")
    return tuple(hooks)


@lru_cache(maxsize=1)
def hooks() -> tuple[StateHook, ...]:
    return load_hooks()


@lru_cache(maxsize=1)
def hooks_by_va() -> dict[int, StateHook]:
    return {hook.store_va: hook for hook in hooks()}


def hook_at(va: int) -> StateHook | None:
    return hooks_by_va().get(va)


def verify_image(image, hooks_table: tuple[StateHook, ...] | None = None) -> None:
    """Check the pinned bytes against a PeImage-like object."""
    table = hooks_table if hooks_table is not None else hooks()
    for hook in table:
        actual = bytes(image.read_va(hook.store_va, len(hook.instruction)))
        if actual != hook.instruction:
            raise StateHookError(
                f"{hook.name}: bytes at {hook.store_va:#x} are {actual.hex()}, "
                f"expected {hook.instruction.hex()}")
