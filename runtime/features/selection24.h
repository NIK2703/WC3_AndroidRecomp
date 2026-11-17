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
 * keeps the original 12-unit cap and the 6x2 grid.  With the module ON the
 * cap is a runtime choice: the launch settings switch enables 24 or keeps the
 * original 12 (the default), so one product build serves both modes.
 */
#ifndef WC3_FEATURES_SELECTION24_H
#define WC3_FEATURES_SELECTION24_H

#include <stdint.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime unit cap used by every limit site the LiftPass rewrites.  The
 * product default is the original 12; the Android launch settings set
 * WC3_SELECTION_24=1 before android_main to enable the 24-unit profile
 * without a second corpus.  A missing variable, or any value other than "1",
 * keeps 12.  The value is latched on first use.
 */
uint32_t wc3_sel24_cap(void);

/*
 * 24-entry ground-order formation workspace.
 *
 * The lifted solver 0x6F16B2F0 keeps a local count, a 24-entry index array and
 * a 24-entry pair array at count+0, count+4 and count+0x64.  The original
 * helper 0x6F169300 zeroes only twelve pairs because it hard-codes
 * eax=count+0x34 and edx=2; the original sort 0x6F16DFD0 reads the pair array
 * at count+0x34/count+0x38 and is still used by the 12-entry caller
 * 0x6F16A6E0.  These two helpers contain the same memory operations for the
 * 24-entry layout; the pass calls them only from 0x6F16B2F0.  pair_base is the
 * guest address count+0x64.
 */
void wc3_sel24_init24(uint32_t pair_base);
void wc3_sel24_sort24(uint32_t count_ptr);

/* Reports an object reference that did not resolve: kind 1 = the selection
 * object's weak pair target, kind 2 = a handle slot with a released object. */
void wc3_sel24_trace_stale(CPU *c, uint32_t kind, uint32_t a, uint32_t b,
                           uint32_t d);

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

/*
 * Debug-only traces for the selection event that crashed on the second order
 * to a group of more than twelve units (write to [0+0x54] from 0x6F057C41).
 * All three are compiled only when WC3_DEBUG && WC3_DEBUG_SELECT24; release
 * keeps the original instruction stream unchanged.  Output goes to
 * <WC3_DATA_DIR>/sel24_trace.txt on Android so the stream stays separate from
 * the much larger run.txt, and falls back to stderr when the state directory
 * is unavailable.
 *
 * wc3_sel24_trace_evt2 is called on entry to 0x6F146D30 with the object that
 * supplies the event payload and the event argument.  It prints the object's
 * {handle, serial} pair (0x14/0x18), its ref/prev/class links and a read-only
 * replay of the handle table lookup at 0x6F054530.  For a rejected pair it
 * also dumps the object, walks the +0x08 interface chain, scans the live
 * handle table for the rejected serial, ref and self, and prints the source
 * interface passed to 0x6F146C10 at [caller_ebp+8] (with its own table scan).
 * The target event descriptor, the raw builder-B candidates and the guest EBP
 * chain are printed as well.
 *
 * wc3_sel24_trace_lookup is called around that lookup in 0x6F04D9C0:
 * stage 0 passes {handle, serial}, stage 1 the lookup result.  Only rejected
 * pairs and zero results are printed.
 *
 * wc3_sel24_trace_guard is called from the null-object early returns in
 * 0x6F057C30, 0x6F689A60 and 0x6F67ABE0.  It records the guard hit and
 * reuses the stack walk so a new build can prove the guard, not a different
 * path, handled the failed lookup or the event whose target interface has no
 * +0x54 object.
 */
void wc3_sel24_trace_evt2(CPU *c, uint32_t self, uint32_t arg);
void wc3_sel24_trace_lookup(CPU *c, uint32_t stage, uint32_t a, uint32_t b);
void wc3_sel24_trace_guard(CPU *c, uint32_t arg);

/*
 * wc3_sel24_trace_tasknull is called right after 0x6F6926B0 reads the task
 * payload at [factory+0x54].  The two guards it feeds - 0x6F689FC0 and
 * 0x6F691E60 - only see the null target; this trace records every task
 * creation, with `null=` marking the ones the engine drops: the wrapper, the
 * payload (vtable, class tag, handle/serial, +0x50/+0x54), the descriptor at
 * [ebp-0x3C] (class tag at +4, resolved class factory at +0xC, argument at
 * +0x10) and the order target from [ebp+8]/[ebp+0xC].
 */
void wc3_sel24_trace_tasknull(CPU *c, uint32_t wrapper);

/*
 * Called from the shared epilogue branch of 0x6F6926B0 at 0x6F69273F: the
 * class-instance check 0x6F04CDD0 returned zero, so the order is dropped for
 * that unit.  Records the rejected unit, the order argument, the class tag at
 * [ebp-0x48] and the wrapper object's identity fields.
 */
void wc3_sel24_trace_orderdrop(CPU *c, uint32_t wrapper, uint32_t tag,
                               uint32_t result);

/*
 * Debug-only traces of the per-unit command batch that explains why a group
 * order reaches fewer units than the selection holds.  Both helpers are
 * compiled only when WC3_DEBUG && WC3_DEBUG_SELECT24 and preserve the guest
 * CPU state, including flags, because the call sites sit between instructions
 * that still consume ecx, edx, esi and the flags.
 *
 * wc3_sel24_trace_order_unit is inserted at the entry of the two batch writers
 * 0x6F6BA590 and 0x6F6BA800, right after `mov esi, [edi]`.  `pair_off` names
 * the handle/serial pair the writer later pushes: 0x38 at the 0x6F6BA590 site
 * and 0x30 at the 0x6F6BA800 site.  The trace prints the object, its [0] word,
 * both pairs and the neighbouring fields, so a writer that reads a stale pair
 * is visible before the handle lookup runs.
 *
 * wc3_sel24_trace_order_loop is inserted after `mov edx, eax` at 0x6F6B9E8D
 * and 0x6F6BA4C2, where eax is the event object that 0x6F69BC80/0x6F69BD80
 * returned and ecx is the target unit.  `cursor_off` is the record offset the
 * loop's esi points at, 0x18 at the 0x6F6B9E8D site and 0x1C at the 0x6F6BA4C2
 * site, and it also selects the matching index slot ([ebp-0x208] or
 * [ebp-0x200]); mixing the two slots reads a pointer or a saved register
 * instead of the counter.  The trace prints the index, the batch count, the
 * event object and all nine dwords of the current record.  On index 0 it also
 * dumps the whole record array, which is what the writer actually produced
 * before the loop started skipping units.  The batch count is [ebp-0x1C4] in
 * both loops.
 */
void wc3_sel24_trace_order_unit(CPU *c, uint32_t pair_off, uint32_t object,
                                uint32_t self);
void wc3_sel24_trace_order_loop(CPU *c, uint32_t cursor_off, uint32_t cursor,
                                uint32_t unit, uint32_t event);

/*
 * Inserted after the element write of the three per-unit assignment lists
 * (0x6F6BB731 and 0x6F6BB90C for the class A container, 0x6F6BBFCC for the
 * old one).  `tag` is 1/2 for the class A layout (count +0x4C, array +0x50)
 * and 3 for the old layout (count +0x28, array +0x2C); `list` is the object
 * the writer appends to and `element` the slot it just stored.  The trace
 * prints both counts and the derived index, so a list that still stops at
 * twelve while the batch holds fourteen is visible in one line.
 */
void wc3_sel24_trace_order_slots(CPU *c, uint32_t tag, uint32_t list,
                                 uint32_t element);

/*
 * Inserted in 0x6F6B93A0 after the two calls to 0x6F04CDD0.  Those calls are
 * the last points where a per-unit order can still be rejected: the function
 * returns without touching the unit when the result is zero.  The unit is the
 * first argument and the event object the second; both are still live in the
 * frame at [ebp-0x14] and [ebp-0x10].  phase 0 is the first delivery,
 * phase 1 the second one.
 */
void wc3_sel24_trace_order_apply(CPU *c, uint32_t unit, uint32_t event,
                                 uint32_t result, uint32_t phase);

/*
 * Inserted in 0x6F6B93A0 around the virtual call that hands the order event
 * to the unit.  Stage 0 prints the unit, the event object, its vtable and the
 * handler that is about to run; stage 1 prints the handler result.  The
 * event object is ebx and the unit is the first argument, kept in the frame
 * at [ebp-0x14].
 */
void wc3_sel24_trace_order_exec(CPU *c, uint32_t stage, uint32_t unit,
                                uint32_t event, uint32_t vtbl,
                                uint32_t handler, uint32_t result);

/*
 * Inserted in 0x6F28EEF0, the function that removes one unit from the
 * selection group.  phase 0 marks the entry of the removal (the group is the
 * argument), phase 1 the actual removal, where the group, the removed unit
 * and the counter at +0x1F8 after the decrement are known.  A unit that dies
 * but is never removed from the group stays in the selection and later gets
 * an order with a dead target.
 */
void wc3_sel24_trace_remove(CPU *c, uint32_t phase, uint32_t group,
                            uint32_t unit);

/*
 * Inserted in 0x6F154B60 twice around the object lifetime: right after the
 * virtual factory returns the task object (0x6F154B76) and again after both
 * initialisation slots ran (0x6F154B92).  The first record shows the object at
 * birth with its class tag, handle and serial; the second one shows whether a
 * slot or the factory callback filled the +0x54 payload that 0x6F6926B0 later
 * reads.  `desc` is the descriptor built by 0x6F055D00.
 */
void wc3_sel24_trace_evbirth(CPU *c, uint32_t object, uint32_t desc);
void wc3_sel24_trace_evinit(CPU *c, uint32_t object, uint32_t desc);

/*
 * Inserted at 0x6F1CBFB3, the first instruction of the negative handle-table
 * release wrapper 0x6F1CBFB0.  The free list links slots through their own
 * value word, so one index released twice links to itself and the allocator
 * later returns 0xFFFFFFFE.  `slotval` equal to 0xFFFFFFFE on a repeated
 * `index` is the duplicate release.
 */
void wc3_sel24_trace_evrelease(CPU *c, uint32_t table, uint32_t handle);

/*
 * Inserted in 0x6F04CE16, the function that applies the order to one unit.
 * phase 0 prints the order object with its class tag, phase 1 the object
 * the apply returned with its +0x78 field.  A unit whose apply failed keeps
 * a null event and crashes the delivery, so the trace shows which branch
 * rejected the unit.
 */
int wc3_sel24_heap_ptr(uint32_t p);

void wc3_sel24_trace_apply(CPU *c, uint32_t phase, uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif /* WC3_FEATURES_SELECTION24_H */
