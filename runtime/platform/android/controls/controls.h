#ifndef WC3_CONTROLS_H
#define WC3_CONTROLS_H

/*
 * Detached Winlator touch engine adapter.
 *
 * This is the only runtime entry point for the detached control overlay.  It
 * owns the vendored C touch state machine and exposes plain touch phases plus
 * an action sink.  It does not know about the recompiler, HLE, GameActivity or
 * JNI.
 */

#include <stdbool.h>
#include <stdint.h>

#include "direct_touch_event.h"
#include "overlay.h"
#include "overlay_mode.h"
#include "core/types/touch_action.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WC3_TOUCH_DOWN = 0,
    WC3_TOUCH_MOVE,
    WC3_TOUCH_UP,
    WC3_TOUCH_CANCEL
} Wc3TouchPhase;

typedef void (*Wc3ActionSink)(const TouchAction *actions, int count,
                              void *user);

/*
 * Direct-mode overlay keys are emitted as native key signals instead of
 * keyboard actions: the guest looks up whatever the player bound to that key.
 * The host installs the same X11-keycode-to-VK translation the legacy layer
 * uses, so both modes agree on which virtual key a profile binding means.
 * A NULL map keeps the keyboard path for every key.
 */
typedef int (*Wc3KeyVkMap)(int keycode, void *user);
void controls_set_key_signal_vk_map(Wc3KeyVkMap map, void *user);

/*
 * Semantic direct touch is an additional input variant.  It is disabled by
 * default: the host enables it only when the user selected it and a guest
 * sink is installed.  If it is enabled without one, game touches are
 * forwarded as raw native phases instead of silently falling back to the
 * legacy overlay engine; only authored overlay controls stay on that path.
 * Wc3DirectTouchSink is defined by direct_touch_event.h.
 */
void controls_set_direct_touch_enabled(bool enabled);
void controls_set_direct_touch_sink(Wc3DirectTouchSink sink, void *user);

/*
 * Legacy long-press haptic for the battlefield.  The direct recognizer runs
 * on the input thread and cannot call the guest, so it delivers the haptic
 * directly through this sink when the profile's gesture long-press timer
 * expires.  The effect comes from the authored profile
 * (gestureLongPressHaptic, 3 in the Warcraft 3 profile).  A NULL sink
 * disables the vibration without changing the gesture.
 */
typedef void (*Wc3DirectTouchHapticSink)(int effect, void *user);
void controls_set_direct_touch_haptic_sink(Wc3DirectTouchHapticSink sink,
                                           void *user);

/*
 * Lock-free routing predicate supplied by the host.  The input thread cannot
 * call guest code, so the guest side publishes where a non-overlay touch
 * belongs.  FIELD means the semantic battlefield recognizer (with a quick
 * swipe driving the native camera drag and a held drag drawing the native
 * selection rectangle); NATIVE means
 * raw pointer phases for the native UI (HUD, menus, targeting/placement);
 * PORTRAIT and MINIMAP are claimed by their separate rectangle gates.
 * LEGACY means direct touch is disabled or the touch hit an authored overlay
 * control element.
 *
 * Legacy is only for overlay control elements.  In direct mode a native HUD,
 * menu or targeting touch must not be sent through the overlay engine; it is
 * forwarded as pointer phases and the guest side posts the native mouse
 * messages itself.  A LEGACY snapshot means the guest has no live CGameUI
 * (startup, loading screen or main menu); it is forwarded as NATIVE, never
 * guessed as FIELD from the battlefield rectangle.  The HLE resolves a live
 * field on release when the snapshot was only one pump stale.  A NULL gate
 * keeps the phase-1 FIELD behavior for tests and host-only builds.
 */
typedef Wc3DirectTouchRoute (*Wc3DirectTouchRouteGate)(void *user);
void controls_set_direct_touch_route_gate(Wc3DirectTouchRouteGate gate,
                                          void *user);

/*
 * Lock-free portrait rectangle supplied by the host.  The input thread calls
 * this for every non-overlay touch; the guest side publishes the visible
 * portrait frame in surface-normalised coordinates (0..1).  Returns 1 and
 * fills all four values when the portrait exists; 0 means "no portrait
 * gesture here" and the touch keeps its FIELD/NATIVE route.  The gate must
 * not call guest code and must be safe on the Android input thread.
 */
typedef int (*Wc3DirectTouchRectGate)(float *left, float *top,
                                      float *right, float *bottom,
                                      void *user);
typedef Wc3DirectTouchRectGate Wc3DirectTouchPortraitRectGate;
void controls_set_direct_touch_portrait_gate(
    Wc3DirectTouchPortraitRectGate gate, void *user);

/*
 * Lock-free minimap input rectangle supplied by the host, same contract as
 * the portrait gate.  It covers the whole visible minimap panel, whose top
 * edge reaches the console while the native mouse frame only covers its
 * lower part.  The input thread claims a touch that starts inside this
 * rectangle for the minimap and clamps every later pointer phase to it, so a
 * finger that leaves the minimap keeps dragging the camera until it is
 * released.  The guest service maps the clamped point into the native frame
 * before posting mouse messages.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchMinimapRectGate;
void controls_set_direct_touch_minimap_gate(
    Wc3DirectTouchMinimapRectGate gate, void *user);

/*
 * Lock-free beacon button (MinimapSignal) rectangle supplied by the host,
 * same contract as the minimap gate.  A touch that starts inside this
 * rectangle is forced to the raw NATIVE route even if the button overlaps the
 * battlefield rectangle, so the guest beacon gesture can claim the press
 * before any native mouse button is posted.  A NULL gate keeps the normal
 * route.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchBeaconRectGate;
void controls_set_direct_touch_beacon_gate(
    Wc3DirectTouchBeaconRectGate gate, void *user);

/*
 * Lock-free command-card rectangle supplied by the host, same contract as
 * the beacon gate.  The card's top row overlaps the battlefield rectangle
 * (the card starts above the field bottom), so a touch that starts inside
 * the card is forced to the raw NATIVE route; otherwise the field
 * recognizer would turn a command-card press into a world tap.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchCommandRectGate;
void controls_set_direct_touch_command_gate(
    Wc3DirectTouchCommandRectGate gate, void *user);

/*
 * Lock-free selected-unit grid rectangle supplied by the host, same contract
 * as the command gate.  The grid's top row overlaps the battlefield rectangle
 * (the 6x2 panel reaches roughly y 0.82 while the field ends at 0.85), so a
 * touch that starts inside the grid is forced to the raw NATIVE route; the
 * guest grid handler then switches the active subgroup or selects the unit.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchGridRectGate;
void controls_set_direct_touch_grid_gate(
    Wc3DirectTouchGridRectGate gate, void *user);

/*
 * Lock-free inventory-bar rectangle supplied by the host, same contract as
 * the command gate.  The bar sits left of the command card and its top row
 * overlaps the battlefield rectangle (the slot centres are around y 0.8355
 * while the field ends at 0.85), so a touch that starts inside the bar is
 * forced to the raw NATIVE route; the guest inventory handler then decides
 * whether an occupied slot starts an item gesture.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchInventoryRectGate;
void controls_set_direct_touch_inventory_gate(
    Wc3DirectTouchInventoryRectGate gate, void *user);

/*
 * Lock-free hero-bar slot union supplied by the host, same contract as the
 * command gate.  The bar is anchored in the upper-left and its lower slots
 * can overlap the battlefield rectangle, so a touch that starts inside the
 * union is forced to the raw NATIVE route; the guest hero handler applies a
 * unit target when a target mode is live and otherwise leaves the tap to the
 * native hero-bar selection.
 */
typedef Wc3DirectTouchRectGate Wc3DirectTouchHeroRectGate;
void controls_set_direct_touch_hero_gate(
    Wc3DirectTouchHeroRectGate gate, void *user);

/* Profile JSON must stay valid only for the duration of this call. */
int controls_init(const char *profile_json, int width, int height);

/*
 * Same as controls_init(), but applies the product gesture mode and timers
 * before the profile is converted to the engine config.  A timer <= 0 keeps
 * the authored .icp value.  tap_click_delay_ms is the SIMPLE-overlay delay
 * between a pointer move and the LMB press; <= 0 selects the 10 ms default
 * and any value is clamped to 8..40 ms.  The legacy controls_init() keeps the
 * profile unchanged, which is also what a direct diagnostic launch without
 * the settings bundle receives.
 */
int controls_init_ex(const char *profile_json, int width, int height,
                     bool double_tap_enabled, int double_tap_timeout_ms,
                     int long_press_timeout_ms, int tap_click_delay_ms);

void controls_set_action_sink(Wc3ActionSink sink, void *user);

int controls_touch(Wc3TouchPhase phase, int pointer_id, float x, float y,
                       uint64_t event_time_ms);
int controls_tick(uint64_t now_ms);

/*
 * 1 while the engine has a pending timer, auto-repeat, mouse movement, a
 * held action, or one-shot cleanup to perform.  An active finger alone is not
 * a tick reason.  When 0, the host may block until the next touch instead of
 * polling controls_tick() at the tick rate.
 */
int controls_needs_tick(void);

/*
 * Exact absolute CLOCK_MONOTONIC millisecond deadline for the next tick, or 0
 * when no timer is pending.  The host must treat 0 as "no deadline", not as
 * "due now".  A returned value <= now_ms means run one tick immediately and
 * query again.
 */
uint64_t controls_next_deadline_ms(uint64_t now_ms);
void controls_cancel_all(void);

/*
 * Exact switch guard.  Returns 1 only when no finger, held action, deferred
 * tap, scheduled action or simulated click is live.  The host must also ensure
 * its own action queue is empty before calling controls_set_overlay_mode().
 */
int controls_switch_safe(void);

/*
 * Switch between the authored FULL profile and the derived SIMPLE variants.
 * Both SIMPLE modes share the same simplified global gestures.  SIMPLE has no
 * on-screen elements (menus); SIMPLE_ESC has exactly the authored ESC element
 * and is used while a cinematic scene is running.  The switch is rejected
 * (negative result, current mode unchanged) when the engine is busy or the
 * requested variant is unavailable.  A successful switch releases held
 * bindings and increments layout_id.
 */
int controls_set_overlay_mode(Wc3OverlayMode mode);
Wc3OverlayMode controls_overlay_mode(void);

/* Monotonic id of the current engine layout; 0 before the first init. */
uint32_t controls_layout_id(void);

/*
 * Product opacity of the on-screen controls, in percent of the alpha
 * channel.  The settings screen exposes 10..50 (10 is nearly invisible, 50
 * is half solid); the value is applied to every control element in every
 * overlay snapshot, so it replaces the authored per-element opacity.
 * 0 keeps the authored profile opacity.  The host applies the setting before
 * the first overlay snapshot is built.
 */
void controls_set_opacity_percent(int percent);
int controls_opacity_percent(void);

/* Rebuild the current layout at a new size.  Returns 0 on success. */
int controls_resize(int width, int height);

/*
 * Fill overlay metadata from the engine.  Call after init and after every
 * resize.  Returns 0 on success, -1 if the profile contains an unsupported
 * element type or more elements than the overlay can hold.
 */
int controls_populate_overlay(Wc3Overlay *overlay);

/*
 * Copy only visual state; element count, geometry and text are unchanged.
 * Returns 0 on success and -1 when the overlay cannot be synced.  Change
 * detection belongs to overlay_visual_state_equal(); this function must not
 * become a second publish policy.  A successful sync consumes the engine's
 * visual-dirty marks.
 */
int controls_sync_visuals(Wc3Overlay *overlay);

/*
 * 1 when the touch engine marked element visuals dirty since the last
 * successful controls_sync_visuals(); 0 when the overlay cannot have changed.
 * This is a cheap gate for the host input loop, not a second publish policy.
 */
int controls_visual_dirty(void);

const char *controls_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_CONTROLS_H */
