/*
 * gdi_obj_hle.c - target-side GDI32 region, pen and line state.
 *
 * Regions are stored as a normalised list of axis-aligned rectangles.  The
 * imported constructors only make rectangles, but CombineRgn can produce
 * L-shaped and holed results; the set operations below keep those exact for
 * the subsequent RectInRegion/CombineRgn calls.
 *
 * Pens and the per-DC current position are observable state.  Actual pixels
 * are drawn by the D3D9 path, so FillRgn/LineTo/Rectangle validate their
 * handles and update position without rasterising into a GDI surface.
 */
#include "gdi_obj_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"

#define REGION_HANDLE_BASE 0x000F0000u
#define REGION_HANDLE_STRIDE 4u
#define REGION_COUNT 64u
#define REGION_RECT_MAX 256u

#define PEN_HANDLE_BASE 0x00100000u
#define PEN_HANDLE_STRIDE 4u
#define PEN_COUNT 64u

#define DC_POS_COUNT 64u

#define RGN_AND 1u
#define RGN_OR 2u
#define RGN_XOR 3u
#define RGN_DIFF 4u
#define RGN_COPY 5u

#define RGN_ERROR 0u
#define RGN_NULL 1u
#define RGN_SIMPLE 2u
#define RGN_COMPLEX 3u

typedef struct GdiRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} GdiRect;

typedef struct GdiRegion {
    uint8_t used;
    uint32_t handle;
    uint16_t count;
    GdiRect rects[REGION_RECT_MAX];
} GdiRegion;

typedef struct GdiPen {
    uint8_t used;
    uint32_t handle;
    uint32_t style;
    uint32_t width;
    uint32_t color;
} GdiPen;

typedef struct GdiDcPos {
    uint8_t used;
    uint32_t hdc;
    int32_t x;
    int32_t y;
} GdiDcPos;

static GdiRegion g_regions[REGION_COUNT];
static GdiPen g_pens[PEN_COUNT];
static GdiDcPos g_positions[DC_POS_COUNT];

/*
 * Regions, pens and DC positions each use a table mutex for identity
 * (used/handle or used/hdc and first-free selection) and one mutex per slot
 * for payload.  A handler pins the slot under the table mutex, releases the
 * table mutex, and touches payload only under the slot mutex; slot holders
 * never acquire a table mutex.  Region creation keeps the table lock through
 * its single add so a failed add can clear identity under the table lock.
 * CombineRgn pins its unique participating region slots in ascending order
 * under the table lock and releases them in reverse.  Runtime nested orders
 * are region table -> pen table (h_fill_rgn), pen table -> pen slot
 * (create/delete), position table -> position slot (MoveToEx/LineTo) and
 * region table -> region slot (create/delete/RectInRegion/CombineRgn).
 * Install takes the three table locks in the order region -> pen -> position
 * before registration.  No guest memory is accessed while any mutex is held.
 */
static pthread_mutex_t g_region_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pen_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_position_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_region_locks[REGION_COUNT];
static pthread_mutex_t g_pen_locks[PEN_COUNT];
static pthread_mutex_t g_position_locks[DC_POS_COUNT];

/*
 * Slot mutexes are initialised once, before any handler is registered.
 * g_slot_locks_ready is written by the once routine and read after
 * pthread_once() returns; a failed init makes install refuse to register.
 */
static pthread_once_t g_slot_locks_once = PTHREAD_ONCE_INIT;
static int g_slot_locks_ready;

static void init_slot_locks(void)
{
    int ready = 1;

    for (unsigned i = 0; i < REGION_COUNT; i++) {
        if (pthread_mutex_init(&g_region_locks[i], NULL) != 0)
            ready = 0;
    }
    for (unsigned i = 0; i < PEN_COUNT; i++) {
        if (pthread_mutex_init(&g_pen_locks[i], NULL) != 0)
            ready = 0;
    }
    for (unsigned i = 0; i < DC_POS_COUNT; i++) {
        if (pthread_mutex_init(&g_position_locks[i], NULL) != 0)
            ready = 0;
    }
    g_slot_locks_ready = ready;
}

static void normalize_rect(GdiRect *rect)
{
    if (rect->left > rect->right) {
        const int32_t swap = rect->left;
        rect->left = rect->right;
        rect->right = swap;
    }
    if (rect->top > rect->bottom) {
        const int32_t swap = rect->top;
        rect->top = rect->bottom;
        rect->bottom = swap;
    }
}

static int rect_empty(const GdiRect *rect)
{
    return !rect || rect->left >= rect->right || rect->top >= rect->bottom;
}

static int rect_equal(const GdiRect *a, const GdiRect *b)
{
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

static int rect_contains(const GdiRect *outer, const GdiRect *inner)
{
    return outer->left <= inner->left && outer->top <= inner->top &&
           outer->right >= inner->right && outer->bottom >= inner->bottom;
}

static int rect_intersect(const GdiRect *a, const GdiRect *b, GdiRect *out)
{
    out->left = a->left > b->left ? a->left : b->left;
    out->top = a->top > b->top ? a->top : b->top;
    out->right = a->right < b->right ? a->right : b->right;
    out->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    return !rect_empty(out);
}

static int rect_can_merge(const GdiRect *a, const GdiRect *b)
{
    if (a->top == b->top && a->bottom == b->bottom) {
        return a->right >= b->left && b->right >= a->left;
    }
    if (a->left == b->left && a->right == b->right) {
        return a->bottom >= b->top && b->bottom >= a->top;
    }
    return 0;
}

static GdiRect rect_union(const GdiRect *a, const GdiRect *b)
{
    GdiRect out;
    out.left = a->left < b->left ? a->left : b->left;
    out.top = a->top < b->top ? a->top : b->top;
    out.right = a->right > b->right ? a->right : b->right;
    out.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return out;
}

/* Region payload helpers operate on rects/count.  Callers must hold the slot
 * lock for every g_regions[] object passed in; stack temporaries need none. */
static void region_remove_at(GdiRegion *region, unsigned index)
{
    if (index + 1u < region->count) {
        memmove(&region->rects[index], &region->rects[index + 1u],
                (region->count - index - 1u) * sizeof region->rects[0]);
    }
    region->count--;
}

static void region_compact(GdiRegion *region)
{
    int changed = 1;

    while (changed) {
        changed = 0;
        for (unsigned i = 0; i < region->count; i++) {
            if (!rect_empty(&region->rects[i])) continue;
            region_remove_at(region, i);
            changed = 1;
            break;
        }
        if (changed) continue;

        for (unsigned i = 0; i < region->count && !changed; i++) {
            for (unsigned j = i + 1u; j < region->count; j++) {
                GdiRect *a = &region->rects[i];
                GdiRect *b = &region->rects[j];
                if (rect_equal(a, b) || rect_contains(a, b)) {
                    region_remove_at(region, j);
                    changed = 1;
                    break;
                }
                if (rect_contains(b, a)) {
                    region_remove_at(region, i);
                    changed = 1;
                    break;
                }
                if (rect_can_merge(a, b)) {
                    *a = rect_union(a, b);
                    region_remove_at(region, j);
                    changed = 1;
                    break;
                }
            }
        }
    }
}

static int region_add(GdiRegion *region, const GdiRect *rect)
{
    if (rect_empty(rect)) return 1;
    if (region->count >= REGION_RECT_MAX) return 0;
    region->rects[region->count++] = *rect;
    return 1;
}

static int region_copy(GdiRegion *dst, const GdiRegion *src)
{
    if (src->count > REGION_RECT_MAX) return 0;
    dst->count = src->count;
    if (src->count)
        memcpy(dst->rects, src->rects, src->count * sizeof dst->rects[0]);
    return 1;
}

static int region_union_op(GdiRegion *dst, const GdiRegion *a,
                           const GdiRegion *b)
{
    if (!region_copy(dst, a)) return 0;
    for (unsigned i = 0; i < b->count; i++) {
        if (!region_add(dst, &b->rects[i])) return 0;
    }
    region_compact(dst);
    return 1;
}

static int region_intersection_op(GdiRegion *dst, const GdiRegion *a,
                                  const GdiRegion *b)
{
    dst->count = 0;
    for (unsigned i = 0; i < a->count; i++) {
        for (unsigned j = 0; j < b->count; j++) {
            GdiRect intersection;
            if (!rect_intersect(&a->rects[i], &b->rects[j], &intersection))
                continue;
            if (!region_add(dst, &intersection)) return 0;
        }
    }
    region_compact(dst);
    return 1;
}

static int region_subtract_rect(GdiRegion *region, const GdiRect *cut)
{
    GdiRegion next;
    memset(&next, 0, sizeof next);

    for (unsigned i = 0; i < region->count; i++) {
        const GdiRect *rect = &region->rects[i];
        GdiRect intersection;
        if (!rect_intersect(rect, cut, &intersection)) {
            if (!region_add(&next, rect)) return 0;
            continue;
        }

        GdiRect strip;
        strip = *rect;
        strip.right = intersection.left;
        if (!region_add(&next, &strip)) return 0;

        strip = *rect;
        strip.left = intersection.right;
        if (!region_add(&next, &strip)) return 0;

        strip.left = intersection.left;
        strip.right = intersection.right;
        strip.bottom = intersection.top;
        if (!region_add(&next, &strip)) return 0;

        strip.top = intersection.bottom;
        strip.bottom = rect->bottom;
        if (!region_add(&next, &strip)) return 0;
    }

    region_compact(&next);
    region->count = next.count;
    if (next.count)
        memcpy(region->rects, next.rects,
               next.count * sizeof region->rects[0]);
    return 1;
}

static int region_subtract_op(GdiRegion *dst, const GdiRegion *a,
                              const GdiRegion *b)
{
    if (!region_copy(dst, a)) return 0;
    for (unsigned i = 0; i < b->count; i++) {
        if (!region_subtract_rect(dst, &b->rects[i])) return 0;
    }
    return 1;
}

/* Caller must hold g_region_table_lock.  Identity-only lookup. */
static GdiRegion *find_region(uint32_t handle)
{
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        if (g_regions[i].used && g_regions[i].handle == handle)
            return &g_regions[i];
    }
    return NULL;
}

/* Caller must hold g_region_table_lock.  Initialises identity only; the
 * caller must pin g_region_locks[slot] and clear count/rects under the slot
 * lock. */
static GdiRegion *new_region(uint32_t *handle_out)
{
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        if (g_regions[i].used) continue;
        g_regions[i].used = 1u;
        g_regions[i].handle = REGION_HANDLE_BASE + i * REGION_HANDLE_STRIDE;
        *handle_out = g_regions[i].handle;
        return &g_regions[i];
    }
    return NULL;
}

/* Caller must hold g_region_table_lock.  Inserts one slot index into a unique
 * ascending set of at most three; CombineRgn then pins the set in order. */
static void add_region_slot_ascending(const GdiRegion *region,
                                      unsigned *slots,
                                      unsigned *slot_count)
{
    const unsigned slot = (unsigned)(region - g_regions);
    unsigned at = 0u;

    while (at < *slot_count && slots[at] < slot)
        at++;
    if (at < *slot_count && slots[at] == slot)
        return;
    for (unsigned i = *slot_count; i > at; i--)
        slots[i] = slots[i - 1u];
    slots[at] = slot;
    (*slot_count)++;
}

/* Caller must hold g_pen_table_lock.  If payload is needed, the caller must
 * pin g_pen_locks[slot] while still holding the table lock, then release the
 * table lock before touching style/width/color. */
static GdiPen *find_pen(uint32_t handle)
{
    for (unsigned i = 0; i < PEN_COUNT; i++) {
        if (g_pens[i].used && g_pens[i].handle == handle)
            return &g_pens[i];
    }
    return NULL;
}

/* Caller must hold g_pen_table_lock.  Initialises identity only; the caller
 * must pin g_pen_locks[slot] before releasing the table lock, then write
 * style/width/color under the slot lock. */
static GdiPen *new_pen(uint32_t *handle_out)
{
    for (unsigned i = 0; i < PEN_COUNT; i++) {
        if (g_pens[i].used) continue;
        g_pens[i].used = 1u;
        g_pens[i].handle = PEN_HANDLE_BASE + i * PEN_HANDLE_STRIDE;
        *handle_out = g_pens[i].handle;
        return &g_pens[i];
    }
    return NULL;
}

static int is_region_handle(uint32_t handle)
{
    return handle >= REGION_HANDLE_BASE &&
           handle < REGION_HANDLE_BASE + REGION_COUNT * REGION_HANDLE_STRIDE;
}

static int is_pen_handle(uint32_t handle)
{
    return handle >= PEN_HANDLE_BASE &&
           handle < PEN_HANDLE_BASE + PEN_COUNT * PEN_HANDLE_STRIDE;
}

static int32_t read_i32(uint32_t address)
{
    return (int32_t)wc3_legacy_rd32g(address);
}

static int read_guest_rect(uint32_t address, GdiRect *rect)
{
    if (!rect || !address || !wc3_legacy_guest_ok(address, 16u)) return 0;
    rect->left = read_i32(address);
    rect->top = read_i32(address + 4u);
    rect->right = read_i32(address + 8u);
    rect->bottom = read_i32(address + 12u);
    normalize_rect(rect);
    return 1;
}

/* Creates one rectangle region.  The table lock publishes identity and pins
 * the new slot; the payload is then initialised under the slot lock only.
 * region_add() writes the first rectangle, so only the unused tail needs
 * clearing.  A fresh count=0 slot cannot fail; the identity-only retirement
 * keeps a defensive path without clearing payload under the table lock.
 * Returns 0 on failure.  No guest access. */
static uint32_t create_rect_region(const GdiRect *rect)
{
    uint32_t handle = 0u;
    GdiRegion *region;
    unsigned slot = 0u;

    pthread_mutex_lock(&g_region_table_lock);
    region = new_region(&handle);
    if (region) {
        slot = (unsigned)(region - g_regions);
        pthread_mutex_lock(&g_region_locks[slot]);

        region->count = 0u;
        if (region_add(region, rect)) {
            pthread_mutex_unlock(&g_region_table_lock);
            if (region->count < REGION_RECT_MAX)
                memset(region->rects + region->count, 0,
                       (REGION_RECT_MAX - region->count) * sizeof region->rects[0]);
            pthread_mutex_unlock(&g_region_locks[slot]);
            return handle;
        }

        /* Unreachable for a fresh region; retire identity under both locks
         * instead of zeroing the payload.  Reuse initialises it again. */
        region->used = 0u;
        region->handle = 0u;
        pthread_mutex_unlock(&g_region_locks[slot]);
    }
    pthread_mutex_unlock(&g_region_table_lock);
    return 0u;
}

static void h_create_rect_rgn(CPU *c)
{
    GdiRect rect = {
        (int32_t)wc3_legacy_arg32(c, 0u),
        (int32_t)wc3_legacy_arg32(c, 1u),
        (int32_t)wc3_legacy_arg32(c, 2u),
        (int32_t)wc3_legacy_arg32(c, 3u)
    };
    normalize_rect(&rect);
    c->eax = create_rect_region(&rect);
}

static void h_create_rect_rgn_indirect(CPU *c)
{
    const uint32_t address = wc3_legacy_arg32(c, 0u);
    GdiRect rect;
    if (!read_guest_rect(address, &rect)) {
        c->eax = 0u;
        return;
    }

    c->eax = create_rect_region(&rect);
}

static void h_combine_rgn(CPU *c)
{
    const uint32_t dest_handle = wc3_legacy_arg32(c, 0u);
    const uint32_t src1_handle = wc3_legacy_arg32(c, 1u);
    const uint32_t src2_handle = wc3_legacy_arg32(c, 2u);
    const uint32_t mode = wc3_legacy_arg32(c, 3u);
    unsigned slots[3];
    unsigned slot_count = 0u;
    GdiRegion *dest;
    GdiRegion *src1;
    GdiRegion *src2;
    GdiRegion result;
    int ok = 0;
    uint32_t classification = RGN_ERROR;

    pthread_mutex_lock(&g_region_table_lock);
    dest = find_region(dest_handle);
    src1 = find_region(src1_handle);
    src2 = mode == RGN_COPY ? NULL : find_region(src2_handle);
    if (!dest || !src1 || (mode != RGN_COPY && !src2)) {
        pthread_mutex_unlock(&g_region_table_lock);
        c->eax = RGN_ERROR;
        return;
    }

    /* Pin every unique participant in ascending slot order while the table
     * lock is still held; no table lock is taken after this point. */
    add_region_slot_ascending(dest, slots, &slot_count);
    add_region_slot_ascending(src1, slots, &slot_count);
    if (src2)
        add_region_slot_ascending(src2, slots, &slot_count);
    for (unsigned i = 0; i < slot_count; i++)
        pthread_mutex_lock(&g_region_locks[slots[i]]);
    pthread_mutex_unlock(&g_region_table_lock);

    memset(&result, 0, sizeof result);
    switch (mode) {
    case RGN_AND:
        ok = region_intersection_op(&result, src1, src2);
        break;
    case RGN_OR:
        ok = region_union_op(&result, src1, src2);
        break;
    case RGN_XOR: {
        GdiRegion left, right;
        memset(&left, 0, sizeof left);
        memset(&right, 0, sizeof right);
        ok = region_subtract_op(&left, src1, src2) &&
             region_subtract_op(&right, src2, src1) &&
             region_union_op(&result, &left, &right);
        break;
    }
    case RGN_DIFF:
        ok = region_subtract_op(&result, src1, src2);
        break;
    case RGN_COPY:
        ok = region_copy(&result, src1);
        break;
    default:
        break;
    }

    if (!ok) {
        for (unsigned i = slot_count; i > 0u; i--)
            pthread_mutex_unlock(&g_region_locks[slots[i - 1u]]);
        c->eax = RGN_ERROR;
        return;
    }

    dest->count = result.count;
    if (result.count)
        memcpy(dest->rects, result.rects,
               result.count * sizeof dest->rects[0]);
    classification = dest->count == 0u ? RGN_NULL
                   : dest->count == 1u ? RGN_SIMPLE
                                       : RGN_COMPLEX;
    for (unsigned i = slot_count; i > 0u; i--)
        pthread_mutex_unlock(&g_region_locks[slots[i - 1u]]);
    c->eax = classification;
}

static void h_fill_rgn(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const uint32_t region_handle = wc3_legacy_arg32(c, 1u);
    const uint32_t brush = wc3_legacy_arg32(c, 2u);
    int region_ok;
    int brush_ok = 1;

    /*
     * The only cross-table order in this file is region table -> pen table.
     * The region table lock is held for both region lookups; the pen table
     * lock is taken only for a pen brush and never in the opposite order.
     * FillRgn needs existence only, so it takes no region or pen slot lock.
     */
    pthread_mutex_lock(&g_region_table_lock);
    region_ok = find_region(region_handle) != NULL;
    if (is_region_handle(brush)) {
        brush_ok = find_region(brush) != NULL;
    } else if (is_pen_handle(brush)) {
        pthread_mutex_lock(&g_pen_table_lock);
        brush_ok = find_pen(brush) != NULL;
        pthread_mutex_unlock(&g_pen_table_lock);
    } else if (!brush) {
        brush_ok = 0;
    }
    pthread_mutex_unlock(&g_region_table_lock);
    c->eax = (hdc && region_ok && brush_ok) ? 1u : 0u;
}

static void h_rect_in_region(CPU *c)
{
    const uint32_t region_handle = wc3_legacy_arg32(c, 0u);
    const uint32_t rect_address = wc3_legacy_arg32(c, 1u);
    GdiRect query;
    GdiRegion *region;
    unsigned slot = 0u;
    int inside = 0;

    if (!read_guest_rect(rect_address, &query)) {
        c->eax = 0u;
        return;
    }

    /* Pin the slot under the table, then scan payload after releasing it. */
    pthread_mutex_lock(&g_region_table_lock);
    region = find_region(region_handle);
    if (region) {
        slot = (unsigned)(region - g_regions);
        pthread_mutex_lock(&g_region_locks[slot]);
    }
    pthread_mutex_unlock(&g_region_table_lock);

    if (region) {
        for (unsigned i = 0; i < region->count; i++) {
            GdiRect intersection;
            if (rect_intersect(&region->rects[i], &query, &intersection)) {
                inside = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_region_locks[slot]);
    }
    c->eax = inside ? 1u : 0u;
}

static void h_create_pen(CPU *c)
{
    const uint32_t style = wc3_legacy_arg32(c, 0u);
    const uint32_t width = wc3_legacy_arg32(c, 1u);
    const uint32_t color = wc3_legacy_arg32(c, 2u);
    uint32_t handle = 0u;
    GdiPen *pen;
    unsigned slot = 0u;

    /* Publish identity and pin the slot while the table is still held. */
    pthread_mutex_lock(&g_pen_table_lock);
    pen = new_pen(&handle);
    if (pen) {
        slot = (unsigned)(pen - g_pens);
        pthread_mutex_lock(&g_pen_locks[slot]);
    }
    pthread_mutex_unlock(&g_pen_table_lock);

    /* Payload only; identity is already published under the table lock.
     * Any handler that needs this payload waits on the slot here. */
    if (pen) {
        pen->style = style;
        pen->width = width;
        pen->color = color;
        pthread_mutex_unlock(&g_pen_locks[slot]);
    }
    c->eax = handle;
}

static void h_delete_object(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    int deleted = 0;

    if (is_region_handle(handle)) {
        GdiRegion *region;
        unsigned slot = 0u;

        pthread_mutex_lock(&g_region_table_lock);
        region = find_region(handle);
        if (region) {
            slot = (unsigned)(region - g_regions);
            pthread_mutex_lock(&g_region_locks[slot]);
            /* Identity is cleared while the table lock is still held. */
            region->used = 0u;
            region->handle = 0u;
        }
        pthread_mutex_unlock(&g_region_table_lock);

        if (region) {
            region->count = 0u;
            memset(region->rects, 0, sizeof region->rects);
            pthread_mutex_unlock(&g_region_locks[slot]);
            deleted = 1;
        }
    } else if (is_pen_handle(handle)) {
        GdiPen *pen;
        unsigned slot = 0u;

        pthread_mutex_lock(&g_pen_table_lock);
        pen = find_pen(handle);
        if (pen) {
            slot = (unsigned)(pen - g_pens);
            pthread_mutex_lock(&g_pen_locks[slot]);
            /* Identity is cleared while the table lock is still held. */
            pen->used = 0u;
            pen->handle = 0u;
        }
        pthread_mutex_unlock(&g_pen_table_lock);

        /* Only payload is cleared here.  The slot stays pinned, so a creator
         * that has already claimed this free slot waits before writing. */
        if (pen) {
            pen->style = 0u;
            pen->width = 0u;
            pen->color = 0u;
            pthread_mutex_unlock(&g_pen_locks[slot]);
            deleted = 1;
        }
    } else if (handle) {
        /* Stock and as-built GDI objects live outside this table. */
        deleted = 1;
    }
    c->eax = deleted ? 1u : 0u;
}

/* Caller must hold g_position_table_lock.  If x/y are needed, the caller must
 * pin g_position_locks[slot] while still holding the table lock, then release
 * the table lock before touching them. */
static GdiDcPos *find_position(uint32_t hdc)
{
    for (unsigned i = 0; i < DC_POS_COUNT; i++) {
        if (g_positions[i].used && g_positions[i].hdc == hdc)
            return &g_positions[i];
    }
    return NULL;
}

/* Caller must hold g_position_table_lock.  Initialises identity only and
 * reports whether the slot was newly allocated; x/y are initialised by the
 * caller under the slot lock. */
static GdiDcPos *ensure_position(uint32_t hdc, int *created)
{
    GdiDcPos *position = find_position(hdc);
    if (position) {
        *created = 0;
        return position;
    }
    for (unsigned i = 0; i < DC_POS_COUNT; i++) {
        if (g_positions[i].used) continue;
        g_positions[i].used = 1u;
        g_positions[i].hdc = hdc;
        *created = 1;
        return &g_positions[i];
    }
    *created = 0;
    return NULL;
}

static void h_move_to_ex(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 1u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 2u);
    const uint32_t old = wc3_legacy_arg32(c, 3u);
    GdiDcPos *position = NULL;
    unsigned slot = 0u;
    int created = 0;
    int have_position = 0;
    int32_t old_x = 0;
    int32_t old_y = 0;

    /* Pin the slot under the table, then release the table before x/y work. */
    pthread_mutex_lock(&g_position_table_lock);
    if (hdc) {
        position = ensure_position(hdc, &created);
    }
    if (position) {
        slot = (unsigned)(position - g_positions);
        pthread_mutex_lock(&g_position_locks[slot]);
        have_position = 1;
    }
    pthread_mutex_unlock(&g_position_table_lock);

    if (have_position) {
        if (created) {
            position->x = 0;
            position->y = 0;
        }
        old_x = position->x;
        old_y = position->y;
        position->x = x;
        position->y = y;
        pthread_mutex_unlock(&g_position_locks[slot]);
    }

    /* Guest memory is written only after all host locks are released. */
    if (have_position && old && wc3_legacy_guest_ok(old, 8u)) {
        wc3_legacy_wr32g(old, (uint32_t)old_x);
        wc3_legacy_wr32g(old + 4u, (uint32_t)old_y);
    }
    c->eax = have_position ? 1u : 0u;
}

static void h_line_to(CPU *c)
{
    const uint32_t hdc = wc3_legacy_arg32(c, 0u);
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 1u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 2u);
    GdiDcPos *position = NULL;
    unsigned slot = 0u;
    int created = 0;
    int have_position = 0;

    /* Pin the slot under the table, then release the table before x/y work. */
    pthread_mutex_lock(&g_position_table_lock);
    if (hdc) {
        position = ensure_position(hdc, &created);
    }
    if (position) {
        slot = (unsigned)(position - g_positions);
        pthread_mutex_lock(&g_position_locks[slot]);
        have_position = 1;
    }
    pthread_mutex_unlock(&g_position_table_lock);

    if (have_position) {
        if (created) {
            position->x = 0;
            position->y = 0;
        }
        position->x = x;
        position->y = y;
        pthread_mutex_unlock(&g_position_locks[slot]);
    }
    c->eax = have_position ? 1u : 0u;
}

static void h_rectangle(CPU *c)
{
    c->eax = wc3_legacy_arg32(c, 0u) ? 1u : 0u;
}

static void h_gdi_flush(CPU *c)
{
    c->eax = 1u;
}

typedef struct GdiObjectOverride {
    const char *name;
    void (*impl)(CPU *c);
} GdiObjectOverride;

static const GdiObjectOverride g_overrides[] = {
    { "DeleteObject", h_delete_object },
};

typedef struct GdiObjectAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} GdiObjectAddition;

static const GdiObjectAddition g_additions[] = {
    { "CreateRectRgn", 4u, h_create_rect_rgn },
    { "CreateRectRgnIndirect", 1u, h_create_rect_rgn_indirect },
    { "CombineRgn", 4u, h_combine_rgn },
    { "FillRgn", 3u, h_fill_rgn },
    { "RectInRegion", 2u, h_rect_in_region },
    { "CreatePen", 3u, h_create_pen },
    { "MoveToEx", 4u, h_move_to_ex },
    { "LineTo", 3u, h_line_to },
    { "Rectangle", 5u, h_rectangle },
    { "GdiFlush", 0u, h_gdi_flush },
};

int wc3_hle_gdi32_objects_install(void)
{
    if (pthread_once(&g_slot_locks_once, init_slot_locks) != 0 ||
        !g_slot_locks_ready)
        return -1;

    /* Installation runs before guest threads exist; the slot mutexes are
     * already initialised and the three table locks are always taken in the
     * order region -> pen -> position. */
    pthread_mutex_lock(&g_region_table_lock);
    pthread_mutex_lock(&g_pen_table_lock);
    pthread_mutex_lock(&g_position_table_lock);
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        g_regions[i].used = 0u;
        g_regions[i].handle = 0u;
    }
    memset(g_pens, 0, sizeof g_pens);
    memset(g_positions, 0, sizeof g_positions);
    pthread_mutex_unlock(&g_position_table_lock);
    pthread_mutex_unlock(&g_pen_table_lock);
    pthread_mutex_unlock(&g_region_table_lock);

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("GDI32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("GDI32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
