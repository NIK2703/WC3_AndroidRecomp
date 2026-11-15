/*
 * selection24.h - 24-unit selection and adaptive 6x2 / 8x3 unit panel.
 *
 * This is gameplay/UI code, not HLE: the selection manager and the info panel
 * remain lifted guest code.  The helpers below are called from the generated
 * corpus by tools/lift/csrc_local/selection24.c when WC3_SELECTION_24 is ON.
 *
 * The helpers call guest code only through push32 + dispatch and preserve the
 * caller's integer registers, esp, eip and flags.  They do not use host locks,
 * static TLS or untagged logging.
 *
 * The exact baseline (WC3_SELECTION_24=OFF) does not contain this module and
 * keeps the original 12-unit cap and the 6x2 grid.
 */
#ifndef WC3_FEATURES_SELECTION24_H
#define WC3_FEATURES_SELECTION24_H

#include <stdint.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called from the CInfoPanelGroup constructor after the original 6x2 grid and
 * its 12 buttons exist.  Creates an 8x3 grid as a second child of the group.
 *
 * group+0x130 = original 6x2 grid (unchanged)
 * group+0x134 = 8x3 grid, or zero when allocation failed
 * group+0x138 = active grid (grid12 initially)
 * group+0x13C = initialisation-attempted marker ("SEL2")
 * group+0x140 = total selected-unit count from CSelectionWar3+0x1F8
 *
 * The group allocation is widened from 0x134 to 0x144 bytes by the LiftPass.
 * The marker gates every helper: before init writes the marker the added
 * fields are not owned by this profile, so hide/fill fall back to grid12 and
 * select/note return without touching group memory.
 */
void wc3_sel24_init_group(CPU *c, uint32_t group);

/*
 * Called from the CInfoPanelGroup::Update prologue after the selection object
 * is available ([eax+0x34] -> esi).  Stores CSelectionWar3+0x1F8 into
 * group+0x140 so the mode choice and the fill loop can see the total count
 * even from split-entry fragments.
 */
void wc3_sel24_note_selection(CPU *c, uint32_t group, uint32_t selection);

/*
 * Called from CInfoPanelGroup::Update after [ebp-0x24] (the number of units in
 * the current subgroup) is known.  Selects 6x2 for <=12 and 8x3 for 13..24
 * using the total selection count in group+0x140.  subgroup_units is only a
 * fallback when the total is missing or out of range (a split fragment that
 * ran without the update prologue).  If grid24 is unavailable the mode stays
 * 6x2; if the mode does not change the active frame is still made visible
 * again, because a parent HideAll may have unregistered it.
 */
void wc3_sel24_select(CPU *c, uint32_t group, uint32_t subgroup_units);

/*
 * Replaces the original hard-coded 6x2 hide-all body.  Hides every button of
 * the active grid, but not the grid frame itself: the following fill path
 * shows individual buttons and must keep the grid registered.
 */
void wc3_sel24_hide_buttons(CPU *c, uint32_t group);

/*
 * Replaces the original hard-coded (index / 6, index % 6) fill block.  Uses
 * the active grid's column count, checks the cell and button, and calls the
 * original CInfoPanelGroup fill routine 0x6F3DA470.  index is the global
 * displayed-icon counter that the caller increments after each fill.
 */
void wc3_sel24_fill(CPU *c, uint32_t group, uint32_t index,
                    uint32_t unit, uint32_t flag);

/*
 * Debug-only trace of the original AddUnit cap.  The selection24 LiftPass
 * inserts a guarded call after `cmp eax, 0x18` at 0x6F28DC30; the body is
 * compiled only when WC3_DEBUG && WC3_DEBUG_SELECT24, so release keeps the
 * original cap path with no extra call.  The helper preserves the guest CPU
 * state, including flags, because the following `jae` depends on them.
 */
void wc3_sel24_trace_addunit(CPU *c, uint32_t group, uint32_t unit,
                             uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* WC3_FEATURES_SELECTION24_H */
