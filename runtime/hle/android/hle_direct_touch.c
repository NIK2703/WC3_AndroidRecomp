/*
 * hle_direct_touch.c - resolve direct-touch gestures on the guest thread.
 *
 * The overlay layer has already decided that a touch belongs to the
 * battlefield.  This file turns the pointer-free event into the same native
 * calls the game would make for a left click (select) or a right click
 * (smart order).  It runs at the PeekMessageA/GetMessageA boundary, so the
 * nested guest calls are equivalent to the ones the window procedure makes
 * for a real mouse message.
 *
 * Phase 1 deliberately keeps the semantic surface small:
 *   - tap: select a friendly unit, or smart-order an enemy/ground target;
 *   - long press: force a smart order to a unit or ground point;
 *   - no drag, no box select, no camera, no synthetic WM_* input.
 *
 * The guest pointers below are addresses from the fixed 1.27.1.7085 image
 * base.  They are pinned in docs/research/DIRECT_TOUCH_CONTROL_ANALYSIS.md;
 * do not replace them with guessed field offsets.
 */
#include "hle_direct_touch.h"
#include "hle_direct_touch_intent.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "guest_host.h"
#include "guest_memory.h"
#include "hle_android.h"
#include "wc3_debug.h"

#define DT_UI_GLOBAL       0x6FD6A8B8u
#define DT_WORLD_GLOBAL    0x6FD47818u
#define DT_PLAYER_MANAGER  0x6FD687A8u

#define DT_UI_WORLD_FRAME  0x000003BCu
#define DT_UI_CLICK_GATE   0x000001ACu
#define DT_UI_ACTIVE_MODE  0x000001B4u
#define DT_UI_MODE_COUNT   0x000001BCu
#define DT_UI_MODE_ARRAY   0x000001C0u
#define DT_UI_TARGET_MODE  0x00000210u
#define DT_UI_SELECT_MODE  0x00000214u
#define DT_UI_DRAG_MODE    0x00000218u
#define DT_UI_ALT_MODE     0x00000220u
#define DT_UI_CAMERA       0x00000254u
#define DT_UI_MANAGER      0x0000024Cu
#define DT_UI_INFO_BAR     0x000003C4u
#define DT_UI_PORTRAIT     0x000003F4u
#define DT_UI_MINIMAP      0x000003C0u
#define DT_UI_COMMAND_BAR  0x000003C8u

#define DT_PORTRAIT_UNIT   0x00000238u
#define DT_PORTRAIT_FRAME  0x000000B4u
#define DT_PORTRAIT_OFF    0x00000260u
#define DT_MINIMAP_FRAME   0x000000B4u
#define DT_FRAME_RECT      0x00000044u
#define DT_FRAME_VISIBLE   0x00000054u

/* Selected-unit panel: CInfoBar+0x13C -> CInfoPanelGroup; the active grid is
 * group+0x138 (6x2, 8x3 or 10x4 depending on the selection size). */
#define DT_INFO_GROUP        0x0000013Cu
#define DT_GROUP_ACTIVE_GRID 0x00000138u
#define DT_GROUP_VISIBLE     0x00000090u
#define DT_GRID_ROWS         0x00000124u
#define DT_GRID_COLS         0x00000128u
#define DT_GRID_ROW_ARRAY    0x00000154u
#define DT_GRID_VISIBLE      0x00000090u
#define DT_GRID_ROW_STRIDE   0x00000010u
#define DT_GRID_ROW_BUTTONS  0x00000008u
#define DT_GRID_BUTTON_UNIT  0x00000124u
#define DT_GRID_BUTTON_VISIBLE 0x00000090u
/* CGridPanel stores rows at +0x124 and columns at +0x128: 2x6 for the
 * original panel, 3x8 for grid24 and 4x10 for grid40.  Do not swap these
 * limits: a cols limit of 4 would reject every grid, including 6x2. */
#define DT_GRID_MAX_ROWS     4u
#define DT_GRID_MAX_COLS     10u

/* Inventory bar: CInfoBar+0x148 -> CInventoryBar.  Each slot entry is eight
 * bytes: the item holder at +0 and the CCommandButton* at +4.  The button's
 * command data lives at +0x190; the native 0x30065 handler rejects a button
 * without it.  The data keeps its item holder at +0x5C0. */
#define DT_INFO_INVENTORY    0x00000148u
#define DT_INV_SIZE          0x00000128u
#define DT_INV_CAP           0x0000012Cu
#define DT_INV_ARRAY         0x00000130u
#define DT_INV_ENTRY_ITEM    0x00000000u
#define DT_INV_ENTRY_BUTTON  0x00000004u
#define DT_INV_BUTTON_DATA   0x00000190u
#define DT_INV_DATA_ITEM     0x000005C0u
#define DT_INV_MAX_SLOTS     6u
/* Mirror of controls.c direct_touch_field_contains().  A release inside this
 * rectangle is a battlefield point; everything outside it is native HUD and
 * must not turn an inventory drag into a drop.  Keep the two in sync. */
#define DT_INV_FIELD_LEFT    0.03f
#define DT_INV_FIELD_RIGHT   0.97f
#define DT_INV_FIELD_TOP     0.10f
#define DT_INV_FIELD_BOTTOM  0.72f

#define DT_SELECTION_PRIMARY 0x000001E4u

#define DT_WORLD_SELECT_GATE 0x000001A4u
#define DT_WORLD_ALT_GATE    0x000001A8u

#define DT_FN_IN_GAME          0x6F36E1F0u
#define DT_FN_MODE_ACTIVE      0x6F36E330u
#define DT_FN_EVENT_GATE       0x6F072190u
#define DT_FN_PAUSED           0x6F32C110u
#define DT_FN_INPUT_COORDS     0x6F8B38A0u
#define DT_FN_PICK             0x6F3840E0u
#define DT_FN_PLAYER           0x6F1E0EA0u
#define DT_FN_VALIDATE_OBJECT  0x6F21B730u
#define DT_FN_SELECT_EVENT     0x6F395410u
#define DT_FN_SELECTION_COUNT  0x6F290270u
/* 0x6F291240 is CSelectionWar3::Realize (mirror update), not a clear.
 * dt_clear_selection() predates this analysis; do not copy the name into new
 * paths.  The grid path uses the explicit DT_FN_SELECTION_REALIZE alias. */
#define DT_FN_CLEAR_SELECTION  0x6F291240u
#define DT_FN_CAN_ORDER        0x6F66EC60u
#define DT_FN_GHOST_INIT       0x6F37C550u
#define DT_FN_CLICK_DISPATCH   0x6F392FA0u
#define DT_FN_GROUND_ORDER     0x6F394680u
#define DT_FN_HANDLE_TO_UNIT   0x6F1EEF90u
#define DT_FN_UNIT_POSITION    0x6F058900u
#define DT_FN_CAMERA_PAN       0x6F361B90u
#define DT_FN_CAMERA_FOLLOW    0x6F3635E0u
#define DT_FN_GRID_ACTIVATE    0x6F293480u
#define DT_FN_SUBGROUP_HAS_UNIT 0x6F290820u
#define DT_FN_GRID_SELECT_UNIT 0x6F396CF0u
#define DT_FN_SELECTION_REALIZE 0x6F291240u
#define DT_FN_COMMAND_BAR_POP  0x6F3C4E40u
#define DT_FN_COMMAND_BAR_HIDE 0x6F3ACA80u
#define DT_FN_UI_DIRTY         0x6F377190u
#define DT_FN_TARGET_UNIT      0x6F3BF010u
#define DT_FN_HOLDER_ALIVE     0x6F0549F0u

#define DT_SELECTION_ACTIVE_GROUP 0x0000020Cu
/* The native 0x50065 handler derives the selection holder from the unit:
 * holder = [[unit+0x6C]+0x124].  The grid button stores a unit holder at
 * +0x124; dt_handle_unit() resolves that to the class instance first. */
#define DT_UNIT_HOLDER_SUB     0x0000006Cu
#define DT_UNIT_HOLDER_REF     0x00000124u
#define DT_UNIT_POSITION_SLOT  0x000000B8u
#define DT_CAMERA_READ_SIZE    0x00000060u

#define DT_EVENT_ID_TERRAIN_GATE 0x1A0068u
#define DT_EVENT_ID_GHOST        0x1A0065u
#define DT_EVENT_ID_GRID_TARGET  0x00050065u

#define DT_CLASS_UNIT 0x2B61676Cu

#define DT_SCRATCH_SIZE 0x100u
#define DT_PICK_OFFSET  0x00u
#define DT_POINT_OFFSET 0x30u
#define DT_UNIT_OFFSET  0x40u
#define DT_EVENT_OFFSET 0x50u
#define DT_EVENT_SIZE   0x40u

#define DT_MAX_EVENTS_PER_PUMP 16u
/* Backstop for a tap that was queued before the game left the map while the
 * guest thread was not pumping.  A normal event is serviced within one pump;
 * anything older than this belongs to a previous map/menu and must not be
 * replayed against the current one. */
#define DT_EVENT_MAX_AGE_MS 2000u

typedef struct {
    uint32_t ui;
    uint32_t world;
    uint32_t manager;
    uint32_t mode;      /* [ui+0x214], CSelectMode */
} DtContext;

static pthread_t g_main_thread;
static int g_main_thread_valid;
/* Re-entrancy guard, written only by the main guest thread but read by worker
 * GetMessageA callers through wc3_hle_direct_touch_can_service(). */
static int g_busy;
/* Routing snapshot.  The guest thread writes it at the message-pump boundary;
 * the Android input thread reads it before claiming a pointer.  Atomic so no
 * host lock is shared across those threads. */
static int g_route;
/* Set only while the service has posted a native WM_LBUTTONDOWN and has not
 * yet posted the matching release.  Main guest thread only. */
static int g_native_down;
/* Set after a direct-owned release was posted but before the guest dispatched
 * it.  A second tap can arrive in the same service batch; the HLE still
 * reports the button down, so the next DOWN must bypass that guard while the
 * queued UP is ahead of it in the message queue.  Main guest thread only. */
static int g_native_up_pending;
/* Last context the service dispatched against.  A change means the queue may
 * still hold semantic events from the previous map/menu even if the
 * transition itself was never observed (the guest thread was not pumping). */
static uint32_t g_last_ui;
static uint32_t g_last_world;
/* Portrait and minimap frame snapshots, surface-normalised.  The guest thread
 * is the only writer; the Android input thread reads them through
 * wc3_hle_direct_touch_portrait_rect()/wc3_hle_direct_touch_minimap_rect().
 * A seqlock keeps the four floats consistent without sharing a host lock
 * across threads. */
typedef struct {
    uint32_t seq;
    uint32_t rect[4];
} DtRectSnapshot;

static DtRectSnapshot g_portrait_snapshot;
static DtRectSnapshot g_minimap_snapshot;
/* Set by wc3_hle_direct_touch_reset() (any thread) when the game leaves the
 * in-game UI.  The service, on the main guest thread, clears the camera
 * follow before it drops the queue. */
static int g_portrait_release_pending;
/* Selected-unit grid drag.  The native grid only reacts on a completed click,
 * so a touch that starts on a button is consumed here and the subgroup under
 * the finger is activated on DOWN and on every MOVE to a different button.
 * Main guest thread only. */
static int g_grid_active;
static uint32_t g_grid_pointer;
static uint32_t g_grid_ui;
static uint32_t g_grid_selection;
static uint32_t g_grid_unit;
/* Set when the grid touch selected a target instead of activating a group.
 * The rest of the gesture is consumed so a drag cannot retarget or reach the
 * native world handler. */
static int g_grid_target;

/* Inventory item drag.  A touch that starts on an occupied slot is consumed
 * so the release can be replayed as the native click pair: left click on the
 * source (use), or right click on the source followed by a left click on the
 * destination slot or world point (swap/move/drop).  Main guest thread only. */
static int g_inv_active;
static uint32_t g_inv_pointer;
static uint32_t g_inv_bar;
static uint32_t g_inv_slot;
static float g_inv_down_x;
static float g_inv_down_y;
/* Our right-button UP may still be queued when the next drag starts.  Force
 * the next DOWN past the logical button bit so the guest still sees a clean
 * DOWN/UP pair, the same way g_native_up_pending handles the left button. */
static int g_inv_right_pending;

#if WC3_DEBUG && WC3_DEBUG_INPUT
static void dt_trace(const char *what, uint32_t a, uint32_t b)
{
    fprintf(stderr, "[direct] %s a=%08X b=%08X\n", what, a, b);
}
#else
#define dt_trace(what, a, b) ((void)0)
#endif

static uint32_t dt_bits(float value)
{
    union {
        float f;
        uint32_t u;
    } u;
    u.f = value;
    return u.u;
}

static float dt_float(uint32_t bits)
{
    union {
        uint32_t u;
        float f;
    } v;
    v.u = bits;
    return v.f;
}

static float dt_clamp01(float value)
{
    if (!(value > 0.0f)) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/* Event timestamps come from the input thread's monotonic clock.  The service
 * runs on the same device clock, so the age check below stays in one domain. */
static uint64_t dt_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int dt_guest_readable(uint32_t va, uint32_t size)
{
    return va && hle_android_guest_ok(va, size);
}

static void dt_publish_route(Wc3DirectTouchRoute route)
{
    __atomic_store_n(&g_route, (int)route, __ATOMIC_RELEASE);
}

static void dt_rect_publish(DtRectSnapshot *snap, float left, float top,
                            float right, float bottom)
{
    const uint32_t seq = __atomic_load_n(&snap->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&snap->seq, seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&snap->rect[0], dt_bits(left), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[1], dt_bits(top), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[2], dt_bits(right), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->rect[3], dt_bits(bottom), __ATOMIC_RELAXED);
    __atomic_store_n(&snap->seq, seq + 2u, __ATOMIC_RELEASE);
}

static int dt_rect_read(const DtRectSnapshot *snap, float *left, float *top,
                        float *right, float *bottom)
{
    if (!left || !top || !right || !bottom) return 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t seq1 =
            __atomic_load_n(&snap->seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1u) continue;
        const uint32_t l =
            __atomic_load_n(&snap->rect[0], __ATOMIC_RELAXED);
        const uint32_t t =
            __atomic_load_n(&snap->rect[1], __ATOMIC_RELAXED);
        const uint32_t r =
            __atomic_load_n(&snap->rect[2], __ATOMIC_RELAXED);
        const uint32_t b =
            __atomic_load_n(&snap->rect[3], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t seq2 =
            __atomic_load_n(&snap->seq, __ATOMIC_RELAXED);
        if (seq1 != seq2 || (seq2 & 1u)) continue;

        const float fl = dt_float(l);
        const float ft = dt_float(t);
        const float fr = dt_float(r);
        const float fb = dt_float(b);
        if (!(fr > fl) || !(fb > ft)) return 0;
        *left = fl;
        *top = ft;
        *right = fr;
        *bottom = fb;
        return 1;
    }
    return 0;
}

static void dt_portrait_publish(float left, float top, float right,
                                float bottom)
{
    dt_rect_publish(&g_portrait_snapshot, left, top, right, bottom);
}

static void dt_minimap_publish(float left, float top, float right,
                               float bottom)
{
    dt_rect_publish(&g_minimap_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_portrait_rect(float *left, float *top,
                                       float *right, float *bottom)
{
    return dt_rect_read(&g_portrait_snapshot, left, top, right, bottom);
}

int wc3_hle_direct_touch_minimap_rect(float *left, float *top,
                                      float *right, float *bottom)
{
    return dt_rect_read(&g_minimap_snapshot, left, top, right, bottom);
}

/*
 * Read a frame subobject rect in surface-normalised form.  The native layout
 * builds the rect from anchor fractions; depending on the UI path it is
 * either already 0..1 or in guest client pixels.  Detect the pixel form by
 * its magnitude, normalise it against the guest client size, and clamp both
 * forms to the surface.
 */
static int dt_frame_rect_read(uint32_t frame, uint32_t width,
                              uint32_t height, float *out_left,
                              float *out_top, float *out_right,
                              float *out_bottom)
{
    if (!dt_guest_readable(frame, DT_FRAME_RECT + 20u)) return 0;

    float l = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 0u));
    float t = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 4u));
    float r = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 8u));
    float b = dt_float(wc3_rd32g(frame + DT_FRAME_RECT + 12u));
    if (!isfinite(l) || !isfinite(t) || !isfinite(r) || !isfinite(b) ||
        !(r > l) || !(b > t)) {
        return 0;
    }
    if (r > 1.5f || b > 1.5f) {
        if (width <= 1u || height <= 1u) return 0;
        l /= (float)width;
        r /= (float)width;
        t /= (float)height;
        b /= (float)height;
    }
    l = dt_clamp01(l);
    t = dt_clamp01(t);
    r = dt_clamp01(r);
    b = dt_clamp01(b);
    if (!(r > l) || !(b > t)) return 0;
    *out_left = l;
    *out_top = t;
    *out_right = r;
    *out_bottom = b;
    return 1;
}

/* Same rect, but only when the frame is visible.  The portrait, minimap and
 * selected-unit grid use this; inventory slot buttons are laid out and
 * clickable even when an individual slot has no item, so their lookup reads
 * the rect without the visibility gate. */
static int dt_frame_rect_snapshot(uint32_t frame, uint32_t width,
                                  uint32_t height, float *out_left,
                                  float *out_top, float *out_right,
                                  float *out_bottom)
{
    if (!dt_guest_readable(frame, DT_FRAME_RECT + 20u) ||
        wc3_rd32g(frame + DT_FRAME_VISIBLE) == 0) {
        return 0;
    }
    return dt_frame_rect_read(frame, width, height, out_left, out_top,
                              out_right, out_bottom);
}

/*
 * Publish the visible portrait frame.  The portrait handler ignores the
 * frame while [portrait+0x260] is non-zero, so an empty rect keeps direct
 * touch from claiming a portrait the game itself would not follow.
 */
static void dt_portrait_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_PORTRAIT, 4u)) {
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t portrait = wc3_rd32g(ui + DT_UI_PORTRAIT);
    if (!portrait ||
        !dt_guest_readable(portrait, DT_PORTRAIT_OFF + 4u) ||
        wc3_rd32g(portrait + DT_PORTRAIT_OFF) != 0) {
        /* The native handler treats a non-zero flag as "portrait off"
         * (0x6F3C203F); do not claim a frame the game itself ignores. */
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    if (!dt_frame_rect_snapshot(portrait + DT_PORTRAIT_FRAME, width, height,
                                &l, &t, &r, &b)) {
        dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_portrait_publish(l, t, r, b);
}

/*
 * Publish the visible minimap frame.  [CGameUI+0x3C0] is the CMinimap
 * object; the visible frame is its +0xB4 subobject, anchored to the minimap
 * container at CGameUI+0x41C.  The primary object's +0x44 rect is not the
 * sized minimap frame, so read the subobject, matching the portrait layout.
 */
static void dt_minimap_snapshot(uint32_t ui, uint32_t width, uint32_t height)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_MINIMAP, 4u)) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint32_t minimap = wc3_rd32g(ui + DT_UI_MINIMAP);
    if (!minimap) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    if (!dt_frame_rect_snapshot(minimap + DT_MINIMAP_FRAME, width, height,
                                &l, &t, &r, &b)) {
        dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    dt_minimap_publish(l, t, r, b);
}

/*
 * Pointer-only replica of 0x6F36E330: the UI keeps the stack of active input
 * modes at +0x1BC (count) and +0x1C0 (array).  Reading it here avoids a guest
 * call on every message-pump boundary.  known is cleared when the stack cannot
 * be read safely; the caller then routes to native input instead of guessing
 * that the battlefield owns the touch.
 */
static int dt_mode_active_snapshot(uint32_t ui, uint32_t mode, int *known)
{
    *known = 0;
    if (!mode) {
        *known = 1;
        return 0;
    }
    if (!dt_guest_readable(ui + DT_UI_MODE_COUNT, 8u)) return 0;
    const uint32_t count = wc3_rd32g(ui + DT_UI_MODE_COUNT);
    if (count > 64u) return 0;
    const uint32_t array = wc3_rd32g(ui + DT_UI_MODE_ARRAY);
    if (count && (!array || !dt_guest_readable(array, count * 4u))) return 0;
    *known = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (wc3_rd32g(array + i * 4u) == mode) return 1;
    }
    return 0;
}

/*
 * Pointer-only routing snapshot.  Once the UI singleton is readable the
 * native window exists, so menus, loading screens, HUD and targeting all
 * route to native pointer phases.  Only a missing UI keeps a touch on the
 * legacy overlay path; that is a startup boundary, not a per-feature
 * fallback.  No guest calls.
 */
static Wc3DirectTouchRoute dt_route_snapshot(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) {
        return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    }
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4u)) {
        return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    }

    const uint32_t select_mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    const uint32_t active_mode = wc3_rd32g(ui + DT_UI_ACTIVE_MODE);
    const uint32_t target_mode = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    const int world_present = wc3_rd32g(ui + DT_UI_WORLD_FRAME) != 0;
    const int click_gate = wc3_rd32g(ui + DT_UI_CLICK_GATE) != 0;

    /* No battlefield yet (menu/loading) or a modal UI blocked clicks: the
     * native window is still the right recipient. */
    if (!select_mode || !world_present || !click_gate) {
        return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    }

    /* The semantic executor needs the live world and player manager.  If
     * either is in transition, native pointer phases are still safe; do not
     * claim a field gesture that the executor would have to drop. */
    if (!dt_guest_readable(DT_WORLD_GLOBAL, 4) ||
        !wc3_rd32g(DT_WORLD_GLOBAL) ||
        !dt_guest_readable(DT_PLAYER_MANAGER, 4) ||
        !wc3_rd32g(DT_PLAYER_MANAGER)) {
        return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    }

    int select_known = 0;
    int target_known = 0;
    const int select_active =
        dt_mode_active_snapshot(ui, select_mode, &select_known);
    const int target_active =
        dt_mode_active_snapshot(ui, target_mode, &target_known);
    if (!select_known || !target_known) {
        return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    }
    return wc3_dt_route(1, select_active, target_active,
                        active_mode == select_mode);
}

/* hle_guest_alloc() is 8-byte aligned and the guest mapping may live in a
 * 4 GiB host window, so host access always goes through guest_host_ptr().
 * The scratch is freed after one event. */
static uint32_t dt_scratch_alloc(void)
{
    uint32_t p = hle_guest_alloc(DT_SCRATCH_SIZE);
    if (!p || !hle_android_guest_ok(p, DT_SCRATCH_SIZE)) return 0;
    memset(guest_host_ptr(p), 0, DT_SCRATCH_SIZE);
    return p;
}

static void dt_scratch_free(uint32_t p)
{
    if (p) hle_guest_free(p);
}

static uint32_t dt_call_thiscall(CPU *c, uint32_t this_va, uint32_t fn,
                                 int argc, const uint32_t *args)
{
    c->ecx = this_va;
    c->edx = 0;
    return wc3_hle_call_guest(c, fn, argc, args);
}

static int dt_call_ui_bool(CPU *c, uint32_t ui, uint32_t fn)
{
    return dt_call_thiscall(c, ui, fn, 0, NULL) != 0;
}

static int dt_mode_active(CPU *c, uint32_t ui, uint32_t mode)
{
    if (!mode) return 0;
    return dt_call_thiscall(c, ui, DT_FN_MODE_ACTIVE, 1, &mode) != 0;
}

static int dt_event_gate(CPU *c, uint32_t world, uint32_t event_id)
{
    return dt_call_thiscall(c, world, DT_FN_EVENT_GATE, 1, &event_id) != 0;
}

static int dt_paused(CPU *c)
{
    return wc3_hle_call_guest(c, DT_FN_PAUSED, 0, NULL) != 0;
}

/*
 * Reproduce the native input-event conversion.  The overlay reports
 * surface-normalised coordinates; the window procedure receives client
 * pixels, and 0x6F074010 turns those into pick-event coordinates through
 * 0x6F8B38A0 (x*0.8, y*0.6).  0x6F3840E0 consumes that event space, not
 * [0,1], so using ev->x/ev->y directly would pick near the top-left corner.
 */
static int dt_event_point(CPU *c, float nx, float ny, uint32_t width,
                          uint32_t height, uint32_t out)
{
    const uint32_t w = width > 1u ? width : 800u;
    const uint32_t h = height > 1u ? height : 600u;
    /* The native producer truncates the client pixel float before
     * 0x6F074010 hands it to 0x6F8B38A0. */
    const float x = wc3_dt_client_pixel(nx, w);
    const float y = wc3_dt_client_pixel(ny, h);

    const uint32_t args[2] = { dt_bits(x), dt_bits(y) };
    c->ecx = out;
    c->edx = out + 4u;
    (void)wc3_hle_call_guest(c, DT_FN_INPUT_COORDS, 2, args);
    return dt_guest_readable(out, 8);
}

static int dt_context(DtContext *ctx)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!dt_guest_readable(ui, DT_UI_WORLD_FRAME + 4)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;

    const uint32_t mode = wc3_rd32g(ui + DT_UI_SELECT_MODE);
    if (!mode) return 0;

    if (!dt_guest_readable(DT_WORLD_GLOBAL, 4)) return 0;
    const uint32_t world = wc3_rd32g(DT_WORLD_GLOBAL);
    if (!dt_guest_readable(world, DT_WORLD_ALT_GATE + 4)) return 0;

    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!dt_guest_readable(manager, 0x60)) return 0;

    ctx->ui = ui;
    ctx->world = world;
    ctx->manager = manager;
    ctx->mode = mode;
    return 1;
}

/*
 * Track the live UI/world pointers while the queue is empty.  Calling this
 * only when an event is queued would leave g_last_* at the previous map:
 * the first tap after a transition would then look like a stale event and
 * be discarded.  The queue is empty here, so adopting a new pointer cannot
 * rescue a stale event; it only keeps the next comparison honest.
 *
 * Pointer-only: no guest calls, no dispatch.  Runs on the main guest thread
 * through wc3_hle_direct_touch_can_service().
 */
static void dt_sync_idle_context(uint32_t width, uint32_t height)
{
    uint32_t ui = 0;
    uint32_t world = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4) &&
        dt_guest_readable(DT_WORLD_GLOBAL, 4)) {
        ui = wc3_rd32g(DT_UI_GLOBAL);
        world = wc3_rd32g(DT_WORLD_GLOBAL);
    }
    /* Keep the input thread's portrait and minimap rectangles fresh even
     * while no direct event is queued: the next touch must not hit a stale
     * frame. */
    dt_portrait_snapshot(ui, width, height);
    dt_minimap_snapshot(ui, width, height);
    /* The snapshot decides between FIELD and NATIVE, including menus and
     * loading screens.  Only a missing UI is LEGACY. */
    dt_publish_route(dt_route_snapshot());
    /* A partial context is a transition in progress.  Keep the previous
     * marker: an event produced now (for example a tap on the loading
     * screen) must still compare against the old map and be dropped. */
    if (!ui || !world) return;
    g_last_ui = ui;
    g_last_world = world;
}

/*
 * Exact readiness for the semantic executor.  Native pointer events do not
 * need this context: they only post Win32 mouse messages.  LEGACY here means
 * "no semantic field action", not "send the touch to the overlay engine".
 */
static Wc3DirectTouchRoute dt_ready_route(CPU *c, DtContext *ctx)
{
    if (!dt_context(ctx)) return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    if (!dt_call_ui_bool(c, ctx->ui, DT_FN_IN_GAME)) {
        return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    }

    const uint32_t target_mode = wc3_rd32g(ctx->ui + DT_UI_TARGET_MODE);
    if (target_mode && dt_mode_active(c, ctx->ui, target_mode)) {
        return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    }
    if (dt_mode_active(c, ctx->ui, ctx->mode)) {
        const uint32_t active_mode =
            wc3_rd32g(ctx->ui + DT_UI_ACTIVE_MODE);
        if (active_mode == ctx->mode) {
            return WC3_DIRECT_TOUCH_ROUTE_FIELD;
        }
    }
    return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
}

static void dt_publish_service_route(Wc3DirectTouchRoute route)
{
    if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
        dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_FIELD);
    } else {
        /* Menus/loading/modal UI are native recipients even though the
         * semantic executor has no context. */
        dt_publish_route(dt_route_snapshot());
    }
}

/*
 * Mirror CWorldFrameWar3::OnSelect's pick-context initialisation.  A direct
 * tap is a left click; a long press is modelled as a right click so the same
 * native smart-order path is used.
 */
static int dt_init_pick(CPU *c, const DtContext *ctx, uint32_t pick,
                        uint32_t button)
{
    const uint32_t ui = ctx->ui;
    const uint32_t world = ctx->world;

    wc3_wr32g(pick + 0x00u, 0);
    wc3_wr32g(pick + 0x04u, 2);

    if (wc3_rd32g(world + DT_WORLD_SELECT_GATE) & button) {
        uint32_t flags = 2u | 0x210u;
        const uint32_t select_mode = ctx->mode;
        const uint32_t drag_mode = wc3_rd32g(ui + DT_UI_DRAG_MODE);
        const uint32_t alt_mode = wc3_rd32g(ui + DT_UI_ALT_MODE);

        wc3_wr32g(pick + 0x00u, 1);
        if (button != 4u && !dt_mode_active(c, ui, drag_mode))
            flags |= 0x20u;
        if (dt_mode_active(c, ui, alt_mode))
            flags |= 0x84u;
        if (button != 4u && dt_mode_active(c, ui, select_mode))
            flags |= 0x04u;
        wc3_wr32g(pick + 0x04u, flags);
    }

    if (dt_event_gate(c, world, DT_EVENT_ID_TERRAIN_GATE) &&
        (wc3_rd32g(world + DT_WORLD_ALT_GATE) & button)) {
        wc3_wr32g(pick + 0x00u, wc3_rd32g(pick + 0x00u) | 2u);
    }

    return wc3_rd32g(pick + 0x00u) != 0;
}

/*
 * Pick a world target.  x_bits/y_bits are already in the native pick-event
 * coordinate space (see dt_event_point), not surface-normalised and not
 * client pixels.  Returns 0 when the native path would not pick at all (or
 * when phase 1 cannot represent the active targeting mode), 1 for a
 * unit/sprite and 2 for terrain.  On success the world point is read from the
 * pick context, and object is the raw pick object.  It is borrowed from the
 * pick context: 0x6F395410 reads event+0xC without releasing it, so the
 * executor must not release it either.  0x6F21B730 creates the reference that
 * the executor does release after the action.
 */
static int dt_pick(CPU *c, const DtContext *ctx, uint32_t x_bits,
                   uint32_t y_bits, uint32_t pick, uint32_t button,
                   uint32_t *object, uint32_t *wx_bits, uint32_t *wy_bits,
                   uint32_t *wz_bits)
{
    *object = 0;
    *wx_bits = 0;
    *wy_bits = 0;
    *wz_bits = 0;

    const uint32_t target_mode = wc3_rd32g(ctx->ui + DT_UI_TARGET_MODE);
    if (target_mode && dt_mode_active(c, ctx->ui, target_mode)) {
        dt_trace("targeting-mode", target_mode, 0);
        return 0;
    }

    if (!dt_init_pick(c, ctx, pick, button)) return 0;

    const uint32_t args[5] = { x_bits, y_bits, pick, 1u, 1u };
    const int r = (int)dt_call_thiscall(c, ctx->world, DT_FN_PICK, 5, args);
    if (r != 1 && r != 2) return 0;

    *object = wc3_rd32g(pick + 0x0Cu);
    *wx_bits = wc3_rd32g(pick + 0x10u);
    *wy_bits = wc3_rd32g(pick + 0x14u);
    *wz_bits = wc3_rd32g(pick + 0x18u);
    return r;
}

/*
 * 0x6F21B730 writes a validated, reference-counted object to the guest dword
 * at ecx.  The caller must release it after the action, exactly like
 * CWorldFrameWar3::OnSelect does.
 */
static uint32_t dt_validate_object(CPU *c, uint32_t object, uint32_t out)
{
    if (!object) return 0;
    wc3_wr32g(out, 0);
    (void)dt_call_thiscall(c, out, DT_FN_VALIDATE_OBJECT, 1, &object);
    return wc3_rd32g(out);
}

static void dt_release_object(CPU *c, uint32_t object)
{
    if (!object || !dt_guest_readable(object, 8)) return;

    uint32_t refs = wc3_rd32g(object + 4u);
    if (!refs) return;
    refs--;
    wc3_wr32g(object + 4u, refs);
    if (refs) return;

    const uint32_t vtable = wc3_rd32g(object);
    if (!vtable || !dt_guest_readable(vtable, 4)) return;
    const uint32_t destroy = wc3_rd32g(vtable);
    if (!destroy) return;
    (void)dt_call_thiscall(c, object, destroy, 0, NULL);
}

static uint32_t dt_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t local_id = wc3_rd16g(ctx->manager + 0x28u);
    const uint32_t player = dt_call_thiscall(c, ctx->manager, DT_FN_PLAYER, 1,
                                             &local_id);
    if (!player || !dt_guest_readable(player + 0x34u, 4)) return 0;

    const uint32_t selection = wc3_rd32g(player + 0x34u);
    /* The live selection exposes its count at +0x1F8 through
     * 0x6F290270; 0x6F291240 clears through +0x1AC.  A shorter readable
     * window would let a corrupt pointer pass the guard and fault inside a
     * guest call. */
    if (!selection || !dt_guest_readable(selection, 0x210u)) return 0;
    return selection;
}

/*
 * The portrait stores a handle-holder, not a CUnit*: [holder+0xC] is the JASS
 * handle and [holder+0x10] is its type.  0x6F1EEF90 resolves and validates it
 * exactly like the native GetUnitX/Y path.  Accept a direct CUnit* too, since
 * a fallback selection slot may already be resolved.
 */
static uint32_t dt_handle_unit(CPU *c, uint32_t holder)
{
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;
    if (wc3_rd32g(holder + 0x0Cu) == DT_CLASS_UNIT) {
        if (wc3_rd32g(holder + 0x20u) != 0) return 0;
        return holder;
    }

    const uint32_t handle = wc3_rd32g(holder + 0x0Cu);
    if (!handle) return 0;
    const uint32_t unit = dt_call_thiscall(c, handle,
                                           DT_FN_HANDLE_TO_UNIT, 0, NULL);
    if (!unit || !dt_guest_readable(unit, 0x24u)) return 0;
    if (wc3_rd32g(unit + 0x0Cu) != DT_CLASS_UNIT) return 0;
    if (wc3_rd32g(unit + 0x20u) != 0) return 0;
    return unit;
}

/*
 * The unit shown by the portrait, or the primary selection when the portrait
 * has no unit.  A displayed unit that no longer validates is not silently
 * replaced by a different selection: the portrait is the user's target.
 */
static uint32_t dt_portrait_unit(CPU *c, const DtContext *ctx)
{
    uint32_t holder = 0;
    if (dt_guest_readable(ctx->ui + DT_UI_PORTRAIT, 4u)) {
        const uint32_t portrait = wc3_rd32g(ctx->ui + DT_UI_PORTRAIT);
        if (portrait) {
            if (!dt_guest_readable(portrait, DT_PORTRAIT_OFF + 4u) ||
                wc3_rd32g(portrait + DT_PORTRAIT_OFF) != 0) {
                /* The native handler ignores the frame while this flag is
                 * set; do not fall back to the selection and pan anyway. */
                return 0;
            }
            if (dt_guest_readable(portrait, DT_PORTRAIT_UNIT + 4u)) {
                holder = wc3_rd32g(portrait + DT_PORTRAIT_UNIT);
            }
            if (holder) return dt_handle_unit(c, holder);
        }
    }

    const uint32_t selection = dt_selection(c, ctx);
    if (!selection ||
        !dt_guest_readable(selection + DT_SELECTION_PRIMARY, 4u)) {
        return 0;
    }
    holder = wc3_rd32g(selection + DT_SELECTION_PRIMARY);
    return dt_handle_unit(c, holder);
}

static uint32_t dt_camera(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui + DT_UI_CAMERA, 4u)) return 0;
    const uint32_t camera = wc3_rd32g(ui + DT_UI_CAMERA);
    if (!camera || !dt_guest_readable(camera, DT_CAMERA_READ_SIZE)) return 0;
    return camera;
}

/*
 * CUnit vtable +0xB8 returns the unit's handle-holder subobject (its own
 * vtable is 0x6FAC2F10; handle at +8, type at +0xC).  The native GetUnitX/Y
 * path then calls 0x6F058900(holder, &vec), which resolves the holder and
 * writes the world position as three floats.  Call the virtual instead of
 * reading a fixed offset so a derived unit class keeps its own subobject.
 */
static int dt_unit_position(CPU *c, uint32_t unit, float *x, float *y)
{
    if (!unit || !dt_guest_readable(unit, 0x10u)) return 0;
    const uint32_t vtable = wc3_rd32g(unit);
    if (!vtable ||
        !dt_guest_readable(vtable + DT_UNIT_POSITION_SLOT + 4u, 4u)) {
        return 0;
    }
    const uint32_t fn = wc3_rd32g(vtable + DT_UNIT_POSITION_SLOT);
    if (!fn) return 0;
    const uint32_t holder = dt_call_thiscall(c, unit, fn, 0, NULL);
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;

    const uint32_t out = dt_scratch_alloc();
    if (!out) return 0;
    const uint32_t args[1] = { out };
    (void)dt_call_thiscall(c, holder, DT_FN_UNIT_POSITION, 1, args);

    const float px = dt_float(wc3_rd32g(out + 0u));
    const float py = dt_float(wc3_rd32g(out + 4u));
    dt_scratch_free(out);
    if (!isfinite(px) || !isfinite(py)) return 0;
    *x = px;
    *y = py;
    return 1;
}

/* Compatibility: a TAP queued by an older build.  The current recognizer
 * starts the follow on DOWN and emits no portrait tap.  The native
 * PanCameraTo path clears an active follow first. */
static void dt_portrait_tap(CPU *c, const DtContext *ctx)
{
    if (dt_paused(c)) {
        dt_trace("portrait-tap-paused", 0, 0);
        return;
    }
    const uint32_t camera = dt_camera(ctx->ui);
    if (!camera) {
        dt_trace("portrait-no-camera", 0, 0);
        return;
    }
    const uint32_t unit = dt_portrait_unit(c, ctx);
    if (!unit) {
        dt_trace("portrait-no-unit", 0, 0);
        return;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (!dt_unit_position(c, unit, &x, &y)) {
        dt_trace("portrait-no-position", unit, 0);
        return;
    }
    const uint32_t args[4] = { dt_bits(x), dt_bits(y), 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_PAN, 4, args);
    dt_trace("portrait-pan", unit, 0);
}

/* Follow: native camera follow with a zero offset; it takes its own unit ref.
 * Started immediately on portrait DOWN and held until the owning finger is
 * released, not by a long-press deadline. */
static void dt_portrait_follow_start(CPU *c, const DtContext *ctx)
{
    if (dt_paused(c)) {
        dt_trace("portrait-follow-paused", 0, 0);
        return;
    }
    const uint32_t camera = dt_camera(ctx->ui);
    if (!camera) {
        dt_trace("portrait-follow-no-camera", 0, 0);
        return;
    }
    const uint32_t unit = dt_portrait_unit(c, ctx);
    if (!unit) {
        dt_trace("portrait-follow-no-unit", 0, 0);
        return;
    }
    const uint32_t args[5] = { unit, 0u, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_FOLLOW, 5, args);
    dt_trace("portrait-follow", unit, 0);
}

/* Release: a null unit makes the native follow path drop its reference. */
static void dt_portrait_release(CPU *c)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    const uint32_t camera = dt_camera(ui);
    if (!camera) return;
    const uint32_t args[5] = { 0u, 0u, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, camera, DT_FN_CAMERA_FOLLOW, 5, args);
    dt_trace("portrait-release", 0, 0);
}

static int dt_selection_nonempty(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return 0;
    const int count = (int)dt_call_thiscall(c, selection,
                                             DT_FN_SELECTION_COUNT, 0, NULL);
    return count > 0 && count <= 40;
}

/*
 * True when the live selection can receive an order from the local player.
 * 0x6F66EC60 is the same predicate the native ground/unit order paths use
 * before issuing an order, so it stays correct for team/shared-control edge
 * cases that a raw owner compare would miss.
 */
static int dt_selection_controlled(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection || !dt_selection_nonempty(c, ctx)) return 0;
    return dt_call_thiscall(c, selection, DT_FN_CAN_ORDER, 0, NULL) != 0;
}

static void dt_clear_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return;
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_CLEAR_SELECTION, 1, &mode);
}

/*
 * The tap/long-press owner rule from the analysis document: the local player
 * and the three neutral non-hostile owners select; every other owner is an
 * order target.  This is deliberately not an alliance query.
 */
static int dt_unit_selectable(uint32_t unit)
{
    if (!dt_guest_readable(unit, 0x5Cu)) return 0;
    if (wc3_rd32g(unit + 0x0Cu) != DT_CLASS_UNIT) return 0;

    const uint16_t owner = wc3_rd16g(unit + 0x58u);
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager + 0x2Au, 2)) return 0;
    const uint16_t local = wc3_rd16g(manager + 0x28u);
    return wc3_dt_owner_is_friendly(owner, local);
}

/*
 * Canonical unit-select path.  0x6F395410 checks the pause gate, rejects a
 * right-button event, calls the native 0x6F358B40 thunk and then selects
 * through 0x6F6BCE50([unit+0x6A0]) or the 0x6F6BD380 fallback.  Reusing it
 * keeps the exact native sequence instead of replaying only its tail.
 */
static void dt_select_unit(CPU *c, uint32_t unit, uint32_t event)
{
    if (!dt_guest_readable(unit, 0x6A8u)) {
        dt_trace("select-unreadable", unit, 0);
        return;
    }
    memset(guest_host_ptr(event), 0, 0x14u);
    wc3_wr32g(event + 0x0Cu, unit);
    wc3_wr32g(event + 0x10u, 1u); /* left button: select, not order */
    const uint32_t args[1] = { event };
    (void)wc3_hle_call_guest(c, DT_FN_SELECT_EVENT, 1, args);
}

/*
 * Selected-unit grid helpers.  The native panel only reacts to a completed
 * button click (command 0x50065) and that click arrives on release.  Direct
 * touch activates the subgroup as soon as the finger lands and follows the
 * finger across the grid; in targeting mode the same down is delivered to the
 * target-mode 0x50065 handler, which validates the unit and applies the
 * target.  No synthetic button event is posted.
 */
static void dt_grid_reset(void)
{
    g_grid_active = 0;
    g_grid_pointer = 0;
    g_grid_ui = 0;
    g_grid_selection = 0;
    g_grid_unit = 0;
    g_grid_target = 0;
}

/*
 * Resolve the live UI and selection without requiring the full field context.
 * A modal menu clears [ui+0x1AC]; in that state the grid is not a valid
 * recipient even if its frames are still visible underneath.
 */
static int dt_grid_context(CPU *c, uint32_t *ui_out, uint32_t *selection_out)
{
    if (!c || !ui_out || !selection_out) return 0;
    *ui_out = 0;
    *selection_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;

    if (!dt_guest_readable(DT_PLAYER_MANAGER, 4u)) return 0;
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x60u)) return 0;

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ui = ui;
    ctx.manager = manager;
    const uint32_t selection = dt_selection(c, &ctx);
    if (!selection) return 0;

    *ui_out = ui;
    *selection_out = selection;
    return 1;
}

static int dt_grid_targeting(uint32_t ui)
{
    if (!ui || !dt_guest_readable(ui, DT_UI_TARGET_MODE + 4u)) return 0;
    const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
    if (!target) return 0;
    return wc3_rd32g(ui + DT_UI_ACTIVE_MODE) == target;
}

/* Return the CUnit under the normalised surface point, or 0 when the point is
 * outside the active grid or the cell has no live unit. */
static uint32_t dt_grid_button_at(CPU *c, uint32_t ui, float nx, float ny,
                                  uint32_t width, uint32_t height)
{
    if (!c || !ui || !dt_guest_readable(ui + DT_UI_INFO_BAR, 4u)) return 0;
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar + DT_INFO_GROUP, 4u)) return 0;
    const uint32_t group = wc3_rd32g(infobar + DT_INFO_GROUP);
    if (!group || !dt_guest_readable(group, DT_GROUP_ACTIVE_GRID + 4u)) return 0;
    if (!wc3_rd32g(group + DT_GROUP_VISIBLE)) return 0;

    const uint32_t grid = wc3_rd32g(group + DT_GROUP_ACTIVE_GRID);
    if (!grid || !dt_guest_readable(grid, DT_GRID_ROW_ARRAY + 4u)) return 0;
    if (!wc3_rd32g(grid + DT_GRID_VISIBLE)) return 0;

    const uint32_t rows = wc3_rd32g(grid + DT_GRID_ROWS);
    const uint32_t cols = wc3_rd32g(grid + DT_GRID_COLS);
    if (!rows || !cols || rows > DT_GRID_MAX_ROWS ||
        cols > DT_GRID_MAX_COLS) {
        return 0;
    }
    const uint32_t row_array = wc3_rd32g(grid + DT_GRID_ROW_ARRAY);
    if (!row_array ||
        !dt_guest_readable(row_array, rows * DT_GRID_ROW_STRIDE)) {
        return 0;
    }

    for (uint32_t row_index = 0; row_index < rows; row_index++) {
        const uint32_t row =
            row_array + row_index * DT_GRID_ROW_STRIDE;
        const uint32_t buttons = wc3_rd32g(row + DT_GRID_ROW_BUTTONS);
        if (!buttons || !dt_guest_readable(buttons, cols * 4u)) continue;

        for (uint32_t col = 0; col < cols; col++) {
            const uint32_t button = wc3_rd32g(buttons + col * 4u);
            if (!button ||
                !dt_guest_readable(button, DT_GRID_BUTTON_UNIT + 4u)) {
                continue;
            }
            if (!wc3_rd32g(button + DT_GRID_BUTTON_VISIBLE)) continue;

            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (!dt_frame_rect_snapshot(button, width, height, &left, &top,
                                        &right, &bottom)) {
                continue;
            }
            if (!(nx >= left && nx < right && ny >= top && ny < bottom)) {
                continue;
            }

            const uint32_t ref = wc3_rd32g(button + DT_GRID_BUTTON_UNIT);
            /* The native fill path and the target-mode 0x50065 handler both
             * skip a holder with [holder+0x5C] & 0x100.  Keep the same rule
             * here so a stale visible button cannot select a unit the game
             * already hides. */
            if (!ref || !dt_guest_readable(ref, 0x60u)) continue;
            if (wc3_rd32g(ref + 0x5Cu) & 0x100u) continue;
            const uint32_t unit = dt_handle_unit(c, ref);
            if (unit) return unit;
        }
    }
    return 0;
}

/* The native 0x50065 handler resolves the selection holder from the touched
 * unit as [[unit+0x6C]+0x124] and validates it with 0x6F0549F0.  The grid
 * button stores a unit holder, not the class instance, so dt_grid_button_at()
 * resolves it first and this function derives the same holder the native
 * handler uses. */
static uint32_t dt_grid_holder(CPU *c, uint32_t unit)
{
    if (!c || !unit ||
        !dt_guest_readable(unit + DT_UNIT_HOLDER_SUB + 4u, 4u)) {
        return 0;
    }
    const uint32_t sub = wc3_rd32g(unit + DT_UNIT_HOLDER_SUB);
    if (!sub ||
        !dt_guest_readable(sub + DT_UNIT_HOLDER_REF + 4u, 4u)) {
        return 0;
    }
    const uint32_t holder = wc3_rd32g(sub + DT_UNIT_HOLDER_REF);
    if (!holder || !dt_guest_readable(holder, 0x14u)) return 0;
    if (!dt_call_thiscall(c, holder, DT_FN_HOLDER_ALIVE, 0, NULL)) return 0;
    return holder;
}

/* Native single-unit selection: 0x6F396CF0(manager, holder, 0, 0, 0).  The
 * manager is [CGameUI+0x24C], exactly what the native 0x50065 branch uses. */
static void dt_grid_select_unit(CPU *c, uint32_t ui, uint32_t holder)
{
    if (!c || !ui || !holder) return;
    if (!dt_guest_readable(ui + DT_UI_MANAGER, 4u)) return;
    const uint32_t manager = wc3_rd32g(ui + DT_UI_MANAGER);
    if (!manager || !dt_guest_readable(manager, 0x60u)) return;

    const uint32_t args[4] = { holder, 0u, 0u, 0u };
    (void)dt_call_thiscall(c, manager, DT_FN_GRID_SELECT_UNIT, 4, args);
    dt_trace("grid-select-unit", holder, manager);
}

/* The native 0x50065 subgroup branch closes command-bar pages and hides the
 * old command buttons before 0x6F293480, then marks the UI dirty after
 * 0x6F291240.  Without that the command card can keep the previous page's
 * buttons after a direct-touch subgroup switch.  The native assert helper
 * 0x6F359350 is debug-only, and the target-mode cancel before the hide cannot
 * fire here because dt_grid_targeting() already routed that case.  The native
 * pop loop is unbounded; cap it so a corrupt mode count cannot hang the
 * guest. */
static void dt_grid_prepare_command_bar(CPU *c, uint32_t ui)
{
    if (!c || !ui || !dt_guest_readable(ui + DT_UI_COMMAND_BAR, 4u)) return;
    const uint32_t bar = wc3_rd32g(ui + DT_UI_COMMAND_BAR);
    if (!bar || !dt_guest_readable(bar, 0x1C0u)) return;

    for (int i = 0; i < 8; i++) {
        if (!dt_call_thiscall(c, bar, DT_FN_COMMAND_BAR_POP, 0, NULL)) break;
    }
    (void)dt_call_thiscall(c, bar, DT_FN_COMMAND_BAR_HIDE, 0, NULL);
}

/* Native 0x50065: a unit already in the active subgroup is selected
 * individually; otherwise 0x6F293480 makes its subgroup current and the
 * native command follows that with 0x6F291240(selection, 0).  Never call the
 * realize alone: it is a mirror update, not a selection change.
 *
 * The native handler also consults the player's selection options through
 * 0x6F0731A0(0)/(1): option 0 forces individual selection, and option 1
 * switches to the multi-unit 0x6F3968B0 branch.  Direct touch intentionally
 * follows the standard option-1-off "activate subgroup, then select the
 * individual unit" rule requested for the panel; the option overrides are
 * not reproduced. */
static void dt_grid_activate(CPU *c, uint32_t ui, uint32_t selection,
                             uint32_t unit)
{
    if (!c || !selection || !unit) return;

    const uint32_t holder = dt_grid_holder(c, unit);
    if (!holder) return;
    if (!dt_guest_readable(selection + DT_SELECTION_ACTIVE_GROUP, 4u)) return;

    const uint32_t active =
        wc3_rd32g(selection + DT_SELECTION_ACTIVE_GROUP);
    if (active && dt_guest_readable(active, 0x10u)) {
        const uint32_t arg = holder;
        if (dt_call_thiscall(c, active, DT_FN_SUBGROUP_HAS_UNIT, 1, &arg)) {
            /* Legacy semantics: the first touch activates the subgroup, a
             * touch on an already-active subgroup selects that unit. */
            dt_grid_select_unit(c, ui, holder);
            return;
        }
    }

    const uint32_t arg = holder;
    dt_grid_prepare_command_bar(c, ui);
    (void)dt_call_thiscall(c, selection, DT_FN_GRID_ACTIVATE, 1, &arg);
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_SELECTION_REALIZE, 1, &mode);
    (void)dt_call_thiscall(c, 0u, DT_FN_UI_DIRTY, 0, NULL);
    dt_trace("grid-activate", holder,
             wc3_rd32g(selection + DT_SELECTION_ACTIVE_GROUP));
}

/* Target-mode 0x50065: event+0xC is the CUnit, event+0x10 is an optional
 * world-position pointer (null lets the handler use the unit position). */
static void dt_grid_target(CPU *c, uint32_t target_mode, uint32_t unit)
{
    if (!c || !target_mode || !unit) return;

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return;
    const uint32_t event = scratch + DT_EVENT_OFFSET;
    memset(guest_host_ptr(event), 0, 0x14u);
    wc3_wr32g(event + 0x08u, DT_EVENT_ID_GRID_TARGET);
    wc3_wr32g(event + 0x0Cu, unit);
    wc3_wr32g(event + 0x10u, 0u);
    const uint32_t args[1] = { event };
    (void)dt_call_thiscall(c, target_mode, DT_FN_TARGET_UNIT, 1, args);
    dt_scratch_free(scratch);
    dt_trace("grid-target", unit, target_mode);
}

/*
 * Consume a native-route pointer gesture that started on the selected-unit
 * grid.  Returns 1 when the event belongs to the grid and must not be posted
 * as a native mouse message.  Only the pointer that landed on the grid keeps
 * ownership; the rest of the gesture is consumed even after it leaves the
 * buttons, so the native UI never sees a MOVE/UP without its DOWN.
 */
static int dt_grid_pointer(CPU *c, const Wc3DirectTouchEvent *ev,
                           uint32_t width, uint32_t height)
{
    if (!c || !ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_grid_active) return 1;

        uint32_t ui = 0;
        uint32_t selection = 0;
        if (!dt_grid_context(c, &ui, &selection)) return 0;
        const uint32_t unit =
            dt_grid_button_at(c, ui, ev->x, ev->y, width, height);
        if (!unit) return 0;

        g_grid_active = 1;
        g_grid_pointer = ev->pointer_id;
        g_grid_ui = ui;
        g_grid_selection = selection;
        g_grid_unit = unit;
        g_grid_target = 0;

        if (dt_grid_targeting(ui)) {
            const uint32_t target = wc3_rd32g(ui + DT_UI_TARGET_MODE);
            dt_grid_target(c, target, unit);
            g_grid_target = 1;
        } else {
            dt_grid_activate(c, ui, selection, unit);
        }
        dt_trace("grid-down", unit, g_grid_target);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE: {
        if (!g_grid_active || ev->pointer_id != g_grid_pointer) return 0;
        if (g_grid_target) return 1;

        const uint32_t unit =
            dt_grid_button_at(c, g_grid_ui, ev->x, ev->y, width, height);
        if (unit && unit != g_grid_unit) {
            g_grid_unit = unit;
            dt_grid_activate(c, g_grid_ui, g_grid_selection, unit);
            dt_trace("grid-move", unit, 0);
        }
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_grid_active || ev->pointer_id != g_grid_pointer) return 0;
        dt_trace("grid-up", g_grid_unit, g_grid_target);
        dt_grid_reset();
        return 1;
    default:
        return 0;
    }
}

static void dt_order_ground(CPU *c, const DtContext *ctx, uint32_t wx_bits,
                            uint32_t wy_bits)
{
    const uint32_t args[2] = { wx_bits, wy_bits };
    (void)dt_call_thiscall(c, ctx->mode, DT_FN_GROUND_ORDER, 2, args);
}

/*
 * Build the same CGhostSpriteClickEvent OnSelect builds for a unit, then let
 * the native dispatcher validate the target and issue the smart order.  The
 * event lives only on our guest scratch and is never posted to a queue.
 *
 * event+0x38 must be the raw pick object, not our validated CUnit: the
 * handler validates it again with 0x6F21B730 and takes its own reference,
 * exactly as the native OnSelect path does.  Our reference, when the caller
 * holds one for target inspection, only keeps the raw object alive.
 */
static void dt_order_unit(CPU *c, const DtContext *ctx, uint32_t scratch,
                          uint32_t object, uint32_t candidate,
                          uint32_t wx_bits, uint32_t wy_bits,
                          uint32_t wz_bits)
{
    const uint32_t event = scratch + DT_EVENT_OFFSET;
    memset(guest_host_ptr(event), 0, DT_EVENT_SIZE);

    const uint32_t event_id = DT_EVENT_ID_GHOST;
    (void)dt_call_thiscall(c, event, DT_FN_GHOST_INIT, 1, &event_id);

    wc3_wr32g(event + 0x1Cu, wx_bits);
    wc3_wr32g(event + 0x20u, wy_bits);
    wc3_wr32g(event + 0x24u, wz_bits);
    wc3_wr32g(event + 0x28u, 4u);              /* right button: smart order */
    wc3_wr32g(event + 0x34u, candidate);
    wc3_wr32g(event + 0x38u, object);

    (void)dt_call_thiscall(c, ctx->mode, DT_FN_CLICK_DISPATCH, 1, &event);
}

static void dt_tap(CPU *c, const DtContext *ctx, const Wc3DirectTouchEvent *ev,
                   uint32_t width, uint32_t height)
{
    if (dt_paused(c)) {
        dt_trace("tap-paused", 0, 0);
        return;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return;

    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t unit_out = scratch + DT_UNIT_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_scratch_free(scratch);
        return;
    }

    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 1u, &object, &wx, &wy, &wz);

    const int nonempty = (r == 1 || r == 2)
        ? dt_selection_nonempty(c, ctx) : 0;

    if (r == 1) {
        const uint32_t unit = dt_validate_object(c, object, unit_out);
        if (unit) {
            /* Validation guarantees a live object, but keep the class read
             * inside a readable window: a corrupted object must not fault in
             * the executor before the native release path sees it. */
            const int readable = dt_guest_readable(unit, 0x10u);
            const int is_unit = readable &&
                wc3_rd32g(unit + 0x0Cu) == DT_CLASS_UNIT;
            const int selectable = is_unit && dt_unit_selectable(unit);
            /* The selection state, not the target owner alone, decides
             * whether this tap selects or orders. */
            const int controlled = is_unit && dt_selection_controlled(c, ctx);
            switch (wc3_dt_tap_intent(r, 1, is_unit, selectable,
                                      nonempty, controlled)) {
            case WC3_DT_INTENT_ORDER_UNIT:
                /* Keep the raw pick pointer in the event; the ghost
                 * handler validates it again and takes its own ref. */
                dt_order_unit(c, ctx, scratch, object,
                              wc3_rd32g(pick + 0x08u), wx, wy, wz);
                dt_trace("tap-order-unit", unit, object);
                break;
            case WC3_DT_INTENT_SELECT_UNIT:
                dt_select_unit(c, unit, scratch + DT_EVENT_OFFSET);
                dt_trace("tap-select-unit", unit, 0);
                break;
            case WC3_DT_INTENT_NONE:
                /* A non-unit sprite has no phase-1 selection/order action;
                 * do not turn it into a ground order. */
                dt_trace("tap-non-unit", object,
                         readable ? wc3_rd32g(unit + 0x0Cu) : 0);
                break;
            default:
                break;
            }
            dt_release_object(c, unit);
            dt_scratch_free(scratch);
            return;
        }
        /* An object that no longer validates is the native "invalid sprite"
         * case: with select mode active and a left button, OnSelect
         * substitutes a terrain click.  Fall through to the world point. */
    }

    if (r == 1 || r == 2) {
        switch (wc3_dt_tap_intent(r, 0, 0, 0, nonempty, 0)) {
        case WC3_DT_INTENT_ORDER_GROUND:
            dt_order_ground(c, ctx, wx, wy);
            dt_trace("tap-order-ground", wx, wy);
            break;
        case WC3_DT_INTENT_CLEAR_SELECTION:
            dt_clear_selection(c, ctx);
            dt_trace("tap-clear", 0, 0);
            break;
        default:
            break;
        }
    }

    dt_scratch_free(scratch);
}

static void dt_long_press(CPU *c, const DtContext *ctx,
                          const Wc3DirectTouchEvent *ev,
                          uint32_t width, uint32_t height)
{
    if (dt_paused(c)) {
        dt_trace("long-paused", 0, 0);
        return;
    }

    const uint32_t scratch = dt_scratch_alloc();
    if (!scratch) return;

    const uint32_t pick = scratch + DT_PICK_OFFSET;
    const uint32_t unit_out = scratch + DT_UNIT_OFFSET;
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_scratch_free(scratch);
        return;
    }

    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 1u, &object, &wx, &wy, &wz);

    uint32_t unit = 0;
    if (r == 1) unit = dt_validate_object(c, object, unit_out);
    const int readable = unit && dt_guest_readable(unit, 0x10u);
    const int is_unit = readable &&
        wc3_rd32g(unit + 0x0Cu) == DT_CLASS_UNIT;
    const int selectable = is_unit && dt_unit_selectable(unit);

    switch (wc3_dt_long_press_intent(r, unit != 0, is_unit, selectable)) {
    case WC3_DT_LONG_INTENT_SELECT_UNIT:
        dt_select_unit(c, unit, scratch + DT_EVENT_OFFSET);
        dt_trace("long-select-unit", unit, 0);
        break;
    case WC3_DT_LONG_INTENT_ORDER_UNIT:
        /* Hostile target: keep the force-attack order.  The ghost handler
         * validates the raw object and falls back to the world point. */
        dt_order_unit(c, ctx, scratch, object,
                      wc3_rd32g(pick + 0x08u), wx, wy, wz);
        dt_trace("long-order-unit", object, 0);
        break;
    case WC3_DT_LONG_INTENT_CLEAR_SELECTION:
        dt_clear_selection(c, ctx);
        dt_trace("long-clear", 0, 0);
        break;
    default:
        break;
    }

    if (unit) dt_release_object(c, unit);
    dt_scratch_free(scratch);
}

static int dt_is_native_pointer(uint32_t kind)
{
    return kind == WC3_DIRECT_TOUCH_POINTER_DOWN ||
           kind == WC3_DIRECT_TOUCH_POINTER_MOVE ||
           kind == WC3_DIRECT_TOUCH_POINTER_UP ||
           kind == WC3_DIRECT_TOUCH_POINTER_CANCEL;
}

static void dt_native_release(void)
{
    if (!g_native_down) return;
    hle_android_mouse_button_ex(0, 0);
    g_native_down = 0;
    g_native_up_pending = 1;
}

/*
 * Native UI input.  The event coordinates are surface-normalised, exactly
 * what hle_android_mouse_move expects.  Posting WM_MOUSEMOVE/WM_LBUTTONDOWN/
 * WM_LBUTTONUP is the same path a real mouse uses; it does not touch the
 * overlay touch engine.  The guest window procedure then runs the minimap,
 * command card, portrait, resource bar and targeting/placement handlers with
 * their native semantics.
 */
static void dt_native_pointer(const Wc3DirectTouchEvent *ev)
{
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
        if (g_native_up_pending && !hle_android_mouse_is_down(0)) {
            /* The queued release has been dispatched; a normal DOWN will do. */
            g_native_up_pending = 0;
        }
        if (g_native_down ||
            (!g_native_up_pending && hle_android_mouse_is_down(0))) {
            /* The overlay engine already owns the left button, or a previous
             * native press has not been released yet.  Do not post a second
             * DOWN: the matching UP would then cancel the other owner. */
            dt_trace("native-down-busy", ev->pointer_id, 0);
            break;
        }
        hle_android_mouse_move(ev->x, ev->y);
        if (g_native_up_pending) {
            /* Our own UP is queued but not dispatched yet.  The queue order
             * keeps it ahead of this DOWN, so the guest still sees a clean
             * DOWN/UP pair instead of losing a rapid second tap. */
            hle_android_mouse_button_down_force(0);
        } else {
            hle_android_mouse_button_ex(0, 1);
        }
        g_native_down = 1;
        g_native_up_pending = 0;
        dt_trace("native-down", dt_bits(ev->x), dt_bits(ev->y));
        break;
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        hle_android_mouse_move(ev->x, ev->y);
        break;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        hle_android_mouse_move(ev->x, ev->y);
        if (g_native_down) {
            hle_android_mouse_button_ex(0, 0);
            g_native_down = 0;
            g_native_up_pending = 1;
        }
        dt_trace("native-up", dt_bits(ev->x), dt_bits(ev->y));
        break;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        dt_native_release();
        dt_trace("native-cancel", ev->pointer_id, 0);
        break;
    default:
        break;
    }
}

/*
 * A semantic gesture can be recognized just as the game enters targeting or
 * placement.  The controls layer decided FIELD on DOWN, so the event arrives
 * as TAP/LONG_PRESS, but the native UI now owns the click.  Replay it as a
 * native click instead of dropping the user's target selection.
 */
static void dt_native_click(const Wc3DirectTouchEvent *ev)
{
    Wc3DirectTouchEvent pointer = *ev;
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_DOWN;
    dt_native_pointer(&pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(&pointer);
}

/*
 * Inventory item gestures.  The native inventory button registers 0x30065:
 * a left click uses the item, a right click enters move mode, and the next
 * left click on a slot or in the world confirms the move.  Direct touch
 * consumes a gesture that starts on an occupied slot and replays that click
 * pair on release.  Item use, swap and drop then stay on the exact native
 * path instead of re-deriving the target-mode fields.
 *
 * Only the pointer that landed on the item owns the gesture.  A second DOWN
 * is swallowed; a MOVE/UP from another pointer is forwarded so a native
 * button cannot stay held.
 */
static void dt_inventory_reset(void)
{
    g_inv_active = 0;
    g_inv_pointer = 0;
    g_inv_bar = 0;
    g_inv_slot = 0;
    g_inv_down_x = 0.0f;
    g_inv_down_y = 0.0f;
}

/* Resolve CInfoBar+0x148 -> CInventoryBar from the live UI singleton. */
static uint32_t dt_inventory_live_bar(void)
{
    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    const uint32_t infobar = wc3_rd32g(ui + DT_UI_INFO_BAR);
    if (!infobar || !dt_guest_readable(infobar, DT_INFO_INVENTORY + 4u)) {
        return 0;
    }
    return wc3_rd32g(infobar + DT_INFO_INVENTORY);
}

/* Resolve the live inventory bar only in the normal in-game UI.  Target mode
 * owns the click while it is active, so a gesture there is forwarded to the
 * native handler unchanged. */
static int dt_inventory_context(uint32_t *bar_out)
{
    if (!bar_out) return 0;
    *bar_out = 0;

    if (!dt_guest_readable(DT_UI_GLOBAL, 4u)) return 0;
    const uint32_t ui = wc3_rd32g(DT_UI_GLOBAL);
    if (!ui || !dt_guest_readable(ui, DT_UI_INFO_BAR + 4u)) return 0;
    if (!wc3_rd32g(ui + DT_UI_WORLD_FRAME)) return 0;
    if (!wc3_rd32g(ui + DT_UI_CLICK_GATE)) return 0;
    if (!wc3_rd32g(ui + DT_UI_SELECT_MODE)) return 0;
    if (dt_grid_targeting(ui)) return 0;

    const uint32_t bar = dt_inventory_live_bar();
    if (!bar || !dt_guest_readable(bar, DT_INV_ARRAY + 4u)) return 0;
    const uint32_t cap = wc3_rd32g(bar + DT_INV_CAP);
    if (!cap || cap > DT_INV_MAX_SLOTS) return 0;

    *bar_out = bar;
    return 1;
}

static int dt_inventory_has_item(uint32_t entry, uint32_t button)
{
    if (wc3_rd32g(entry + DT_INV_ENTRY_ITEM)) return 1;
    const uint32_t data = wc3_rd32g(button + DT_INV_BUTTON_DATA);
    if (!data || !dt_guest_readable(data, DT_INV_DATA_ITEM + 4u)) return 0;
    return wc3_rd32g(data + DT_INV_DATA_ITEM) != 0;
}

/*
 * Return the slot under the point, or -1.  need_item restricts the search to
 * occupied slots for the gesture source; the destination search accepts an
 * empty slot so a drag can move the item into it.  The rect is read without
 * the frame visibility test because the six inventory buttons stay laid out
 * even when an individual slot has no item icon.
 */
static int dt_inventory_slot_at(uint32_t bar, float nx, float ny,
                                uint32_t width, uint32_t height,
                                int need_item, uint32_t *button_out)
{
    if (button_out) *button_out = 0;
    if (!bar || !dt_guest_readable(bar, DT_INV_ARRAY + 4u)) return -1;
    const uint32_t cap = wc3_rd32g(bar + DT_INV_CAP);
    if (!cap || cap > DT_INV_MAX_SLOTS) return -1;
    const uint32_t array = wc3_rd32g(bar + DT_INV_ARRAY);
    if (!array || !dt_guest_readable(array, cap * 8u)) return -1;

    for (uint32_t i = 0; i < cap; i++) {
        const uint32_t entry = array + i * 8u;
        const uint32_t button = wc3_rd32g(entry + DT_INV_ENTRY_BUTTON);
        if (!button ||
            !dt_guest_readable(button, DT_INV_BUTTON_DATA + 4u)) {
            continue;
        }
        if (need_item && !dt_inventory_has_item(entry, button)) continue;

        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (!dt_frame_rect_read(button, width, height, &left, &top,
                                &right, &bottom)) {
            continue;
        }
        if (!(nx >= left && nx < right && ny >= top && ny < bottom)) {
            continue;
        }
        if (button_out) *button_out = button;
        return (int)i;
    }
    return -1;
}

static int dt_inventory_field_contains(float nx, float ny)
{
    return nx >= DT_INV_FIELD_LEFT && nx <= DT_INV_FIELD_RIGHT &&
           ny >= DT_INV_FIELD_TOP && ny <= DT_INV_FIELD_BOTTOM;
}

/* Replay a native left click.  dt_native_pointer keeps the pending-release
 * bookkeeping correct, so a tap right after another native click is not
 * dropped by the logical button state. */
static void dt_inventory_left_click(float x, float y)
{
    Wc3DirectTouchEvent pointer;
    memset(&pointer, 0, sizeof pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_DOWN;
    pointer.x = x;
    pointer.y = y;
    dt_native_pointer(&pointer);
    pointer.kind = WC3_DIRECT_TOUCH_POINTER_UP;
    dt_native_pointer(&pointer);
}

/* Right click on the source item enters the native move mode. */
static void dt_inventory_right_click(float x, float y)
{
    if (g_inv_right_pending && !hle_android_mouse_is_down(2)) {
        g_inv_right_pending = 0;
    }
    hle_android_mouse_move(x, y);
    if (g_inv_right_pending) {
        hle_android_mouse_button_down_force(2);
    } else {
        hle_android_mouse_button_ex(2, 1);
    }
    hle_android_mouse_button_ex(2, 0);
    g_inv_right_pending = 1;
}

/*
 * Resolve the release.  Same slot is a tap and uses the item; another slot
 * is a swap/move confirmed by a left click on that slot; a field point is a
 * drop confirmed by a left click at the world position.  Anywhere else
 * cancels, so a release over the command card or a HUD panel never turns
 * into a world order.
 */
static void dt_inventory_finish(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    const uint32_t bar = g_inv_bar;
    const uint32_t src_slot = g_inv_slot;
    const float down_x = g_inv_down_x;
    const float down_y = g_inv_down_y;

    /* The UI may have been replaced between DOWN and UP.  Never post a click
     * into a new inventory bar or a menu, and never start a move from a slot
     * that no longer holds an item. */
    uint32_t live_bar = 0;
    if (!dt_inventory_context(&live_bar) || live_bar != bar) {
        dt_trace("inv-context", src_slot, live_bar);
        dt_inventory_reset();
        return;
    }
    if (dt_inventory_slot_at(bar, down_x, down_y, width, height, 1,
                             NULL) != (int)src_slot) {
        dt_trace("inv-source", src_slot, 0);
        dt_inventory_reset();
        return;
    }

    uint32_t dest_button = 0;
    const int dest_slot = dt_inventory_slot_at(bar, ev->x, ev->y, width,
                                               height, 0, &dest_button);
    dt_inventory_reset();

    if (dest_slot == (int)src_slot) {
        dt_inventory_left_click(down_x, down_y);
        dt_trace("inv-tap", src_slot, 0);
        return;
    }
    if (dest_slot >= 0 && dest_button) {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (dt_frame_rect_read(dest_button, width, height, &left, &top,
                               &right, &bottom)) {
            dt_inventory_right_click(down_x, down_y);
            dt_inventory_left_click((left + right) * 0.5f,
                                    (top + bottom) * 0.5f);
            dt_trace("inv-slot", src_slot, (uint32_t)dest_slot);
            return;
        }
    }
    if (dt_inventory_field_contains(ev->x, ev->y)) {
        dt_inventory_right_click(down_x, down_y);
        dt_inventory_left_click(ev->x, ev->y);
        dt_trace("inv-drop", src_slot, 0);
        return;
    }
    dt_trace("inv-cancel", src_slot, 0);
}

static int dt_inventory_pointer(const Wc3DirectTouchEvent *ev,
                                uint32_t width, uint32_t height)
{
    if (!ev || ev->route != WC3_DIRECT_TOUCH_ROUTE_NATIVE) return 0;

    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN: {
        if (g_inv_active) return 1;
        /* A grid gesture already owns the screen contact; let the grid
         * handler swallow this second DOWN instead of starting a second
         * gesture here. */
        if (g_grid_active) return 0;

        uint32_t bar = 0;
        if (!dt_inventory_context(&bar)) return 0;
        const int slot = dt_inventory_slot_at(bar, ev->x, ev->y, width,
                                              height, 1, NULL);
        if (slot < 0) return 0;

        g_inv_active = 1;
        g_inv_pointer = ev->pointer_id;
        g_inv_bar = bar;
        g_inv_slot = (uint32_t)slot;
        g_inv_down_x = ev->x;
        g_inv_down_y = ev->y;
        dt_trace("inv-down", (uint32_t)slot, bar);
        return 1;
    }
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_UP:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        dt_inventory_finish(ev, width, height);
        return 1;
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (!g_inv_active || ev->pointer_id != g_inv_pointer) return 0;
        dt_trace("inv-abort", g_inv_slot, 0);
        dt_inventory_reset();
        return 1;
    default:
        return 0;
    }
}

static void dt_dispatch(CPU *c, const DtContext *ctx,
                        const Wc3DirectTouchEvent *ev,
                        uint32_t width, uint32_t height)
{
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_TAP:
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            dt_portrait_tap(c, ctx);
        } else {
            dt_tap(c, ctx, ev, width, height);
        }
        break;
    case WC3_DIRECT_TOUCH_LONG_PRESS:
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            /* Compatibility for an event queued by an older build.  The
             * current recognizer starts the portrait follow on DOWN. */
            dt_portrait_follow_start(c, ctx);
        } else {
            dt_long_press(c, ctx, ev, width, height);
        }
        break;
    case WC3_DIRECT_TOUCH_FOLLOW_START:
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            dt_portrait_follow_start(c, ctx);
        }
        break;
    case WC3_DIRECT_TOUCH_HOLD_RELEASE:
        dt_portrait_release(c);
        break;
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_POINTER_MOVE:
    case WC3_DIRECT_TOUCH_POINTER_UP:
    case WC3_DIRECT_TOUCH_POINTER_CANCEL:
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_inventory_pointer(ev, width, height)) {
            break;
        }
        if (ev->route == WC3_DIRECT_TOUCH_ROUTE_NATIVE &&
            dt_grid_pointer(c, ev, width, height)) {
            break;
        }
        dt_native_pointer(ev);
        break;
    default:
        dt_trace("unknown-kind", ev->kind, ev->pointer_id);
        break;
    }
}

static void dt_request_main_thread_wake(void *user)
{
    (void)user;
    wc3_android_request_direct_touch();
}

void wc3_hle_direct_touch_start(void)
{
    g_main_thread = pthread_self();
    g_main_thread_valid = 1;
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_portrait_release_pending, 0, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
    g_last_ui = 0;
    g_last_world = 0;
    wc3_direct_touch_clear();
    wc3_direct_touch_set_wake(dt_request_main_thread_wake, NULL);
}

void wc3_hle_direct_touch_enqueue(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    if (!event) return;
    (void)wc3_direct_touch_push(event);
}

void wc3_hle_direct_touch_reset(void)
{
    __atomic_store_n(&g_portrait_release_pending, 1, __ATOMIC_RELEASE);
    dt_grid_reset();
    dt_inventory_reset();
    dt_native_release();
    dt_portrait_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_minimap_publish(0.0f, 0.0f, 0.0f, 0.0f);
    dt_publish_route(WC3_DIRECT_TOUCH_ROUTE_LEGACY);
    wc3_direct_touch_clear();
}

int wc3_hle_direct_touch_route(void)
{
    return __atomic_load_n(&g_route, __ATOMIC_ACQUIRE);
}

int wc3_hle_direct_touch_can_service(void)
{
    if (!g_main_thread_valid) return 0;
    if (!pthread_equal(pthread_self(), g_main_thread)) return 0;
    return !__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE);
}

void wc3_hle_direct_touch_service(CPU *c, uint32_t width, uint32_t height)
{
    if (!c || !wc3_hle_direct_touch_can_service()) return;
    if (__atomic_exchange_n(&g_portrait_release_pending, 0,
                            __ATOMIC_ACQ_REL)) {
        dt_portrait_release(c);
    }
    if (wc3_direct_touch_pending() == 0) {
        dt_sync_idle_context(width, height);
        return;
    }

    __atomic_store_n(&g_busy, 1, __ATOMIC_RELEASE);

    /* Pointer-only markers: dt_context() fails in menus and loading screens,
     * but a native menu click must still be serviced.  Keep the last non-zero
     * UI/world pointers; only a real replacement is a map transition. */
    uint32_t cur_ui = 0;
    uint32_t cur_world = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4) &&
        dt_guest_readable(DT_WORLD_GLOBAL, 4)) {
        cur_ui = wc3_rd32g(DT_UI_GLOBAL);
        cur_world = wc3_rd32g(DT_WORLD_GLOBAL);
    }
    const int context_changed =
        (g_last_ui && cur_ui && g_last_ui != cur_ui) ||
        (g_last_world && cur_world && g_last_world != cur_world);
    if (cur_ui) g_last_ui = cur_ui;
    if (cur_world) g_last_world = cur_world;

    /* The portrait and minimap frames can change with a selection or a UI
     * transition; publish them before resolving the batch so the next touch
     * sees the new UI. */
    dt_portrait_snapshot(cur_ui, width, height);
    dt_minimap_snapshot(cur_ui, width, height);

    if (context_changed) {
        /* Queued semantic events belong to the previous map.  Native events
         * are cleared too so a stale DOWN cannot leak a held button into the
         * new UI; the age bound below is the backstop when the pointers were
         * reused. */
        dt_trace("context-change", g_last_ui, g_last_world);
        dt_grid_reset();
        dt_inventory_reset();
        dt_native_release();
        /* A queued HOLD_RELEASE may be dropped with the batch; clear the
         * camera follow before the old context goes away. */
        dt_portrait_release(c);
        wc3_direct_touch_clear();
    }

    DtContext ctx;
    memset(&ctx, 0, sizeof ctx);
    Wc3DirectTouchRoute route = dt_ready_route(c, &ctx);
    dt_publish_service_route(route);

    for (uint32_t i = 0; i < DT_MAX_EVENTS_PER_PUMP; i++) {
        Wc3DirectTouchEvent event;
        if (!wc3_direct_touch_pop(&event)) break;

        /* The input thread may have queued this event just before the game
         * left the map.  The context checks catch most transitions; the age
         * bound is the backstop when a map reload reuses the same UI/world
         * objects and the guest thread was blocked through the transition. */
        const uint64_t now_ms = dt_now_ms();
        if (now_ms && event.time_ms && now_ms > event.time_ms &&
            now_ms - event.time_ms > DT_EVENT_MAX_AGE_MS) {
            dt_trace("stale-event", event.kind, event.pointer_id);
            if (dt_is_native_pointer(event.kind)) {
                dt_grid_reset();
                dt_inventory_reset();
                dt_native_release();
            }
            continue;
        }

        if (event.kind == WC3_DIRECT_TOUCH_HOLD_RELEASE) {
            /* A portrait hold ends even if the UI moved to a menu: the
             * release only needs the current camera, not the semantic
             * context. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (dt_is_native_pointer(event.kind)) {
            /* Native UI input is independent of the semantic context: menus,
             * loading screens, HUD and targeting all use it. */
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (event.route == WC3_DIRECT_TOUCH_ROUTE_PORTRAIT) {
            /* Portrait follow/release resolves against the portrait's
             * displayed unit, not the field pick.  It still needs the
             * in-game context; in a menu the event belongs to the previous
             * map. */
            if (ctx.ui && ctx.world) {
                dt_dispatch(c, &ctx, &event, width, height);
            } else {
                dt_trace("portrait-drop", event.kind, event.pointer_id);
            }
        } else if (route == WC3_DIRECT_TOUCH_ROUTE_FIELD) {
            dt_dispatch(c, &ctx, &event, width, height);
        } else if (route == WC3_DIRECT_TOUCH_ROUTE_NATIVE) {
            /* The gesture was classified as a field tap before the game
             * entered targeting/placement.  Let the native click handler
             * consume it instead of losing the target. */
            dt_trace("semantic-native", event.kind, event.pointer_id);
            dt_native_click(&event);
        } else {
            /* Not in game: the controls layer already routes menus as native
             * pointer phases, so a semantic event here belongs to a stale
             * map/menu transition. */
            dt_trace("semantic-drop", event.kind, event.pointer_id);
        }

        /* Re-evaluate after every event.  A semantic action can enter
         * targeting or load a map; a native click can dismiss a modal or
         * start a cinematic. */
        DtContext next_ctx;
        memset(&next_ctx, 0, sizeof next_ctx);
        route = dt_ready_route(c, &next_ctx);
        dt_publish_service_route(route);

        if (next_ctx.ui != ctx.ui || next_ctx.world != ctx.world) {
            dt_trace("batch-context-change", ctx.ui ^ next_ctx.ui,
                     ctx.world ^ next_ctx.world);
            if (next_ctx.ui) g_last_ui = next_ctx.ui;
            if (next_ctx.world) g_last_world = next_ctx.world;
            dt_grid_reset();
            dt_inventory_reset();
            dt_native_release();
            /* The pending HOLD_RELEASE belongs to the old context; release
             * the follow now instead of leaving it latched on the camera. */
            dt_portrait_release(c);
            wc3_direct_touch_clear();
            break;
        }
        ctx = next_ctx;
    }

    /* A native click can dismiss a modal or open a menu; refresh the frames
     * once at the end of the batch so the input thread sees the new UI. */
    uint32_t final_ui = 0;
    if (dt_guest_readable(DT_UI_GLOBAL, 4u)) {
        final_ui = wc3_rd32g(DT_UI_GLOBAL);
    }
    dt_portrait_snapshot(final_ui, width, height);
    dt_minimap_snapshot(final_ui, width, height);

    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}
