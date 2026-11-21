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
#define DT_UI_TARGET_MODE  0x00000210u
#define DT_UI_SELECT_MODE  0x00000214u
#define DT_UI_DRAG_MODE    0x00000218u
#define DT_UI_ALT_MODE     0x00000220u

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
#define DT_FN_SELECT_UNIT      0x6F6BCE50u
#define DT_FN_SELECT_FALLBACK  0x6F6BD380u
#define DT_FN_CLEAR_SELECTION  0x6F291240u
#define DT_FN_GHOST_INIT       0x6F37C550u
#define DT_FN_CLICK_DISPATCH   0x6F392FA0u
#define DT_FN_GROUND_ORDER     0x6F394680u

#define DT_EVENT_ID_TERRAIN_GATE 0x1A0068u
#define DT_EVENT_ID_GHOST        0x1A0065u

#define DT_CLASS_UNIT 0x2B61676Cu

#define DT_SCRATCH_SIZE 0x100u
#define DT_PICK_OFFSET  0x00u
#define DT_POINT_OFFSET 0x30u
#define DT_UNIT_OFFSET  0x40u
#define DT_EVENT_OFFSET 0x50u
#define DT_EVENT_SIZE   0x40u

#define DT_MAX_EVENTS_PER_PUMP 4u
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
/* Last context the service dispatched against.  A change means the queue may
 * still hold events from the previous map/menu even if the transition itself
 * was never observed (the guest thread was not pumping). */
static uint32_t g_last_ui;
static uint32_t g_last_world;

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

/* Two-register thiscall: 0x6F6BD380 takes its handle in ecx and its object
 * pointer in edx and reads no stack arguments. */
static uint32_t dt_call_thiscall_edx(CPU *c, uint32_t this_va, uint32_t edx_va,
                                     uint32_t fn)
{
    c->ecx = this_va;
    c->edx = edx_va;
    return wc3_hle_call_guest(c, fn, 0, NULL);
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
static void dt_sync_idle_context(void)
{
    uint32_t ui = 0;
    uint32_t world = 0;
    if (!dt_guest_readable(DT_UI_GLOBAL, 4) ||
        !dt_guest_readable(DT_WORLD_GLOBAL, 4)) {
        return;
    }
    ui = wc3_rd32g(DT_UI_GLOBAL);
    world = wc3_rd32g(DT_WORLD_GLOBAL);
    /* A partial context is a transition in progress.  Keep the previous
     * marker: an event produced now (for example a tap on the loading
     * screen) must still compare against the old map and be dropped. */
    if (!ui || !world) return;
    g_last_ui = ui;
    g_last_world = world;
}

static int dt_world_ready(CPU *c, DtContext *ctx)
{
    if (!dt_context(ctx)) return 0;
    if (!dt_call_ui_bool(c, ctx->ui, DT_FN_IN_GAME)) return 0;
    /* The direct actions reproduce CSelectMode.  If another mode owns input
     * (targeting, drag, a modal UI), drop the queued event; phase 1 has no
     * legacy fallback for a field tap and must not invent an order. */
    return dt_mode_active(c, ctx->ui, ctx->mode);
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
    /* 0x6F291240 reads selection+0x1AC; the selection object is at least
     * that large.  A shorter readable window would let a corrupt pointer
     * pass the guard and fault inside the guest call. */
    if (!selection || !dt_guest_readable(selection, 0x1B0u)) return 0;
    return selection;
}

static int dt_selection_nonempty(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return 0;
    const uint32_t count = wc3_rd32g(selection + 4u);
    return count != 0 && count <= 40u;
}

static void dt_clear_selection(CPU *c, const DtContext *ctx)
{
    const uint32_t selection = dt_selection(c, ctx);
    if (!selection) return;
    const uint32_t mode = 0;
    (void)dt_call_thiscall(c, selection, DT_FN_CLEAR_SELECTION, 1, &mode);
}

static void dt_select_unit(CPU *c, uint32_t unit)
{
    if (!dt_guest_readable(unit, 0x6A4u)) return;
    const uint32_t selection = wc3_rd32g(unit + 0x6A0u);
    /* Native 0x6F395410 also calls the 0x6F358B40 thunk (ecx=0x6FAA5AF8)
     * before this.  That thunk only enters 0x6F34B150 when the global
     * [0x6FCE5518] has bits 8 and 0x100 set; it is a diagnostic/telemetry
     * path, not selection, so phase 1 omits it.  The selection action itself
     * is 0x6F6BCE50 with ecx = [unit+0x6A0]. */
    if (selection) {
        /* 0x6F6BCE50 reads selection+0x0C/+0x10; the native caller passes
         * [unit+0x6A0] without a local guard, so validate before the nested
         * guest call.  A corrupt pointer is dropped, not turned into a
         * different selection path. */
        if (!dt_guest_readable(selection, 0x14u)) {
            dt_trace("select-unreadable", selection, 0);
            return;
        }
        (void)dt_call_thiscall(c, selection, DT_FN_SELECT_UNIT, 0, NULL);
        return;
    }
    /* No selection object: 0x6F395410 falls back to 0x6F6BD380 with
     * ecx = [unit+0x190] (low byte is the payload handle) and
     * edx = [unit+0x18C] (packet body pointer). */
    const uint32_t handle = wc3_rd32g(unit + 0x190u);
    const uint32_t body = wc3_rd32g(unit + 0x18Cu);
    (void)dt_call_thiscall_edx(c, handle, body, DT_FN_SELECT_FALLBACK);
}

static int dt_should_order_unit(uint32_t unit)
{
    if (!dt_guest_readable(unit, 0x5Cu)) return 0;
    if (wc3_rd32g(unit + 0x0Cu) != DT_CLASS_UNIT) return 0;

    const uint16_t owner = wc3_rd16g(unit + 0x58u);
    const uint32_t manager = wc3_rd32g(DT_PLAYER_MANAGER);
    if (!manager || !dt_guest_readable(manager + 0x2Au, 2)) return 0;
    const uint16_t local = wc3_rd16g(manager + 0x28u);
    return !wc3_dt_owner_is_friendly(owner, local);
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

    if (r == 1) {
        const uint32_t unit = dt_validate_object(c, object, unit_out);
        if (unit) {
            /* Validation guarantees a live object, but keep the class read
             * inside a readable window: a corrupted object must not fault in
             * the executor before the native release path sees it. */
            const int readable = dt_guest_readable(unit, 0x10u);
            const int is_unit = readable &&
                wc3_rd32g(unit + 0x0Cu) == DT_CLASS_UNIT;
            /* The unit branch is selection-agnostic: a friendly unit selects,
             * an enemy/neutral-hostile unit takes the ghost-order path.  With
             * an empty selection that order is a native no-op; selecting an
             * enemy as if it were friendly would be wrong.  The selection
             * count only decides the terrain fallback below. */
            const int order = is_unit ? dt_should_order_unit(unit) : 0;
            switch (wc3_dt_tap_intent(r, 1, is_unit, 0, order)) {
            case WC3_DT_INTENT_ORDER_UNIT:
                /* Keep the raw pick pointer in the event; the ghost
                 * handler validates it again and takes its own ref. */
                dt_order_unit(c, ctx, scratch, object,
                              wc3_rd32g(pick + 0x08u), wx, wy, wz);
                dt_trace("tap-order-unit", unit, object);
                break;
            case WC3_DT_INTENT_SELECT_UNIT:
                dt_select_unit(c, unit);
                dt_trace("tap-select-unit", unit, 0);
                break;
            case WC3_DT_INTENT_NONE:
                /* Left-click on a non-unit sprite is a native sprite
                 * selection; phase 1 does not implement it and must not
                 * turn it into a ground order. */
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
        const int nonempty = dt_selection_nonempty(c, ctx);
        switch (wc3_dt_tap_intent(r, 0, 0, nonempty, 0)) {
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
    const uint32_t point = scratch + DT_POINT_OFFSET;
    if (!dt_event_point(c, ev->x, ev->y, width, height, point)) {
        dt_scratch_free(scratch);
        return;
    }

    uint32_t object = 0, wx = 0, wy = 0, wz = 0;
    const int r = dt_pick(c, ctx, wc3_rd32g(point), wc3_rd32g(point + 4u),
                          pick, 4u, &object, &wx, &wy, &wz);

    switch (wc3_dt_long_press_intent(r)) {
    case WC3_DT_LONG_INTENT_ORDER_UNIT:
        /* The ghost handler validates the raw object and falls back to the
         * world point if it is no longer a valid unit, so a long press can
         * hand it the pick pointer directly, exactly like a native
         * right-click. */
        dt_order_unit(c, ctx, scratch, object,
                      wc3_rd32g(pick + 0x08u), wx, wy, wz);
        dt_trace("long-order-unit", object, 0);
        break;
    case WC3_DT_LONG_INTENT_ORDER_GROUND:
        dt_order_ground(c, ctx, wx, wy);
        dt_trace("long-order-ground", wx, wy);
        break;
    default:
        break;
    }

    dt_scratch_free(scratch);
}

static void dt_dispatch(CPU *c, const DtContext *ctx,
                        const Wc3DirectTouchEvent *ev,
                        uint32_t width, uint32_t height)
{
    switch (ev->kind) {
    case WC3_DIRECT_TOUCH_TAP:
        dt_tap(c, ctx, ev, width, height);
        break;
    case WC3_DIRECT_TOUCH_LONG_PRESS:
        dt_long_press(c, ctx, ev, width, height);
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
    wc3_direct_touch_clear();
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
    if (wc3_direct_touch_pending() == 0) {
        dt_sync_idle_context();
        return;
    }

    __atomic_store_n(&g_busy, 1, __ATOMIC_RELEASE);
    DtContext ctx;
    if (!dt_world_ready(c, &ctx)) {
        /* Menu, loading, or a UI singleton transition: a queued tap belongs
         * to the previous map and must not hit the next one. */
        g_last_ui = 0;
        g_last_world = 0;
        wc3_direct_touch_clear();
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
        return;
    }

    /* If the transition happened while the guest thread was not pumping,
     * dt_world_ready() can be true again for the next map.  A changed UI or
     * world pointer still identifies the queued events as belonging to the
     * previous one. */
    if ((g_last_ui && g_last_ui != ctx.ui) ||
        (g_last_world && g_last_world != ctx.world)) {
        dt_trace("context-change", g_last_ui ^ ctx.ui,
                 g_last_world ^ ctx.world);
        wc3_direct_touch_clear();
    }
    g_last_ui = ctx.ui;
    g_last_world = ctx.world;

    const uint32_t batch_ui = ctx.ui;
    const uint32_t batch_world = ctx.world;
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
            continue;
        }

        dt_dispatch(c, &ctx, &event, width, height);

        /* A tap can load a map or change the UI; stop the batch instead of
         * resolving the remaining events against stale pointers. */
        if (!dt_world_ready(c, &ctx)) {
            g_last_ui = 0;
            g_last_world = 0;
            wc3_direct_touch_clear();
            break;
        }
        if (ctx.ui != batch_ui || ctx.world != batch_world) {
            dt_trace("batch-context-change", batch_ui ^ ctx.ui,
                     batch_world ^ ctx.world);
            g_last_ui = ctx.ui;
            g_last_world = ctx.world;
            wc3_direct_touch_clear();
            break;
        }
    }
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}
