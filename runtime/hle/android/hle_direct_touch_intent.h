/*
 * hle_direct_touch_intent.h - pure direct-touch decisions.
 *
 * The guest-thread executor in hle_direct_touch.c owns the guest calls.  The
 * small math and intent rules live here so T0 can test them without a guest
 * image, fake CPU, or linked game.dll.  Keep this header free of guest
 * addresses and platform dependencies.
 */
#ifndef WC3_HLE_DIRECT_TOUCH_INTENT_H
#define WC3_HLE_DIRECT_TOUCH_INTENT_H

#include <math.h>
#include <stdint.h>

#include "direct_touch_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native 0x6F3840E0 return values. */
enum {
    WC3_DT_PICK_NONE = 0,
    WC3_DT_PICK_OBJECT = 1,
    WC3_DT_PICK_TERRAIN = 2
};

/*
 * Surface-normalised coordinate to client pixel float.  This is the same
 * clamp+offset the existing mouse path uses: nx*(win-1)+0.5.  The native
 * producer truncates the float when it builds the input event; use
 * wc3_dt_pick_axis() for that step.
 */
static inline float wc3_dt_surface_to_client(float fraction, uint32_t win)
{
    const float span = win > 1u ? (float)(win - 1u) : 0.0f;
    float v = fraction * span + 0.5f;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > span) v = span;
    return v;
}

/* The native input event stores integer client pixels as floats. */
static inline float wc3_dt_client_pixel(float fraction, uint32_t win)
{
    return (float)(int)wc3_dt_surface_to_client(fraction, win);
}

/*
 * Command card, inventory and selected-unit grid frames live in the game's
 * 800x600 console space, not in surface fractions.  The native input
 * producer 0x6F8B38A0 scales the surface point by 0.8 horizontally and 0.6
 * vertically before the frame hit-test, and CFrame+0x44 stores the rect in
 * that same space.  dt_frame_rect_read() already flips the raw bottom-left
 * rect onto the overlay top-left axis, so this only undoes the remaining
 * console scale: x/0.8 and (y - 0.4)/0.6.  The minimap, portrait and beacon
 * are full-surface frames and must not go through this.
 */
static inline void wc3_dt_console_rect_scale(float *left, float *top,
                                             float *right, float *bottom)
{
    if (left) *left /= 0.8f;
    if (right) *right /= 0.8f;
    if (top) *top = (*top - 0.4f) / 0.6f;
    if (bottom) *bottom = (*bottom - 0.4f) / 0.6f;
}

/*
 * Client pixel float to pick-event coordinate.  0x6F8B38A0 multiplies its
 * two stack arguments by 0x6FAA9328 (0.8f) and 0x6FAA9324 (0.6f).  The
 * native input event carries integer client pixels, so truncate first.
 */
static inline float wc3_dt_pick_axis(float client_px, float scale)
{
    return (float)(int)client_px * scale;
}

/*
 * Touch routing.  The controls layer runs on the Android input thread and
 * cannot call guest code, so the guest service publishes this decision from
 * a pointer-only snapshot.
 *
 * ui_ready is false only while the UI singleton is not readable or the game
 * is still starting; then direct touch cannot claim the pointer yet.
 * Otherwise the native UI is up: a battlefield touch is semantic only when
 * CSelectMode is the active input mode and no targeting mode is active.
 * Everything else -- native HUD, menu, targeting/placement -- is forwarded
 * as raw pointer phases to the native mouse path.  It is never handed to the
 * overlay engine.
 */
static inline Wc3DirectTouchRoute wc3_dt_route(int ui_ready,
                                               int select_active,
                                               int target_active,
                                               int select_is_active_mode)
{
    if (!ui_ready) return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    if (target_active) return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
    if (select_active && select_is_active_mode) {
        return WC3_DIRECT_TOUCH_ROUTE_FIELD;
    }
    return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
}

/*
 * A field drag owns its native mouse button from BEGIN to END.  Pressing that
 * button can change the game's active input mode, so the route snapshot can
 * briefly become NATIVE after BEGIN.  That is the drag itself, not a reason to
 * cancel it: only a new gesture, an explicit reset, or a context change may
 * end an active drag before its END event arrives.
 */
static inline int wc3_dt_field_drag_keep(Wc3DirectTouchRoute route,
                                         int drag_active)
{
    return drag_active || route == WC3_DIRECT_TOUCH_ROUTE_FIELD;
}

typedef enum {
    WC3_DT_INTENT_NONE = 0,
    WC3_DT_INTENT_SELECT_UNIT,
    WC3_DT_INTENT_ORDER_UNIT,
    WC3_DT_INTENT_ORDER_GROUND,
    WC3_DT_INTENT_CLEAR_SELECTION
} Wc3DtTapIntent;

/*
 * Context-tap decision.  validated_object is false for pick result 1 when
 * 0x6F21B730 no longer accepts the picked object; native treats that as a
 * terrain click.  A validated non-unit sprite is a no-op in phase 1.
 *
 * The discriminator is whether the current selection can receive an order
 * from the local player, not merely whether it is non-empty.  Without a
 * controlled selection (empty, hostile/neutral units, decorations, or an
 * ally who has not shared control) a unit tap is the left-click analogue:
 * it selects that unit, whatever its owner.  With a controlled selection
 * (own units or an ally who shared control) a unit tap is the right-click
 * analogue (smart order, including a friendly unit) and a terrain tap is a
 * smart point order.  A non-empty but uncontrollable selection never turns
 * a terrain tap into a ground order; terrain clears it instead.
 */
static inline Wc3DtTapIntent wc3_dt_tap_intent(int pick_result,
                                               int validated_object,
                                               int is_unit,
                                               int selection_controlled)
{
    if (pick_result == WC3_DT_PICK_OBJECT && validated_object) {
        if (!is_unit) return WC3_DT_INTENT_NONE;
        return selection_controlled ? WC3_DT_INTENT_ORDER_UNIT
                                    : WC3_DT_INTENT_SELECT_UNIT;
    }
    if (pick_result == WC3_DT_PICK_OBJECT || pick_result == WC3_DT_PICK_TERRAIN)
        return selection_controlled ? WC3_DT_INTENT_ORDER_GROUND
                                    : WC3_DT_INTENT_CLEAR_SELECTION;
    return WC3_DT_INTENT_NONE;
}

/* Beacon button (MinimapSignal) release decision.  A release on the button
 * arms the next minimap tap; a release over the minimap places there; a
 * release over the field places at the world point; any other drag cancels. */
typedef enum {
    WC3_DT_BEACON_NONE = 0,
    WC3_DT_BEACON_ARM,
    WC3_DT_BEACON_MINIMAP,
    WC3_DT_BEACON_FIELD
} Wc3DtBeaconRelease;

static inline Wc3DtBeaconRelease wc3_dt_beacon_release(int inside_button,
                                                       int inside_minimap,
                                                       int inside_field,
                                                       int moved)
{
    /* Release on the button is always a tap, even if the finger jittered
     * past the movement slop: the user's intent is still "arm the next
     * minimap tap".  The button is checked before the field because a HUD
     * control must not turn into a world order when rectangles overlap. */
    if (inside_button) return WC3_DT_BEACON_ARM;
    if (inside_minimap) return WC3_DT_BEACON_MINIMAP;
    if (inside_field) return WC3_DT_BEACON_FIELD;
    /* The button rectangle can be unavailable for a frame (for example the
     * minimap was rebuilt between press and release).  A release without
     * movement still means the press was a tap on the button. */
    if (!moved) return WC3_DT_BEACON_ARM;
    return WC3_DT_BEACON_NONE;
}

/*
 * Command-card movement threshold.  The recognizer routes a MOVE that leaves
 * the HUD over the field path, so the command handler may not see every MOVE
 * of a drag.  Recompute movement from the DOWN and UP points; otherwise a
 * drag into the field looks like a tap and the target click is lost.
 */
static inline int wc3_dt_command_moved(float down_x, float down_y,
                                       float up_x, float up_y,
                                       float slop)
{
    const float dx = up_x - down_x;
    const float dy = up_y - down_y;
    return dx * dx + dy * dy > slop * slop;
}

/*
 * Command-card release.  The DOWN click selects the command and, for a
 * targeted/area command, enters the game's target mode; the drag only moves
 * the cursor.  `targeted` is the live target mode at release.  The release
 * confirms the target only when the drag ended over the field.  A tap, an
 * unarmed command, or a release outside the field posts no field click.
 */
typedef enum {
    WC3_DT_COMMAND_NONE = 0,
    WC3_DT_COMMAND_TARGET
} Wc3DtCommandRelease;

static inline Wc3DtCommandRelease wc3_dt_command_release(int moved,
                                                         int targeted,
                                                         int inside_field)
{
    if (moved && targeted && inside_field) return WC3_DT_COMMAND_TARGET;
    return WC3_DT_COMMAND_NONE;
}

/*
 * Command-card target mode.  The command button's click handler (0x6F3BE1E0)
 * selects an input mode from the command data type: unit, point, area and the
 * remaining target case use CGameUI+0x228, +0x238, +0x23C and +0x240.  The
 * +0x210 mode belongs to the selected-unit grid, not the command card.  A
 * release confirms only when one of these modes is the active input mode
 * (CGameUI+0x1B4); an instant command leaves the select mode active and a
 * submenu leaves a different mode active.
 */
static inline int wc3_dt_command_target_mode(uint32_t active,
                                             uint32_t mode_210,
                                             uint32_t mode_228,
                                             uint32_t mode_238,
                                             uint32_t mode_23c,
                                             uint32_t mode_240)
{
    if (!active) return 0;
    return (mode_210 && active == mode_210) ||
           (mode_228 && active == mode_228) ||
           (mode_238 && active == mode_238) ||
           (mode_23c && active == mode_23c) ||
           (mode_240 && active == mode_240);
}

/*
 * Map a surface-normalised point inside the published minimap frame to the
 * frame-local coordinate space AddSignal uses.  The published rectangle and
 * the local clamp rectangle are both derived from CFrame+0x44, so only their
 * fractions matter; the units (client pixels or 0..1) cancel out.
 *
 * The CFrame rect is stored as {top, left, bottom, right}; callers pass the
 * four fields in that order.  A point outside the frame is clamped to its
 * edge, matching the native clamp AddSignal applies to the stored signal.
 */
static inline int wc3_dt_minimap_local_point(float nx, float ny,
                                             float frame_top,
                                             float frame_left,
                                             float frame_bottom,
                                             float frame_right,
                                             float local_top,
                                             float local_left,
                                             float local_bottom,
                                             float local_right,
                                             float *out_x,
                                             float *out_y)
{
    if (!out_x || !out_y) return 0;
    if (!(frame_right > frame_left) || !(frame_bottom > frame_top)) return 0;
    if (!(local_right > local_left) || !(local_bottom > local_top)) return 0;

    float fx = (nx - frame_left) / (frame_right - frame_left);
    float fy = (ny - frame_top) / (frame_bottom - frame_top);
    if (!isfinite(fx) || !isfinite(fy)) return 0;
    if (fx < 0.0f) fx = 0.0f;
    if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fy > 1.0f) fy = 1.0f;

    *out_x = local_left + fx * (local_right - local_left);
    *out_y = local_top + fy * (local_bottom - local_top);
    return 1;
}

/*
 * Invert the minimap world->screen affine matrix.  CMinimap stores it as
 * m[0..8] at +0x750 and 0x6F3B0A30 (AddSignal) applies it as:
 *
 *   sx = m[0]*x + m[3]*y + m[6]
 *   sy = m[1]*x + m[4]*y + m[7]
 *
 * The release point is in the same frame-local space AddSignal writes, so
 * the inverse maps it back to the world point PingMinimap would accept.
 */
static inline int wc3_dt_minimap_screen_to_world(const float m[9],
                                                 float sx, float sy,
                                                 float *world_x,
                                                 float *world_y)
{
    if (!m || !world_x || !world_y) return 0;
    for (int i = 0; i < 9; i++) {
        if (!isfinite(m[i])) return 0;
    }
    const float det = m[0] * m[4] - m[3] * m[1];
    if (!isfinite(det) || fabsf(det) < 1e-12f) return 0;

    const float dx = sx - m[6];
    const float dy = sy - m[7];
    const float x = (m[4] * dx - m[3] * dy) / det;
    const float y = (m[0] * dy - m[1] * dx) / det;
    if (!isfinite(x) || !isfinite(y)) return 0;
    *world_x = x;
    *world_y = y;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_DIRECT_TOUCH_INTENT_H */
