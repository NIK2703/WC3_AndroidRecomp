#!/usr/bin/env python3
"""T0: pinned state-hook table, lifter emission and generated C header."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lift"))

from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402
from pe_image import PeImage  # noqa: E402
import lifter32  # noqa: E402
import state_hooks  # noqa: E402
import gen_state_hooks  # noqa: E402


def main() -> int:
    game_dll = Path(r"E:\Games\Warcraft III\game.dll")
    if not game_dll.exists():
        print("state_hooks: unavailable (game.dll fixture missing)")
        return 2

    hooks = state_hooks.load_hooks()
    if [h.name for h in hooks] != [
            "cgameui_created", "cgameui_seen", "cgameui_destroyed",
            "pause", "gameover", "gameover_cleared"]:
        print("state_hooks: unexpected hook names")
        return 1

    pe = PeImage(game_dll)
    try:
        state_hooks.verify_image(pe, hooks)
    except state_hooks.StateHookError as exc:
        print(f"state_hooks: {exc}")
        return 1

    # The generated header must be a pure function of the JSON table.
    text = gen_state_hooks.render(hooks)
    for hook in hooks:
        expected = f'{hook.store_va:#010x}u, {hook.kind}u, "{hook.value}"'
        if expected not in text:
            print(f"state_hooks: generated header misses {hook.name}")
            return 1

    # Both lifters must append the hook after the exact store, not replace it.
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = lifter32.Lifter(str(game_dll), pe.image_size, pe.read_va)
    lifter.image_lo = pe.base
    lifter.image_hi = pe.base + pe.image_size
    for hook in hooks:
        insn = list(md.disasm(
            pe.read_va(hook.store_va, len(hook.instruction)),
            hook.store_va))
        if len(insn) != 1 or insn[0].size != len(hook.instruction):
            print(f"state_hooks: cannot decode {hook.name}")
            return 1
        out = lifter.translate(insn[0], [])
        if not out or out[-1] != hook.line:
            print(f"state_hooks: Python lifter mismatch for {hook.name}: {out}")
            return 1
        if len(out) < 2:
            print(f"state_hooks: store missing for {hook.name}")
            return 1

    csrc = (ROOT / "tools" / "lift" / "csrc" / "liftcore.c").read_text(
        encoding="utf-8")
    if "lf_state_hook_after(&e, insn);" not in csrc:
        print("state_hooks: C lifter does not call the hook emitter")
        return 1

    print("state_hooks: okay")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
