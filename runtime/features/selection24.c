/*
 * selection24.c - 24-unit selection and adaptive 6x2 / 8x3 unit panel.
 *
 * The lifted CInfoPanelGroup and CSelectionWar3 code stays in the generated
 * corpus.  This module only supplies the four host helpers that the
 * tools/lift/csrc_local/selection24.c pass calls from the addresses listed in
 * docs/research/SELECTION_24_ANALYSIS.md.
 *
 * Guest code is called through push32 + dispatch, never through a host call.
 * Each helper saves and restores the caller's integer registers, esp, eip and
 * flags so the generated caller sees exactly the state it expects.  x87/SSE
 * registers are caller-saved at every patch point and are not preserved here;
 * the original code at those points does not keep live values in them either.
 *
 * This is gameplay/UI code, not HLE.  No host locks, no static TLS and no
 * untagged diagnostics.
 */
#include "selection24.h"
#include "wc3_debug.h"

/* Debug-only trace.  The macro is a no-op in release, so no diagnostic call
 * survives into the product; the tag is named for the selection24 subsystem,
 * not for this file. */
#if WC3_DEBUG && WC3_DEBUG_SELECT24
#include <stdio.h>
#define SEL24_TRACE(...) fprintf(stderr, "[select24] " __VA_ARGS__)
#else
#define SEL24_TRACE(...) do { } while (0)
#endif

/* One definition in the helper library: the product build links this module
 * whenever WC3_SELECTION_24 is ON, so the marker is present exactly once. */
#if defined(__GNUC__)
__attribute__((used))
#endif
const char wc3_selection24_build_marker[] = "WC3_SELECTION_24";

/* CInfoPanelGroup fields added by the selection24 profile. */
#define SEL24_GROUP_GRID12   0x130u
#define SEL24_GROUP_GRID24   0x134u
#define SEL24_GROUP_ACTIVE   0x138u
#define SEL24_GROUP_INIT     0x13Cu
#define SEL24_GROUP_TOTAL    0x140u

/* CSelectionWar3 field with the total number of selected units. */
#define SEL24_SELECTION_TOTAL 0x1F8u

/* CSimpleGrid / CSimpleFrame fields used by the original code. */
#define SEL24_GRID_ROWS      0x124u
#define SEL24_GRID_COLS      0x128u
#define SEL24_GRID_SCALE     0x060u
#define SEL24_GRID_VISIBLE   0x090u
#define SEL24_GRID_ROWS_AT   0x154u
#define SEL24_FRAME_VISIBLE  0x090u
#define SEL24_FRAME_REGISTERED 0x094u

/* Row descriptor: 0x10 bytes, button array pointer at +8, button stride 4. */
#define SEL24_ROW_STRIDE     0x010u
#define SEL24_ROW_BUTTONS    0x008u

#define SEL24_BUTTON_VISIBLE 0x090u

/* Size of the CInfoPanelGroup allocation is widened 0x134 -> 0x144. */
#define SEL24_GROUP_SIZE     0x144u

#define SEL24_GRID_SIZE      0x15Cu
#define SEL24_GRID_LINE      0x024u
#define SEL24_BUTTON_SIZE    0x140u
#define SEL24_BUTTON_LINE    0x076u
#define SEL24_ALLOC_FILE     0x6FABF9F8u

/* Original grid12 geometry and the scaled 8x3 grid24 geometry. */
#define SEL24_GRID12_WIDTH   0x3E3E6666u
#define SEL24_GRID12_HEIGHT  0x3DD00000u
#define SEL24_GRID24_WIDTH   0x3E7DDDDDu
#define SEL24_GRID24_HEIGHT  0x3E0AAAABu
#define SEL24_GRID24_OFFSET_X 0xBB088889u
#define SEL24_GRID24_OFFSET_Y 0x00000000u
#define SEL24_GRID24_SCALE   0x3F400000u /* 0.75f */

#define SEL24_GRID24_ROWS    3u
#define SEL24_GRID24_COLS    8u
#define SEL24_GRID12_ROWS    2u
#define SEL24_GRID12_COLS    6u

/* Guest entry points.  All are regular lifted functions. */
#define SEL24_ALLOC          0x6F07C6D2u
#define SEL24_GRID_CTOR      0x6F10E230u
#define SEL24_GRID_RESIZE    0x6F10EE50u
#define SEL24_GRID_INSERT    0x6F10E490u
#define SEL24_GRID_WIDTH     0x6F1116C0u
#define SEL24_GRID_HEIGHT    0x6F111520u
#define SEL24_GRID_ANCHOR    0x6F111600u
#define SEL24_BUTTON_CTOR    0x6F39EDC0u
#define SEL24_FILL_CELL      0x6F3DA470u
#define SEL24_FRAME_HIDE     0x6F10D410u
#define SEL24_FRAME_SHOW     0x6F10DE60u

/* Real return addresses from the corresponding original call sites. */
#define SEL24_RET_GRID_ALLOC 0x6F39ED29u
#define SEL24_RET_BUTTON_ALLOC 0x6F3B91F5u
#define SEL24_RET_GRID_CTOR  0x6F39ED3Fu
#define SEL24_RET_WIDTH      0x6F39ED5Cu
#define SEL24_RET_HEIGHT     0x6F39ED6Fu
#define SEL24_RET_ANCHOR     0x6F39ED93u
#define SEL24_RET_RESIZE     0x6F39EDA2u
#define SEL24_RET_BUTTON     0x6F3B9213u
#define SEL24_RET_INSERT     0x6F3B922Eu
#define SEL24_RET_FILL       0x6F3C7E99u
#define SEL24_RET_HIDE       0x6F3ACCE2u
#define SEL24_RET_SHOW       0x6F10DEC4u

/* The group is allocated without zeroing, so the marker is a magic value.
 * A fresh object is initialised once; the marker prevents a second grid24
 * from being created (or a failed allocation from being retried) if the tail
 * is reached twice. */
#define SEL24_INIT_MAGIC     0x53454C32u /* "SEL2" */

/* Vtable slots of CSimpleFrame. */
#define SEL24_VT_HIDE        0x064u
#define SEL24_VT_SHOW        0x068u

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eip;
    uint32_t cf, zf, sf, of, pf, af;
} Sel24CpuState;

static void sel24_save_state(CPU *c, Sel24CpuState *s)
{
    s->eax = c->eax;
    s->ecx = c->ecx;
    s->edx = c->edx;
    s->ebx = c->ebx;
    s->esp = c->esp;
    s->ebp = c->ebp;
    s->esi = c->esi;
    s->edi = c->edi;
    s->eip = c->eip;
    s->cf = c->cf;
    s->zf = c->zf;
    s->sf = c->sf;
    s->of = c->of;
    s->pf = c->pf;
    s->af = c->af;
}

static void sel24_restore_state(CPU *c, const Sel24CpuState *s)
{
    c->eax = s->eax;
    c->ecx = s->ecx;
    c->edx = s->edx;
    c->ebx = s->ebx;
    c->esp = s->esp;
    c->ebp = s->ebp;
    c->esi = s->esi;
    c->edi = s->edi;
    c->eip = s->eip;
    c->cf = s->cf;
    c->zf = s->zf;
    c->sf = s->sf;
    c->of = s->of;
    c->pf = s->pf;
    c->af = s->af;
}

#if WC3_DEBUG && WC3_DEBUG_SELECT24
/*
 * The LiftPass inserts this call immediately after the original AddUnit cap
 * comparison.  The following `jae` still needs the flags, so the trace saves
 * and restores the full integer CPU state before printing.
 */
void wc3_sel24_trace_addunit(CPU *c, uint32_t group, uint32_t unit,
                             uint32_t count)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    fprintf(stderr,
            "[select24] addunit this=%08X unit=%08X count=%u\n",
            group, unit, count);
    sel24_restore_state(c, &saved);
}
#endif

/*
 * Call one lifted guest function.  args[] is in C order (leftmost first) and
 * is pushed right-to-left.  The return value is c->eax after the call.  The
 * caller's full integer state is restored, including esp, so a callee that
 * cleans its arguments (stdcall) and one that does not (cdecl) behave the
 * same way here.
 */
static uint32_t sel24_invoke(CPU *c, uint32_t target, uint32_t ecx,
                             uint32_t ret, const uint32_t *args,
                             unsigned count)
{
    Sel24CpuState saved;
    uint32_t result;
    unsigned i;

    sel24_save_state(c, &saved);
    c->ecx = ecx;
    for (i = count; i > 0u; --i) {
        push32(c, args[i - 1u]);
    }
    push32(c, ret);
    dispatch(c, target);
    result = c->eax;
    sel24_restore_state(c, &saved);
    return result;
}

static uint32_t sel24_alloc(CPU *c, uint32_t size, uint32_t line,
                            uint32_t ret)
{
    uint32_t args[4];

    args[0] = size;
    args[1] = SEL24_ALLOC_FILE;
    args[2] = line;
    args[3] = 0u;
    return sel24_invoke(c, SEL24_ALLOC, 0u, ret, args, 4u);
}

static void sel24_grid_ctor(CPU *c, uint32_t grid, uint32_t parent)
{
    uint32_t args[1];

    args[0] = parent;
    (void)sel24_invoke(c, SEL24_GRID_CTOR, grid, SEL24_RET_GRID_CTOR,
                       args, 1u);
}

static void sel24_grid_resize(CPU *c, uint32_t grid, uint32_t rows,
                              uint32_t cols)
{
    uint32_t args[2];

    args[0] = rows;
    args[1] = cols;
    (void)sel24_invoke(c, SEL24_GRID_RESIZE, grid, SEL24_RET_RESIZE,
                       args, 2u);
}

static void sel24_grid_insert(CPU *c, uint32_t grid, uint32_t col,
                              uint32_t row, uint32_t button)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = button;
    args[3] = 1u; /* flag: SetParent was already done by the button ctor */
    (void)sel24_invoke(c, SEL24_GRID_INSERT, grid, SEL24_RET_INSERT,
                       args, 4u);
}

static void sel24_grid_width(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel24_invoke(c, SEL24_GRID_WIDTH, grid, SEL24_RET_WIDTH, args, 1u);
}

static void sel24_grid_height(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel24_invoke(c, SEL24_GRID_HEIGHT, grid, SEL24_RET_HEIGHT,
                       args, 1u);
}

static void sel24_grid_anchor(CPU *c, uint32_t grid, uint32_t child_anchor,
                              uint32_t parent, uint32_t parent_anchor,
                              uint32_t x, uint32_t y, uint32_t flag)
{
    uint32_t args[6];

    args[0] = child_anchor;
    args[1] = parent;
    args[2] = parent_anchor;
    args[3] = x;
    args[4] = y;
    args[5] = flag;
    (void)sel24_invoke(c, SEL24_GRID_ANCHOR, grid, SEL24_RET_ANCHOR,
                       args, 6u);
}

static void sel24_button_ctor(CPU *c, uint32_t button, uint32_t grid)
{
    uint32_t args[1];

    args[0] = grid;
    (void)sel24_invoke(c, SEL24_BUTTON_CTOR, button, SEL24_RET_BUTTON,
                       args, 1u);
}

static void sel24_fill_cell(CPU *c, uint32_t group, uint32_t col,
                            uint32_t row, uint32_t unit, uint32_t flag)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = unit;
    args[3] = flag;
    (void)sel24_invoke(c, SEL24_FILL_CELL, group, SEL24_RET_FILL, args, 4u);
}

static void sel24_frame_method(CPU *c, uint32_t frame, uint32_t slot,
                               uint32_t ret, uint32_t visible)
{
    uint32_t vtable;
    uint32_t method;

    if (!frame) {
        return;
    }
    wr32(frame + SEL24_FRAME_VISIBLE, visible);
    vtable = rd32(frame);
    if (!vtable) {
        return;
    }
    method = rd32(vtable + slot);
    if (method) {
        (void)sel24_invoke(c, method, frame, ret, NULL, 0u);
    }
}

static uint32_t sel24_valid_grid(uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t active;

    if (!group) {
        return 0u;
    }
    /* Before init the added fields are outside the original 0x134 allocation
     * and may contain garbage; the original grid12 pointer is still valid. */
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return rd32(group + SEL24_GROUP_GRID12);
    }
    grid12 = rd32(group + SEL24_GROUP_GRID12);
    grid24 = rd32(group + SEL24_GROUP_GRID24);
    active = rd32(group + SEL24_GROUP_ACTIVE);
    if (active == grid12 || (grid24 && active == grid24)) {
        return active;
    }
    return grid12;
}

/*
 * Hide every real button of one grid without touching the grid frame.  The
 * original group hide-all assumes all 12 cells exist; the 24-cell grid can
 * contain null cells after a partial allocation, so every level is checked.
 */
static void sel24_hide_grid(CPU *c, uint32_t grid)
{
    uint32_t rows;
    uint32_t cols;
    uint32_t row_array;
    uint32_t row;
    uint32_t col;

    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL24_GRID_ROWS);
    cols = rd32(grid + SEL24_GRID_COLS);
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array || rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }

    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL24_ROW_STRIDE;
        uint32_t buttons = rd32(row_desc + SEL24_ROW_BUTTONS);
        if (!buttons) {
            continue;
        }
        for (col = 0u; col < cols; ++col) {
            uint32_t button = rd32(buttons + col * 4u);
            if (button) {
                sel24_frame_method(c, button, SEL24_VT_HIDE, SEL24_RET_HIDE,
                                   0u);
            }
        }
    }
}

/*
 * Make sure the active grid frame is registered again after a parent
 * HideAll/ShowAll cycle.  Buttons are re-hidden first: ShowAll recurses into
 * children that have their own visible flag set, and stale buttons must not
 * survive an update that only fills part of the grid.
 */
static void sel24_ensure_visible(CPU *c, uint32_t grid)
{
    if (!grid) {
        return;
    }
    if (rd32(grid + SEL24_FRAME_VISIBLE) == 0u ||
        rd32(grid + SEL24_FRAME_REGISTERED) == 0u) {
        sel24_hide_grid(c, grid);
        sel24_frame_method(c, grid, SEL24_VT_SHOW, SEL24_RET_SHOW, 1u);
    }
}

/*
 * Check that the resize really produced the requested rows/cols and that
 * every row has a button array.  This keeps the helper fail-closed when the
 * grid or a row array could not be allocated.
 */
static int sel24_grid_ready(uint32_t grid, uint32_t rows, uint32_t cols)
{
    uint32_t row_array;
    uint32_t row;

    if (!grid) {
        return 0;
    }
    if (rd32(grid + SEL24_GRID_ROWS) != rows ||
        rd32(grid + SEL24_GRID_COLS) != cols) {
        return 0;
    }
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array) {
        return 0;
    }
    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL24_ROW_STRIDE;
        if (!rd32(row_desc + SEL24_ROW_BUTTONS)) {
            return 0;
        }
    }
    return 1;
}

void wc3_sel24_init_group(CPU *c, uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t row;
    uint32_t col;

    if (!group) {
        return;
    }
    if (rd32(group + SEL24_GROUP_INIT) == SEL24_INIT_MAGIC) {
        return;
    }

    grid12 = rd32(group + SEL24_GROUP_GRID12);
    wr32(group + SEL24_GROUP_INIT, SEL24_INIT_MAGIC);
    wr32(group + SEL24_GROUP_GRID24, 0u);
    wr32(group + SEL24_GROUP_ACTIVE, grid12);
    wr32(group + SEL24_GROUP_TOTAL, 0u);
    SEL24_TRACE("init group=%08X grid12=%08X marker=%08X\n",
                group, grid12, (unsigned)SEL24_INIT_MAGIC);
    if (!grid12) {
        return;
    }

    grid24 = sel24_alloc(c, SEL24_GRID_SIZE, SEL24_GRID_LINE,
                         SEL24_RET_GRID_ALLOC);
    SEL24_TRACE("init group=%08X grid24=%08X\n", group, grid24);
    if (!grid24) {
        return;
    }

    sel24_grid_ctor(c, grid24, group);
    wr32(grid24 + SEL24_GRID_SCALE, SEL24_GRID24_SCALE);
    sel24_grid_width(c, grid24, SEL24_GRID24_WIDTH);
    sel24_grid_height(c, grid24, SEL24_GRID24_HEIGHT);
    sel24_grid_anchor(c, grid24, 7u, group, 7u,
                      SEL24_GRID24_OFFSET_X, SEL24_GRID24_OFFSET_Y, 1u);
    sel24_grid_resize(c, grid24, SEL24_GRID24_ROWS, SEL24_GRID24_COLS);
    SEL24_TRACE("init group=%08X grid24=%08X ready=%d rows=%u cols=%u\n",
                group, grid24,
                sel24_grid_ready(grid24, SEL24_GRID24_ROWS,
                                 SEL24_GRID24_COLS),
                (unsigned)SEL24_GRID24_ROWS, (unsigned)SEL24_GRID24_COLS);

    if (!sel24_grid_ready(grid24, SEL24_GRID24_ROWS, SEL24_GRID24_COLS)) {
        /* The partial grid stays a hidden child of the group.  It is not
         * referenced by the group fields, so the 6x2 path remains intact. */
        wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
        sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
        sel24_hide_grid(c, grid24);
        wr32(group + SEL24_GROUP_GRID24, 0u);
        wr32(group + SEL24_GROUP_ACTIVE, grid12);
        return;
    }

    wr32(group + SEL24_GROUP_GRID24, grid24);
    wr32(group + SEL24_GROUP_ACTIVE, grid12);

    for (row = 0u; row < SEL24_GRID24_ROWS; ++row) {
        for (col = 0u; col < SEL24_GRID24_COLS; ++col) {
            uint32_t button = sel24_alloc(c, SEL24_BUTTON_SIZE,
                                          SEL24_BUTTON_LINE,
                                          SEL24_RET_BUTTON_ALLOC);
            if (!button) {
                wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
                sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE,
                                   0u);
                sel24_hide_grid(c, grid24);
                wr32(group + SEL24_GROUP_GRID24, 0u);
                wr32(group + SEL24_GROUP_ACTIVE, grid12);
                return;
            }
            sel24_button_ctor(c, button, grid24);
            sel24_grid_insert(c, grid24, col, row, button);
        }
    }

    /* The 8x3 grid is a dormant child until the first 13..24 update. */
    wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
    sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
    sel24_hide_grid(c, grid24);
    wr32(grid12 + SEL24_GRID_VISIBLE, 1u);
}

/*
 * Called from the update prologue with the CSelectionWar3 object.  The full
 * update function and the split entry fragments share the same stack frame,
 * but only the prologue has the selection pointer.  Keep the total count in
 * the group object so the mode choice works in every fragment.
 */
void wc3_sel24_note_selection(CPU *c, uint32_t group, uint32_t selection)
{
    uint32_t total = 0u;

    (void)c;
    if (!group) {
        return;
    }
    SEL24_TRACE("note group=%08X marker=%08X grid12=%08X selection=%08X\n",
                group, rd32(group + SEL24_GROUP_INIT),
                rd32(group + SEL24_GROUP_GRID12), selection);
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return;
    }
    /* init only writes the marker after it has seen a real grid12; requiring
     * the same pointer here keeps a chance marker match on a short allocation
     * from turning into an out-of-bounds write at +0x140. */
    if (!rd32(group + SEL24_GROUP_GRID12)) {
        return;
    }
    if (selection) {
        total = rd32(selection + SEL24_SELECTION_TOTAL);
    }
    SEL24_TRACE("note group=%08X selection=%08X total=%u stored=%u\n",
                group, selection, total, (unsigned)SEL24_SELECTION_TOTAL);
    wr32(group + SEL24_GROUP_TOTAL, total);
}

void wc3_sel24_select(CPU *c, uint32_t group, uint32_t subgroup_units)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t desired;
    uint32_t old;
    uint32_t total;

    if (!group) {
        return;
    }
    SEL24_TRACE("select group=%08X marker=%08X subgroup=%u\n",
                group, rd32(group + SEL24_GROUP_INIT), subgroup_units);
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return;
    }
    grid12 = rd32(group + SEL24_GROUP_GRID12);
    grid24 = rd32(group + SEL24_GROUP_GRID24);
    if (!grid12) {
        return;
    }

    total = rd32(group + SEL24_GROUP_TOTAL);
    if (total == 0u || total > 24u) {
        /* The split-entry fragment may run without the update prologue.  The
         * current subgroup's unit count is a safe lower bound: it never asks
         * for more cells than this fragment can fill. */
        total = subgroup_units;
    }

    desired = (total > SEL24_GRID12_ROWS * SEL24_GRID12_COLS && grid24)
                  ? grid24
                  : grid12;

    old = rd32(group + SEL24_GROUP_ACTIVE);
    if (old != grid12 && (grid24 == 0u || old != grid24)) {
        old = grid12;
        wr32(group + SEL24_GROUP_ACTIVE, old);
    }
    SEL24_TRACE("select group=%08X subgroup=%u total=%u grid12=%08X "
                "grid24=%08X active=%08X desired=%08X\n",
                group, subgroup_units, total, grid12, grid24, old, desired);
    if (desired == old) {
        /* A parent HideAll can unregister the active grid; ShowAll would not
         * restore it because its own visible flag is zero. */
        sel24_ensure_visible(c, desired);
        return;
    }

    /* Remove stale buttons from the target grid first, then swap frames.
     * The fill path that follows shows only the buttons it actually fills. */
    sel24_hide_grid(c, desired);
    sel24_frame_method(c, old, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
    sel24_frame_method(c, desired, SEL24_VT_SHOW, SEL24_RET_SHOW, 1u);
    wr32(group + SEL24_GROUP_ACTIVE, desired);
}

void wc3_sel24_hide_buttons(CPU *c, uint32_t group)
{
    uint32_t grid = sel24_valid_grid(group);

    SEL24_TRACE("hide group=%08X grid=%08X\n", group, grid);
    if (grid) {
        sel24_hide_grid(c, grid);
    }
}

void wc3_sel24_fill(CPU *c, uint32_t group, uint32_t index,
                    uint32_t unit, uint32_t flag)
{
    uint32_t grid;
    uint32_t rows;
    uint32_t cols;
    uint32_t row_array;
    uint32_t row;
    uint32_t col;
    uint32_t row_desc;
    uint32_t buttons;
    uint32_t button;

    if (!group) {
        return;
    }
    grid = sel24_valid_grid(group);
    SEL24_TRACE("fill group=%08X index=%u grid=%08X\n", group, index, grid);
    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL24_GRID_ROWS);
    cols = rd32(grid + SEL24_GRID_COLS);
    if (rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }
    if (index >= rows * cols) {
        return;
    }

    row = index / cols;
    col = index % cols;
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array) {
        return;
    }
    row_desc = row_array + row * SEL24_ROW_STRIDE;
    buttons = rd32(row_desc + SEL24_ROW_BUTTONS);
    if (!buttons) {
        return;
    }
    button = rd32(buttons + col * 4u);
    if (!button) {
        SEL24_TRACE("fill group=%08X index=%u button=0\n", group, index);
        return;
    }

    SEL24_TRACE("fill group=%08X index=%u unit=%08X flag=%u grid=%08X "
                "rows=%u cols=%u row=%u col=%u button=%08X\n",
                group, index, unit, flag, grid, rows, cols, row, col, button);
    sel24_fill_cell(c, group, col, row, unit, flag);
}
