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

/*
 * Event policy while the pause menu is open.  The pause menu is a native
 * menu and must expose exactly the main-menu gesture set: key signals and
 * raw native pointer phases.  A control-group chord, a semantic field or
 * portrait event, and a minimap drag are game gestures and are dropped.
 * POINTER_CANCEL is always accepted so a contact that was already a world
 * gesture when the menu opened can release its native button.  A raw minimap
 * DOWN/MOVE/UP is carried with the MINIMAP route precisely so it can be
 * dropped here without confusing it with a menu click.
 */
static inline int wc3_dt_menu_event_allowed(uint32_t kind, uint32_t route)
{
    if (kind == WC3_DIRECT_TOUCH_KEY_SIGNAL) return 1;
    if (kind == WC3_DIRECT_TOUCH_GROUP_SIGNAL) return 0;
    if (kind == WC3_DIRECT_TOUCH_POINTER_CANCEL) return 1;
    if (kind == WC3_DIRECT_TOUCH_POINTER_DOWN ||
        kind == WC3_DIRECT_TOUCH_POINTER_MOVE ||
        kind == WC3_DIRECT_TOUCH_POINTER_UP) {
        return route != WC3_DIRECT_TOUCH_ROUTE_MINIMAP;
    }
    return 0;
}

/*
 * The pause menu and the main menu share the native pointer gateway.  The
 * battlefield rectangle is still live under the pause panel, so a pause
 * press must not enter the field tap deferral: that resolver is skipped by
 * the native-menu dispatch, and a held DOWN would leave its release with no
 * click.  The check is the exact menu state, not geometry.
 */
static inline int wc3_dt_native_deferral_allowed(int pause_menu)
{
    return !pause_menu;
}

/*
 * CEscMenu owns the contact as a native menu.  dt_ready_route() deliberately
 * returns a zero semantic context for it, so the live UI/world under the panel
 * must not be read as a context switch.  Treating it as one resets gestures
 * and cancels the armed native DOWN on every pause event: the guest receives
 * only WM_MOUSEMOVE and the cursor hovers without clicking.
 */
static inline int wc3_dt_context_switch_relevant(int pause_menu)
{
    return !pause_menu;
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
 * Unit-type rawcodes are big-endian FourCC dwords: the high byte is the
 * first character, so 'egol' is 0x65676F6C.  Native GetUnitTypeId
 * (0x6F2040C0) returns this dword from CUnit+0x30.
 */
#define WC3_DT_RAWCODE(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

#define WC3_DT_UNIT_TYPE_ENTANGLED_GOLD_MINE \
    WC3_DT_RAWCODE('e', 'g', 'o', 'l')
#define WC3_DT_UNIT_TYPE_HAUNTED_GOLD_MINE \
    WC3_DT_RAWCODE('u', 'g', 'o', 'l')
#define WC3_DT_UNIT_TYPE_HUMAN_TRANSPORT \
    WC3_DT_RAWCODE('h', 'b', 'o', 't')
#define WC3_DT_UNIT_TYPE_ORC_TRANSPORT \
    WC3_DT_RAWCODE('o', 'b', 'o', 't')
#define WC3_DT_UNIT_TYPE_UNDEAD_TRANSPORT \
    WC3_DT_RAWCODE('u', 'b', 'o', 't')
#define WC3_DT_UNIT_TYPE_NEUTRAL_TRANSPORT \
    WC3_DT_RAWCODE('n', 'b', 'o', 't')
#define WC3_DT_UNIT_TYPE_NIGHT_ELF_TRANSPORT \
    WC3_DT_RAWCODE('e', 't', 'r', 's')
#define WC3_DT_UNIT_TYPE_UNDEAD_AIR_BARGE \
    WC3_DT_RAWCODE('u', 'a', 'r', 'b')
#define WC3_DT_UNIT_TYPE_GOBLIN_ZEPPELIN \
    WC3_DT_RAWCODE('n', 'z', 'e', 'p')

/*
 * Warcraft III 1.27 has twelve playable player slots, 0..11.  Slots 12..15
 * are the neutral players (Passive/Hostile/Victim/Extra).  A neutral unit
 * can be selected, but it is not an order source for the local player.
 */
#define WC3_DT_PLAYER_NEUTRAL_FIRST 12u

/*
 * A live selection can receive an order only when its primary unit belongs
 * to a playable player and the native 0x6F66EC60 predicate accepts it.
 * 0x6F66EC60 alone is too broad for this decision: on 2026-11-27 a selected
 * neutral Goblin Ammo Dump (ngad, owner 15) returned can_order=1, so a tap
 * on egol took the order path instead of selecting the mine.  Owner 12..15
 * must stay a select-only selection.
 */
static inline int wc3_dt_selection_can_order(uint32_t owner,
                                             int native_controllable)
{
    return owner < WC3_DT_PLAYER_NEUTRAL_FIRST && native_controllable != 0;
}

/*
 * Target types whose tap must use the native right-click smart order even
 * when the target itself is controllable.  This is also the exception class
 * whose tap/long pair is swapped: tap orders, long tap selects.  The pair
 * itself is built by wc3_dt_unit_gesture_policy().  These are the base-game
 * rawcodes from Units\UnitData.slk and Units\UnitAbilities.slk:
 *   egol Entangled Gold Mine, ugol Haunted Gold Mine,
 *   hbot/obot/ubot/nbot/etrs transport ships,
 *   nzep Goblin Zeppelin, uarb Undead air barge.
 * The neutral gold mine ngol is intentionally absent: it is not
 * controllable, so tap already orders through the uncontrollable-target leg
 * and long tap selects.
 * A custom map can clone these units under new rawcodes; an ability-based
 * check (Aegm/Abgm/Acar) is possible but not proven yet.  Keep this decision
 * pure so T0 can lock the list without a guest image.
 */
static inline int wc3_dt_unit_type_tap_order(uint32_t type_id)
{
    switch (type_id) {
    case WC3_DT_UNIT_TYPE_ENTANGLED_GOLD_MINE:
    case WC3_DT_UNIT_TYPE_HAUNTED_GOLD_MINE:
    case WC3_DT_UNIT_TYPE_HUMAN_TRANSPORT:
    case WC3_DT_UNIT_TYPE_ORC_TRANSPORT:
    case WC3_DT_UNIT_TYPE_UNDEAD_TRANSPORT:
    case WC3_DT_UNIT_TYPE_NEUTRAL_TRANSPORT:
    case WC3_DT_UNIT_TYPE_NIGHT_ELF_TRANSPORT:
    case WC3_DT_UNIT_TYPE_UNDEAD_AIR_BARGE:
    case WC3_DT_UNIT_TYPE_GOBLIN_ZEPPELIN:
        return 1;
    default:
        return 0;
    }
}

/*
 * Tap/long-tap policy for a unit target.  This pair is the only place that
 * decides whether the two gestures are direct or swapped:
 *
 *   controlled selection? | target kind                | tap    | long tap
 *   no                    | any                        | select | select
 *   yes                   | controllable exception     | order  | select
 *   yes                   | controllable ordinary      | select | order
 *   yes                   | uncontrollable             | order  | select
 *
 * With a controlled selection every unit target therefore has exactly one
 * selection and one order gesture.  Only the ordinary controllable class is
 * the direct pair; the exception and uncontrollable classes are swapped.
 * Without a controlled selection there is no order source, so both gestures
 * select/clear.  Rev 12 changed the ordinary long tap from select to order;
 * rev 13 changed the uncontrollable long tap from order to select so the
 * pair never loses either action.
 *
 * `target_tap_order` marks the controllable exception class (gold mine,
 * transport, zeppelin, air barge).  Their tap is the native right-click
 * order, so the long tap is the selection gesture.  An ordinary own unit is
 * the direct pair: tap selects, long tap orders.  Callers consume exactly
 * one field; no second condition is allowed in the executor.
 *
 * `target_controllable` is the owner-gated native 0x6F66EC60 result for the
 * CUnit under the finger: the native predicate plus playable owner 0..11.
 * `selection_controlled` uses the same gate for the primary.  An
 * uncontrollable target (hostile, neutral, or an ally without shared
 * control) keeps the smart order on tap and uses long tap to select it.
 */
typedef struct {
    Wc3DtTapIntent tap;
    Wc3DtTapIntent long_tap;
} Wc3DtUnitGesturePolicy;

static inline Wc3DtUnitGesturePolicy wc3_dt_unit_gesture_policy(
    int selection_controlled, int target_controllable, int target_tap_order)
{
    Wc3DtUnitGesturePolicy policy;
    if (!selection_controlled) {
        /* No order source: both gestures are selection/clear. */
        policy.tap = WC3_DT_INTENT_SELECT_UNIT;
        policy.long_tap = WC3_DT_INTENT_SELECT_UNIT;
    } else if (target_controllable && !target_tap_order) {
        /* Ordinary controllable target: direct pair. */
        policy.tap = WC3_DT_INTENT_SELECT_UNIT;
        policy.long_tap = WC3_DT_INTENT_ORDER_UNIT;
    } else {
        /* Exception or uncontrollable target: swapped pair.  Long tap is
         * the selection gesture, so both actions stay available. */
        policy.tap = WC3_DT_INTENT_ORDER_UNIT;
        policy.long_tap = WC3_DT_INTENT_SELECT_UNIT;
    }
    return policy;
}

/* Tap leg.  Kept as the named entry point for wc3_dt_field_tap_intent(). */
static inline Wc3DtTapIntent wc3_dt_tap_unit_intent(
    int selection_controlled, int target_controllable,
    int target_tap_order)
{
    return wc3_dt_unit_gesture_policy(selection_controlled,
                                      target_controllable,
                                      target_tap_order).tap;
}

/* Long-tap leg.  The policy, not a second controllable test, decides
 * whether a stationary hold selects or orders. */
static inline Wc3DtTapIntent wc3_dt_long_tap_unit_intent(
    int selection_controlled, int target_controllable,
    int target_tap_order)
{
    return wc3_dt_unit_gesture_policy(selection_controlled,
                                      target_controllable,
                                      target_tap_order).long_tap;
}

/*
 * Non-unit/terrain leg.  Units are decided by wc3_dt_tap_unit_intent()
 * through wc3_dt_field_tap_intent(); this helper must not grow a second unit
 * rule.  `validated_object` is false for pick result 1 when the picked
 * object no longer validates; native treats that as a terrain click.  A
 * validated non-unit sprite is a no-op when there is no controlled selection.
 * With a controlled selection every non-unit object and the terrain become
 * the right-click analogue, so the executor re-picks with the right button
 * and lets the native handler resolve the decoration or ground target.
 */
static inline Wc3DtTapIntent wc3_dt_tap_intent(int pick_result,
                                               int validated_object,
                                               int is_unit,
                                               int selection_controlled)
{
    if (pick_result == WC3_DT_PICK_OBJECT) {
        if (is_unit) return WC3_DT_INTENT_NONE; /* contract: not used */
        if (selection_controlled) return WC3_DT_INTENT_ORDER_UNIT;
        return validated_object ? WC3_DT_INTENT_NONE
                                : WC3_DT_INTENT_CLEAR_SELECTION;
    }
    if (pick_result == WC3_DT_PICK_TERRAIN) {
        return selection_controlled ? WC3_DT_INTENT_ORDER_GROUND
                                    : WC3_DT_INTENT_CLEAR_SELECTION;
    }
    return WC3_DT_INTENT_NONE;
}

/*
 * Single decision point for a battlefield tap.  The executor and T0 call
 * only this helper, so a unit tap can never be resolved by the non-unit leg
 * and the two rules cannot drift apart.
 */
static inline Wc3DtTapIntent wc3_dt_field_tap_intent(
    int pick_result, int validated_object, int is_unit,
    int selection_controlled, int target_controllable,
    int target_tap_order)
{
    if (pick_result == WC3_DT_PICK_OBJECT && validated_object && is_unit)
        return wc3_dt_tap_unit_intent(selection_controlled,
                                      target_controllable,
                                      target_tap_order);
    return wc3_dt_tap_intent(pick_result, validated_object, is_unit,
                             selection_controlled);
}

/* Half-open rectangle test in surface-normalised coordinates.  Published HUD
 * snapshots are half-open, matching the native frame hit test. */
static inline int wc3_dt_point_in_rect(float x, float y, float left,
                                       float top, float right, float bottom)
{
    return right > left && bottom > top &&
           x >= left && x < right && y >= top && y < bottom;
}

/* Native command-card, inventory and selected-unit grid frames are half-open.
 * The name keeps a button hit test from being confused with the inclusive
 * field/HUD rectangles.  The arithmetic stays in wc3_dt_point_in_rect(). */
static inline int wc3_dt_button_rect_hit(float x, float y, float left,
                                         float top, float right, float bottom)
{
    return wc3_dt_point_in_rect(x, y, left, top, right, bottom);
}

/* Inclusive rectangle test for the battlefield field rectangle.  Boundary
 * points belong to it; this is intentionally not used for native buttons. */
static inline int wc3_dt_rect_contains_inclusive(float x, float y, float left,
                                                 float top, float right,
                                                 float bottom)
{
    return x >= left && x <= right && y >= top && y <= bottom;
}

/* Surface-normalised rectangle used by the snapshot union.  The union helper
 * only extends the bounds; it does not change the half-open policy of the
 * hit tests. */
typedef struct {
    float left;
    float top;
    float right;
    float bottom;
} Wc3DtRect;

static inline void wc3_dt_rect_union(Wc3DtRect *dst, const Wc3DtRect *src)
{
    if (!dst || !src) return;
    if (!(src->right > src->left) || !(src->bottom > src->top)) return;
    if (!(dst->right > dst->left) || !(dst->bottom > dst->top)) {
        *dst = *src;
        return;
    }
    if (src->left < dst->left) dst->left = src->left;
    if (src->top < dst->top) dst->top = src->top;
    if (src->right > dst->right) dst->right = src->right;
    if (src->bottom > dst->bottom) dst->bottom = src->bottom;
}

/* Left-button ownership policies.  The call sites must choose explicitly:
 * a shortened check must not silently become the full one, or the reverse. */
typedef enum {
    WC3_DT_NATIVE_BUSY_FULL,
    WC3_DT_NATIVE_BUSY_DOWN_OR_UP,
    WC3_DT_NATIVE_BUSY_DOWN_ONLY,
    WC3_DT_NATIVE_BUSY_HARDWARE,
} Wc3DtNativeBusyMode;

static inline int wc3_dt_native_left_busy(Wc3DtNativeBusyMode mode,
                                          int down, int up_pending,
                                          int release_left, int hw_down)
{
    switch (mode) {
    case WC3_DT_NATIVE_BUSY_FULL:
        return down || up_pending || (!release_left && hw_down);
    case WC3_DT_NATIVE_BUSY_DOWN_OR_UP:
        return down || up_pending;
    case WC3_DT_NATIVE_BUSY_DOWN_ONLY:
        return down;
    case WC3_DT_NATIVE_BUSY_HARDWARE:
        return down || (!release_left && hw_down);
    }
    return 0;
}

/*
 * Inventory item release.  The source slot was occupied at DOWN; the caller
 * resolves the live destination slot and whether its button rect is
 * readable, plus the field/HUD state of the release point.
 *
 * A destination slot wins over the tap slop: a deliberate short drag to an
 * adjacent slot must move the item, not degrade into a use.  The tap slop
 * then keeps a finger that drifted out of the source slot from turning a tap
 * into a world drop.  A HUD panel (command card, inventory bar, minimap,
 * portrait, beacon) is never a battlefield drop.  `source_slot < 0` is the
 * stale/unknown context and always cancels.
 */
typedef enum {
    WC3_DT_INV_CANCEL = 0,
    WC3_DT_INV_USE,
    WC3_DT_INV_MOVE_SLOT,
    WC3_DT_INV_DROP_FIELD
} Wc3DtInventoryRelease;

static inline Wc3DtInventoryRelease wc3_dt_inventory_release(
    int source_slot, int dest_slot, int dest_rect_ok, int tap_slop_ok,
    int inside_field, int inside_hud)
{
    if (source_slot < 0) return WC3_DT_INV_CANCEL;
    if (dest_slot == source_slot) return WC3_DT_INV_USE;
    if (dest_slot >= 0) {
        /* A destination slot without a readable button rect cannot be
         * confirmed by a native click; do not fall through to a world drop
         * at the same point. */
        return dest_rect_ok ? WC3_DT_INV_MOVE_SLOT : WC3_DT_INV_CANCEL;
    }
    if (tap_slop_ok) return WC3_DT_INV_USE;
    if (inside_field && !inside_hud) return WC3_DT_INV_DROP_FIELD;
    return WC3_DT_INV_CANCEL;
}

/*
 * Smart-order gate for a long tap without drag.  The caller has already
 * resolved the live selection, the field rectangle and the published HUD
 * rectangles; the target itself is resolved on release, so no pick is part of
 * the gate.  A long tap is an order only when every condition holds; any
 * failure falls back to the ordinary native press/click, so a HUD edge, a
 * paused game or a target/placement mode can never become a world order.
 */
static inline int wc3_dt_smart_order_candidate(int controlled,
                                               int inside_field,
                                               int inside_hud,
                                               int target_active,
                                               int paused,
                                               int native_busy)
{
    return controlled && inside_field && !inside_hud &&
           !target_active && !paused && !native_busy;
}

/*
 * Order route for a right-button pick.  WC3_DT_PICK_OBJECT is not only a
 * CGhostImage: a controllable unit is returned as a CUnit, and 0x6F21B730
 * accepts only the ghost.  A CUnit pick is still a unit target and must use
 * the native right click, never the terrain attack-move: A + left click on a
 * friendly unit is not a valid confirmation and the game drops the order.
 *
 * Terrain and a failed pick use the attack-move only when the caller asked
 * for it (long tap); tap keeps the right-click smart move and the native
 * fallback.
 */
typedef enum {
    WC3_DT_ORDER_ROUTE_NONE = 0,
    WC3_DT_ORDER_ROUTE_UNIT,
    WC3_DT_ORDER_ROUTE_NATIVE,
    WC3_DT_ORDER_ROUTE_GROUND,
    WC3_DT_ORDER_ROUTE_ATTACK_MOVE
} Wc3DtOrderRoute;

static inline Wc3DtOrderRoute wc3_dt_order_route(int pick_result,
                                                 int ghost_valid,
                                                 int attack_move)
{
    if (pick_result == WC3_DT_PICK_OBJECT) {
        return ghost_valid ? WC3_DT_ORDER_ROUTE_UNIT
                           : WC3_DT_ORDER_ROUTE_NATIVE;
    }
    if (pick_result == WC3_DT_PICK_TERRAIN) {
        return attack_move ? WC3_DT_ORDER_ROUTE_ATTACK_MOVE
                           : WC3_DT_ORDER_ROUTE_GROUND;
    }
    return attack_move ? WC3_DT_ORDER_ROUTE_ATTACK_MOVE
                       : WC3_DT_ORDER_ROUTE_NATIVE;
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
 * A selected-unit grid press becomes an individual unit selection only when
 * it was a tap: no target was applied, the finger never crossed into another
 * cell (or left the cells), and it ended on the cell where it started.
 * Crossing cells is the subgroup switch gesture and must not collapse the
 * squad to one unit on release.
 */
static inline int wc3_dt_grid_release_select(int target, int moved,
                                             int same_cell)
{
    return !target && !moved && same_cell;
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
 * Contact-start events advance the input epoch.  The epoch is the
 * deterministic cancellation signal for a deferred field click: it changes in
 * the same g_uq_lock critical section that queues the new input, so the click
 * either wins the lock before the input or is cancelled after it.  Motion,
 * release, hold-release and camera phases continue an existing gesture and
 * must not cancel.  FIELD_CAMERA_BEGIN is excluded because a rejected begin
 * starts no pan; an accepted one cancels on the guest thread through
 * wc3_dt_event_cancels_field_click(..., camera_started=1).
 *
 * A portrait contact emits FOLLOW_START instead of POINTER_DOWN, and a
 * control-group chord emits GROUP_SIGNAL; both start a new command and are
 * included.  KEY_SIGNAL is not produced by the current controls layer; if it
 * returns, its down transition must be routed through hle_android_key_event
 * or carry its direction explicitly.
 */
static inline int wc3_dt_event_bumps_input_epoch(uint32_t kind)
{
    switch (kind) {
    case WC3_DIRECT_TOUCH_POINTER_DOWN:
    case WC3_DIRECT_TOUCH_TAP:
    case WC3_DIRECT_TOUCH_FIELD_HOVER:
    case WC3_DIRECT_TOUCH_FIELD_SELECT_BEGIN:
    case WC3_DIRECT_TOUCH_FIELD_SELECT_DRAG_BEGIN:
    case WC3_DIRECT_TOUCH_LONG_PRESS:
    case WC3_DIRECT_TOUCH_FOLLOW_START:
    case WC3_DIRECT_TOUCH_GROUP_SIGNAL:
        return 1;
    default:
        return 0;
    }
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
