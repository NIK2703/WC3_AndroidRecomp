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
 * Physical owner of a contact.  This is the input-thread route and it must not
 * depend on the active input mode: a live battlefield is FIELD whether
 * CSelectMode, a command target, or a transient panel mode is on top.  The
 * release resolver in the guest thread decides whether that field contact is
 * a semantic select/order, a target confirmation, or a native UI click.
 *
 * target/placement is deliberately absent from the arguments.  Reintroducing
 * it here recreates the split-route defect: DOWN is classified NATIVE, the
 * mode returns to select before UP, and the release never becomes a TAP.
 */
static inline Wc3DirectTouchRoute wc3_dt_physical_route(int ui_ready,
                                                        int world_ready,
                                                        int click_gate)
{
    if (!ui_ready) return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
    if (world_ready && click_gate) return WC3_DIRECT_TOUCH_ROUTE_FIELD;
    return WC3_DIRECT_TOUCH_ROUTE_NATIVE;
}

/* Total release outcome for a field contact that reached the guest. */
typedef enum {
    WC3_DT_FIELD_RELEASE_SEMANTIC = 0,
    WC3_DT_FIELD_RELEASE_DEFER,
    WC3_DT_FIELD_RELEASE_NATIVE,
    WC3_DT_FIELD_RELEASE_DROP
} Wc3DtFieldRelease;

/*
 * Resolve a field release without a fifth "NONE" outcome.  `ready_field` is
 * the exact semantic route for this pump, `should_defer` is the map-load gate,
 * and `ui_present` says whether there is still a live UI to receive the
 * click.  Target/transient/menu releases are native clicks, not semantic
 * orders; an absent UI is an explicit drop.
 */
static inline Wc3DtFieldRelease wc3_dt_field_release(int ready_field,
                                                     int should_defer,
                                                     int ui_present)
{
    if (ready_field) return WC3_DT_FIELD_RELEASE_SEMANTIC;
    if (should_defer) return WC3_DT_FIELD_RELEASE_DEFER;
    if (ui_present) return WC3_DT_FIELD_RELEASE_NATIVE;
    return WC3_DT_FIELD_RELEASE_DROP;
}

/*
 * The NATIVE release outcome is a validated target/placement confirmation
 * only while a target mode is live.  That path must replay the click in the
 * current pump: the modal placement loop may never call GetCursorPos, so the
 * idle gateway could not publish it.  Every other native release is an
 * ordinary menu/HUD/loading click and enters the same universal gateway as a
 * normal press; the gateway's idle hover pass is what keeps the first tap
 * from being hit-tested against a stale hot control.  A LEGACY route means
 * the game has no live semantic context (menu, loading, closed click gate),
 * so even a stale target-mode word must not select the replay path.
 */
static inline int wc3_dt_native_release_replay(Wc3DirectTouchRoute route,
                                               int target_live)
{
    return route != WC3_DIRECT_TOUCH_ROUTE_LEGACY && target_live;
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

/* True when (x,y) is farther from (from_x,from_y) than the slop radius. */
static inline int wc3_dt_past_slop(float from_x, float from_y,
                                   float x, float y, float slop)
{
    const float dx = x - from_x;
    const float dy = y - from_y;
    return dx * dx + dy * dy > slop * slop;
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

/* Half-open rectangle test in surface-normalised coordinates.  Published HUD
 * snapshots are half-open, matching the native frame hit test. */
static inline int wc3_dt_point_in_rect(float x, float y, float left,
                                       float top, float right, float bottom)
{
    return right > left && bottom > top &&
           x >= left && x < right && y >= top && y < bottom;
}

/*
 * Attack-move gate for a long tap without drag.  The caller has already
 * resolved the live selection, the native pick, the field rectangle and the
 * published HUD rectangles.  A long tap is an attack only when every
 * condition holds; any failure falls back to the ordinary native click, so a
 * HUD edge, a unit under the finger, a paused game or a target/placement mode
 * can never become a world attack.
 */
static inline int wc3_dt_attack_candidate(int controlled,
                                          int pick_result,
                                          int inside_field,
                                          int inside_hud,
                                          int target_active,
                                          int paused,
                                          int native_busy)
{
    return controlled &&
           pick_result == WC3_DT_PICK_TERRAIN &&
           inside_field &&
           !inside_hud &&
           !target_active &&
           !paused &&
           !native_busy;
}

/* Beacon button (MinimapSignal) release decision.  A release on the button
 * arms the next minimap tap; a release over the minimap places there; a
 * release over the portrait places on the displayed unit; a release over
 * the field places at the world point; any other drag cancels.  Portrait is
 * checked before field because the portrait sits inside the field rectangle. */
typedef enum {
    WC3_DT_BEACON_NONE = 0,
    WC3_DT_BEACON_ARM,
    WC3_DT_BEACON_MINIMAP,
    WC3_DT_BEACON_PORTRAIT,
    WC3_DT_BEACON_FIELD
} Wc3DtBeaconRelease;

static inline Wc3DtBeaconRelease wc3_dt_beacon_release(int inside_button,
                                                       int inside_minimap,
                                                       int inside_portrait,
                                                       int inside_field,
                                                       int moved)
{
    /* Release on the button is always a tap, even if the finger jittered
     * past the movement slop: the user's intent is still "arm the next
     * minimap tap".  The button is checked before the field because a HUD
     * control must not turn into a world order when rectangles overlap. */
    if (inside_button) return WC3_DT_BEACON_ARM;
    if (inside_minimap) return WC3_DT_BEACON_MINIMAP;
    if (inside_portrait) return WC3_DT_BEACON_PORTRAIT;
    if (inside_field) return WC3_DT_BEACON_FIELD;
    /* The button rectangle can be unavailable for a frame (for example the
     * minimap was rebuilt between press and release).  A release without
     * movement still means the press was a tap on the button. */
    if (!moved) return WC3_DT_BEACON_ARM;
    return WC3_DT_BEACON_NONE;
}

/*
 * Resolve a beacon release from the DOWN and UP points of one continuous
 * press.  The raw MOVE phases can be deferred behind the input barrier or
 * coalesced away, so a hold on the signal button that ends over the minimap
 * or the world must still be classified as a drag.  Recomputing movement from
 * the two endpoints is the same rule the command-card release uses.
 */
static inline Wc3DtBeaconRelease wc3_dt_beacon_release_from_points(
    int inside_button, int inside_minimap, int inside_portrait,
    int inside_field,
    float down_x, float down_y, float up_x, float up_y, float slop)
{
    const int moved = wc3_dt_past_slop(down_x, down_y, up_x, up_y, slop);
    return wc3_dt_beacon_release(inside_button, inside_minimap,
                                 inside_portrait, inside_field, moved);
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
    return wc3_dt_past_slop(down_x, down_y, up_x, up_y, slop);
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
 * A command-card drag can end on a unit in the selected-unit grid instead of
 * the battlefield.  The grid sits below the field rectangle, so the generic
 * field branch would cancel the command.  A targeted command whose finger
 * moved onto a grid unit must resolve there exactly like a grid press during
 * target mode; a tap or an instant command must not steal the grid press.
 */
static inline int wc3_dt_command_grid_target(int targeted, int grid_hit,
                                             int moved)
{
    return targeted && grid_hit && moved;
}

/*
 * The portrait is the other HUD target for a command-card drag.  Like the
 * grid it sits below the field rectangle, so the generic field branch would
 * cancel the command.  A targeted command that ends on the displayed unit's
 * portrait resolves to that unit, never to the terrain under the portrait.
 */
static inline int wc3_dt_command_portrait_target(int targeted,
                                                 int portrait_hit, int moved)
{
    return targeted && portrait_hit && moved;
}

/*
 * Command-card target mode.  The native click handler selects one of the
 * target input modes, but the command data word at +0x8 cannot identify that
 * choice: observed values are small pairs and ASCII-like words even on
 * commands that entered target mode.  The release therefore uses the live
 * input mode (CGameUI+0x1B4) versus CSelectMode (CGameUI+0x214), not this
 * pointer list.
 *
 * The list below is a diagnostic fallback only.  The live game can select a
 * target mode object that is not one of these fields (a T2 trace saw
 * active=42B10088 while all four fields held different pointers; the card
 * target used 43420088).  The +0x210 mode belongs to the selected-unit grid,
 * not the command card.  An instant command leaves the select mode active.
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
 * The command data word at +0x8 is not a reliable command type across the
 * observed buttons: T2 saw small pairs (0x000D0016, 0x000D001D) and
 * ASCII-like values from adjacent data on commands that clearly entered
 * target mode.  The live input mode is authoritative: selecting a targeted
 * command leaves the select mode, and confirming the target returns to it.
 * The target object is not necessarily one of the fixed CGameUI fields, so
 * compare against the live select mode rather than a hard-coded pointer.
 * A queued release for an instant command sees the select mode still active
 * and is dropped by the timeout.
 */
static inline int wc3_dt_command_target_live(uint32_t active,
                                             uint32_t select_mode)
{
    return select_mode && active && active != select_mode;
}

/*
 * A live non-select input mode owns the next world click as a target or
 * placement confirmation.  The click gate is deliberately absent: target and
 * placement modes may close it, while the active mode still owns the click.
 * The camera mode is also non-select but never confirms a target.
 */
static inline int wc3_dt_target_mode_live(uint32_t active_mode,
                                          uint32_t select_mode,
                                          uint32_t target_mode,
                                          uint32_t camera_mode)
{
    if (target_mode && active_mode == target_mode) return 1;
    if (camera_mode && active_mode == camera_mode) return 0;
    return wc3_dt_command_target_live(active_mode, select_mode);
}

/* CGameUI+0x21C is the dedicated camera input mode object.  Direct-touch
 * camera is mode-neutral and never sets it; the guard remains for a native
 * or legacy source.  The camera mode is not a command target, so a release
 * must never confirm into it just because it differs from CSelectMode. */
static inline int wc3_dt_camera_mode_active(uint32_t active_mode,
                                            uint32_t camera_mode)
{
    return camera_mode != 0u && active_mode == camera_mode;
}

/*
 * A field press can arrive while the game is not ready for semantic input:
 * the map is loading/fading in, the player manager is not live yet, or the
 * input gate is still closed.  The native click would be dropped, so the HLE
 * holds the press and replays a tap once the exact service route becomes
 * FIELD.  The select mode must already be the active mode: a menu, a field
 * drag or a command target mode owns the pointer and must stay native.
 *
 * `ready_field` is the exact service route for this pump, not the
 * input-thread snapshot.  The snapshot can see a live select mode while the
 * game is still not in game; that mismatch is the idle first tap after a map
 * load.
 */
static inline int wc3_dt_field_click_defer(int inside_field, int ui_present,
                                           int world_present,
                                           int select_present,
                                           int select_active,
                                           int click_gate,
                                           int ready_field)
{
    return inside_field && ui_present && world_present && select_present &&
           select_active && (!click_gate || !ready_field);
}

/*
 * A new semantic gesture supersedes a tap held for the map-load replay.
 * A camera begin joins that set only after dt_camera_pan_begin accepted it:
 * with a mode-neutral camera the semantic route stays FIELD, so a stale
 * replay at the old screen point could otherwise run during the pan.  A
 * rejected begin starts no pan and must leave the held tap for replay.
 */
static inline int wc3_dt_event_cancels_field_click(uint32_t kind,
                                                   int camera_started)
{
    if (kind == WC3_DIRECT_TOUCH_POINTER_DOWN ||
        kind == WC3_DIRECT_TOUCH_TAP ||
        kind == WC3_DIRECT_TOUCH_FIELD_HOVER ||
        kind == WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN ||
        kind == WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN)
        return 1;
    return camera_started && kind == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN;
}

/*
 * The visible minimap panel reaches the console top edge, while the native
 * map frame that receives mouse moves only covers its lower part.  A touch
 * must be claimed over the whole panel, then mapped into the smaller native
 * frame before it is posted as a mouse message.  The command card publishes
 * that console top edge on the same surface-normalised axis; use it instead
 * of a hardcoded fraction so the gate tracks UI layout changes.
 */
static inline int wc3_dt_minimap_input_rect(float map_left, float map_top,
                                            float map_right, float map_bottom,
                                            int console_valid,
                                            float console_top,
                                            float *out_left, float *out_top,
                                            float *out_right,
                                            float *out_bottom)
{
    if (!out_left || !out_top || !out_right || !out_bottom) return 0;
    if (!(map_right > map_left) || !(map_bottom > map_top)) return 0;
    if (!isfinite(map_left) || !isfinite(map_top) ||
        !isfinite(map_right) || !isfinite(map_bottom)) return 0;
    *out_left = map_left;
    *out_right = map_right;
    *out_bottom = map_bottom;
    *out_top = map_top;
    /* 0.5 rejects an empty or pixel-form console snapshot; the real card
     * starts near 0.782 on the 2400x1080 surface. */
    if (console_valid && isfinite(console_top) && console_top > 0.5f &&
        console_top < map_top) {
        *out_top = console_top;
    }
    return 1;
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
 * Beacon placement goes through CMinimap+0x644..0x650, which AddSignal
 * (0x6F3B0A30) reads as CRect {minY, minX, maxY, maxX}.  DDC Y grows upward,
 * so the published surface top maps to maxY and the bottom to minY.  Passing
 * the surface fraction straight into wc3_dt_minimap_local_point() mirrors the
 * signal about the map's horizontal centre; mirror the fraction inside the
 * frame first and reuse the same clamping/fraction math.
 */
static inline int wc3_dt_minimap_ddc_local_point(float nx, float ny,
                                                 float frame_top,
                                                 float frame_left,
                                                 float frame_bottom,
                                                 float frame_right,
                                                 float min_y, float min_x,
                                                 float max_y, float max_x,
                                                 float *out_x,
                                                 float *out_y)
{
    const float mirrored_y = frame_top + frame_bottom - ny;
    return wc3_dt_minimap_local_point(nx, mirrored_y,
                                      frame_top, frame_left,
                                      frame_bottom, frame_right,
                                      min_y, min_x, max_y, max_x,
                                      out_x, out_y);
}

/*
 * Clamp a native minimap drag point to the full visible panel.  The panel is
 * the container frame at CGameUI+0x41C; its +0xB4 child only covers the
 * lower map image.  The native camera handler maps the whole container to
 * the map, so a drag must keep its finger coordinate in the container space
 * instead of being compressed into the child frame.  This helper only
 * clamps; the caller applies the one-pixel Win32 RECT inset.
 */
static inline void wc3_dt_minimap_pointer_point(float x, float y,
                                                float left, float top,
                                                float right, float bottom,
                                                float *out_x, float *out_y)
{
    if (!out_x || !out_y) return;
    *out_x = x < left ? left : (x > right ? right : x);
    *out_y = y < top ? top : (y > bottom ? bottom : y);
}

/* The input rect is the whole visible panel.  Its top is above the field
 * rectangle's bottom, so an overlap strip must be claimed by MINIMAP before
 * any FIELD defer/tap is considered.  The +0xB4 map frame is smaller and
 * must never be used as the ownership gate for that strip. */
static inline int wc3_dt_minimap_input_contains(float x, float y,
                                                float left, float top,
                                                float right, float bottom)
{
    return x >= left && x <= right && y >= top && y <= bottom;
}

/* Prefer the live CGameUI+0x41C panel rect over the console-derived guess.
 * It must be non-degenerate and contain the +0xB4 map image; otherwise the
 * fallback keeps the panel usable on a partially built HUD. */
static inline int wc3_dt_minimap_panel_prefers(float pl, float pt,
                                               float pr, float pb,
                                               float ml, float mt,
                                               float mr, float mb)
{
    return pr > pl && pb > pt &&
           pl <= ml && pt <= mt && pr >= mr && pb >= mb;
}

/* A panel rect is usable only when it contains the map image and stays near
 * it.  A whole-HUD or whole-screen frame also contains the minimap, but
 * claiming it as the minimap gate would swallow every other touch.  The
 * margins cover the Minimap* button row above/right of the image. */
static inline int wc3_dt_minimap_panel_plausible(float pl, float pt,
                                                 float pr, float pb,
                                                 float ml, float mt,
                                                 float mr, float mb)
{
    return pr > pl && pb > pt &&
           pl >= ml - 0.05f && pt >= mt - 0.15f &&
           pr <= mr + 0.15f && pb <= mb + 0.02f &&
           pl <= ml && pt <= mt && pr >= mr && pb >= mb;
}

/* A beacon button is smaller than the map and may sit fully above or right
 * of it, so it only has to touch the expanded map bounds; a stray frame far
 * from the minimap is rejected. */
static inline int wc3_dt_minimap_beacon_plausible(float bl, float bt,
                                                  float br, float bb,
                                                  float ml, float mt,
                                                  float mr, float mb)
{
    return br > bl && bb > bt &&
           bl >= ml - 0.05f && bt >= mt - 0.15f &&
           br <= mr + 0.15f && bb <= mb + 0.02f &&
           bl < mr + 0.05f && br > ml - 0.05f &&
           bt < mb + 0.02f && bb > mt - 0.15f;
}

/* The Minimap* button row sits above/right of the map image.  Once its rect
 * is resolved by frame ID, the minimap input area is the union of the panel
 * and that button; both gestures share one ownership gate. */
static inline void wc3_dt_minimap_input_union(float *left, float *top,
                                              float *right, float *bottom,
                                              float bl, float bt,
                                              float br, float bb)
{
    if (!left || !top || !right || !bottom || !(br > bl) || !(bb > bt))
        return;
    if (bl < *left) *left = bl;
    if (bt < *top) *top = bt;
    if (br > *right) *right = br;
    if (bb > *bottom) *bottom = bb;
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
