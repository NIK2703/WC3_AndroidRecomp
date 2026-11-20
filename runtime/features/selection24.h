/*
 * selection24.h - 12/24/40-unit selection and adaptive 6x2 / 8x3 / 10x4 panel.
 *
 * This is gameplay/UI code, not HLE: the selection manager and the info panel
 * remain lifted guest code.  The helpers below are called from the generated
 * corpus by tools/lift/csrc_local/selection24.c when the selection24 profile is
 * ON.
 *
 * The helpers call guest code only through push32 + dispatch and preserve the
 * caller's integer registers, esp, eip and flags.  They do not use host locks,
 * static TLS or untagged logging.
 *
 * The exact baseline (selection24 OFF) does not contain this module and keeps
 * the original 12-unit cap and the 6x2 grid.  With the module ON the corpus
 * carries the 24-entry comparison layout or the 40-entry product layout chosen
 * at configure time, and the cap is a runtime choice: the Android launch
 * settings write WC3_SELECTION_CAP=12|24|40 before android_main, so the 40
 * product build serves all three modes.
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
 * WC3_SELECTION_CAP=12|24|40 before android_main.  The legacy
 * WC3_SELECTION_24=1 is still honoured for older hosts and selects 24.  An
 * unknown or missing value keeps 12.  The value is latched on first use.
 */
uint32_t wc3_sel24_cap(void);

/*
 * Save/load stream compatibility for the PrClGen slot array.
 *
 * The pre-40 corpus wrote wc3_sel24_cap() slots and read wc3_sel24_cap()
 * slots; the stream itself carried no count.  A 40-slot reader therefore
 * over-read a legacy 24-slot save and produced a null unit at the end of the
 * group.  New saves put a marker in the third dword of slot 0 (the field is
 * zero in every initialised slot and is not part of the order state); the
 * reader, which starts with the legacy 24 default, detects the marker after
 * reading slot 0 and adjusts its remaining-slot counter.  Old unmarked saves
 * therefore keep loading as 24, while new 12/24/40 saves round-trip at their
 * own width.
 *
 * wc3_sel24_stream_slot_write/read are called from the generated serializer
 * (0x6F16AE87) and deserializer (0x6F16ABD9).  They only touch guest memory
 * and never call guest code.
 */
uint32_t wc3_sel24_stream_default_cap(void);
uint32_t wc3_sel24_stream_slot_write(uint32_t self, uint32_t slot_ptr,
                                     uint32_t value);
void wc3_sel24_stream_slot_read(uint32_t slot_serial, uint32_t bool_base,
                                uint32_t cap_addr);

/*
 * Called at the entry of the CPrCluster member-vector insertion 0x6F16C077.
 * The member vector is normally a fixed 12-slot vector (growth chunk zero), so
 * the 13th insertion fails and the caller silently drops the unit.  In the
 * 24/40-unit profile this gives the vector the game's own growth chunk (12),
 * which lets the existing grow path 0x6F16C0B3 reallocate as needed.  It does
 * nothing for the baseline 12-unit profile and for any vector that is not the
 * CPrCluster member vector at cluster+0x1C.
 */
void wc3_sel24_cluster_prepare(uint32_t vector);

/*
 * Diagnostic trace inserted at the ground-order base read 0x6F16D012 and at
 * the group builder entry 0x6F16A5B3.  sel is the selection/cluster object,
 * group the temporary group, cursor the batch cursor value and idx the loop
 * index.  It prints the live [sel+0x28] unit-record base, the embedded member
 * vector fields and the group index, so a 12-entry base reused after the
 * member-vector grow is visible in sel24_trace.txt.  Compiled out unless
 * WC3_DEBUG && WC3_DEBUG_SELECT24.
 */
void wc3_sel24_trace_order_base(CPU *c, uint32_t sel, uint32_t group,
                                uint32_t cursor, uint32_t idx);

/*
 * 40-entry ground-order formation workspace.
 *
 * The lifted solver 0x6F16B2F0 keeps a local count, a 40-entry index array and
 * a 40-entry pair array at count+0, count+4 and count+0xA4.  The original
 * helper 0x6F169300 zeroes only twelve pairs because it hard-codes
 * eax=count+0x34 and edx=2; the original sort 0x6F16DFD0 reads the pair array
 * at count+0x34/count+0x38 and is left in place for reference.  Both live
 * callers now use the 40-entry helpers: the pass routes the solver through
 * init24/sort24 and the selection group through init_group/sort_group.
 * pair_base is the guest address count+0xA4.
 */
void wc3_sel24_init24(uint32_t pair_base);
void wc3_sel24_sort24(uint32_t count_ptr);

/*
 * Selection-group workspace wrappers used by the 16-element array in
 * L_6F16A5F8 / L_6F16A6E0.  One group entry is 0x1F4 bytes: count +0,
 * index[40] +4, pair[40] +0xA4 (key/value dwords) and tail +0x1E4..+0x1F0.
 * The wrappers do not modify the guest CPU state; under
 * WC3_DEBUG && WC3_DEBUG_SELECT24 they snapshot the entry around zeroing and
 * around the sort so a tail overwrite by the old pair stride is visible in
 * sel24_trace.txt.  The names differ from the panel-group helper
 * wc3_sel24_init_group(CPU *, uint32_t), which creates the CInfoPanelGroup.
 */
void wc3_sel24_group_init(uint32_t pair_base);
void wc3_sel24_group_sort(uint32_t group);

/*
 * Formation x cursor carry across the clusters of one point order.
 *
 * The order builder may split a mixed selection into several clusters (for
 * example 34 + 6 units), and L_6F16A5B0 lays out each cluster independently.
 * Its final x cursor is the right edge of the last laid-out row; committing
 * it and using it as the origin of the next cluster keeps the whole order in
 * one battle line instead of restarting every cluster at the base constant
 * 0x6FD3C744.  The builder handlers B/C reset the carry once per order.  The
 * 12-unit profile is the exact baseline: origin returns base and commit is a
 * no-op, so multi-cluster 12-unit behavior does not change.
 */
void wc3_sel24_formation_reset(void);
uint32_t wc3_sel24_formation_origin(uint32_t base);
void wc3_sel24_formation_commit(uint32_t x);

/*
 * L_6F16A5B0 frame-local wrappers.  The pass calls origin_apply after the
 * x cursor store and commit_frame at the group-loop exit.  The wrappers own
 * the profile-dependent [ebp-0xA50]/[ebp-0x1F50] displacement so the
 * generated C does not embed a frame offset that differs between the 24- and
 * 40-entry corpora.  They change only the guest stack local, never the CPU
 * registers or flags.
 */
void wc3_sel24_formation_origin_apply(CPU *c);
void wc3_sel24_formation_commit_frame(CPU *c);

/*
 * Formation row length returned by the table read in L_6F16D270 / L_6F16D286
 * / L_6F16D2D3.  index is the guest eax (13 * flag + count), count is the
 * group size and pair selects the table pair (0 = A/B at 0x6FA91D20,
 * 1 = C/D at 0x6FA91D88).  For count <= 12 the original table value is
 * returned unchanged, so the 12-unit profile and the exact 1.27 behaviour
 * stay bit-identical.  The table only has 13 entries per block and is
 * followed by a vtable, so the 13..40 columns are defined here: the compact
 * mode continues its constant 3, the grid mode continues the three-row shape
 * with ceil(count / 3) for 13..24 (13..15 -> 5, 16..18 -> 6, 19..21 -> 7,
 * 22..24 -> 8) and the four-row shape with ceil(count / 4) for 25..40
 * (25..28 -> 7, 29..32 -> 8, 33..36 -> 9, 37..40 -> 10).  The value is a row
 * length: the order loop clamps it to the units left, so every value >= 1
 * advances the loop.
 */
uint32_t wc3_sel24_formation_columns(uint32_t index, uint32_t count,
                                     uint32_t pair);

/*
 * Called from the CInfoPanelGroup constructor after the original 6x2 grid and
 * its 12 buttons exist.  Creates an 8x3 grid as a second child and, when the
 * runtime cap is 40, a 10x4 grid as a third child of the group.
 *
 * group+0x130 = original 6x2 grid (unchanged)
 * group+0x134 = 8x3 grid, or zero when allocation failed
 * group+0x138 = active grid (grid12 initially)
 * group+0x13C = initialisation-attempted marker ("SEL2")
 * group+0x140 = total selected-unit count from CSelectionWar3+0x1F8
 * group+0x144 = 10x4 grid, or zero when allocation failed or the structural
 *                cap is 24 (the 24-entry allocation ends at 0x144)
 *
 * The group allocation is widened from 0x134 to 0x144 (24 entries) or 0x150
 * (40 entries) bytes by the LiftPass.
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
 * the current subgroup) is known.  Selects 6x2 for <=12, 8x3 for 13..24 and
 * 10x4 for 25..40 using the total selection count in group+0x140.
 * subgroup_units is only a fallback when the total is missing or out of range
 * (a split fragment that ran without the update prologue).  If the target grid
 * is unavailable the mode stays at the largest available grid; if the mode does
 * not change the active frame is still made visible again, because a parent
 * HideAll may have unregistered it.
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
 */
void wc3_sel24_trace_evt2(CPU *c, uint32_t self, uint32_t arg);
void wc3_sel24_trace_lookup(CPU *c, uint32_t stage, uint32_t a, uint32_t b);

/*
 * wc3_sel24_trace_tasknull is called right after 0x6F6926B0 reads the task
 * payload at [factory+0x54].  The two consumers it feeds - 0x6F689FC0 and
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
 * Point-order / formation tracing added for the >12-unit ground-order defect.
 *
 * order_begin is inserted in both point-order handlers (0x6F6B991E for
 * message 0x12 and 0x6F6B9F9E for message 0x13) after the request is saved in
 * ebx.  It prints the full request, including the target pair at +0x30/+0x34
 * and the ground point at +0x28/+0x2C, so a unit-target order can be told
 * apart from a territory order.  source is 0 for the 0x12 handler and 1 for
 * the 0x13 handler.
 *
 * listadd is inserted on the 0x6F16966D `cmovne eax, esi` line, immediately
 * before the PrClGen pointer write at 0x6F169670.  It records the slot index,
 * the runtime cap, the pending pointer and the number of occupied slots,
 * catching a path that still stops at twelve.
 *
 * formation_begin is inserted after 0x6F16B306 at the entry of the ground
 * solver 0x6F16B2F0.  It prints the CPrCluster capacity, count and every
 * member unit before the index/pair workspace is touched.
 *
 * cluster_try is inserted at the entry of the member-vector insertion
 * 0x6F16C077.  It prints the vector base, grow flag, capacity and count plus
 * the caller's add count.  `over` is the exact 0x6F16C083 branch condition,
 * so a zero grow flag with `over=1` is the rejected 13th..24th member.
 *
 * cluster_insert is inserted after the member-vector insertion call at
 * 0x6F16B7ED.  Its eax result is the value the original code ignores before
 * it unlinks the unit from PrClGen; the line shows the result, the cluster
 * count and the slot pair.
 */
void wc3_sel24_trace_order_begin(CPU *c, uint32_t request, uint32_t source);
void wc3_sel24_trace_listadd(CPU *c, uint32_t list, uint32_t index,
                             uint32_t value, uint32_t unit, uint32_t store);
void wc3_sel24_trace_formation_begin(CPU *c, uint32_t cluster);
void wc3_sel24_trace_cluster_try(CPU *c, uint32_t vector, uint32_t add);
void wc3_sel24_trace_cluster_insert(CPU *c, uint32_t cluster, uint32_t unit,
                                    uint32_t list, uint32_t slot,
                                    uint32_t result);

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
void wc3_sel24_trace_apply(CPU *c, uint32_t phase, uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif /* WC3_FEATURES_SELECTION24_H */
