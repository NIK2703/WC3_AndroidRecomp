#include "controls.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/state.h"
#include "core/types/bind.h"
#include "game_state.h"

#define RECORDED_ACTIONS 256
static TouchAction g_actions[RECORDED_ACTIONS];
static int g_action_count;

static void sink(const TouchAction *actions, int count, void *user)
{
    (void)user;
    for (int i = 0; i < count && g_action_count < RECORDED_ACTIONS; i++)
        g_actions[g_action_count++] = actions[i];
}

static void actions_clear(void)
{
    g_action_count = 0;
}

#define DIRECT_RECORDED 16
static Wc3DirectTouchKind g_direct_kinds[DIRECT_RECORDED];
static Wc3DirectTouchRoute g_direct_routes[DIRECT_RECORDED];
static uint32_t g_direct_pointer_ids[DIRECT_RECORDED];
static uint32_t g_direct_gesture_ids[DIRECT_RECORDED];
static uint64_t g_direct_times[DIRECT_RECORDED];
static float g_direct_x[DIRECT_RECORDED];
static float g_direct_y[DIRECT_RECORDED];
static int g_direct_count;

static void direct_sink(const Wc3DirectTouchEvent *event, void *user)
{
    (void)user;
    if (!event || g_direct_count >= DIRECT_RECORDED) return;
    /* FIELD_HOVER is a native-cursor phase consumed by the service before
     * semantic dispatch.  Count it separately so the routing assertions
     * below keep checking the camera/selection stream only. */
    if (event->kind == WC3_DIRECT_TOUCH_FIELD_HOVER) return;
    g_direct_kinds[g_direct_count] = (Wc3DirectTouchKind)event->kind;
    g_direct_routes[g_direct_count] = (Wc3DirectTouchRoute)event->route;
    g_direct_pointer_ids[g_direct_count] = event->pointer_id;
    g_direct_gesture_ids[g_direct_count] = event->gesture_id;
    g_direct_times[g_direct_count] = event->time_ms;
    g_direct_x[g_direct_count] = event->x;
    g_direct_y[g_direct_count] = event->y;
    g_direct_count++;
}

static void direct_clear(void)
{
    g_direct_count = 0;
}

static int direct_has(Wc3DirectTouchKind kind)
{
    for (int i = 0; i < g_direct_count; i++) {
        if (g_direct_kinds[i] == kind) return 1;
    }
    return 0;
}

static int direct_has_route(Wc3DirectTouchKind kind, Wc3DirectTouchRoute route)
{
    for (int i = 0; i < g_direct_count; i++) {
        if (g_direct_kinds[i] == kind && g_direct_routes[i] == route) return 1;
    }
    return 0;
}

static int direct_index(Wc3DirectTouchKind kind)
{
    for (int i = 0; i < g_direct_count; i++) {
        if (g_direct_kinds[i] == kind) return i;
    }
    return -1;
}

static int direct_nth_index(Wc3DirectTouchKind kind, int nth)
{
    for (int i = 0; i < g_direct_count; i++) {
        if (g_direct_kinds[i] == kind && nth-- == 0) return i;
    }
    return -1;
}

static int direct_has_signal(uint32_t vk, int down)
{
    for (int i = 0; i < g_direct_count; i++) {
        if (g_direct_kinds[i] == WC3_DIRECT_TOUCH_KEY_SIGNAL &&
            g_direct_pointer_ids[i] == vk &&
            g_direct_routes[i] == (down ? 1u : 0u))
            return 1;
    }
    return 0;
}

/* The production host installs the legacy X11 table.  The test only needs
 * the keys this file presses; returning 0 keeps any other key on the old
 * keyboard path, which is exactly the documented fallback. */
static int test_key_vk(int keycode, void *user)
{
    (void)user;
    switch (keycode) {
    case 50: return 0xA0;  /* KEY_SHIFT_L -> VK_LSHIFT */
    case 37: return 0xA2;  /* KEY_CTRL_L -> VK_LCONTROL */
    case 111: return 0x26; /* KEY_UP -> VK_UP */
    case 113: return 0x25; /* KEY_LEFT -> VK_LEFT */
    case 114: return 0x27; /* KEY_RIGHT -> VK_RIGHT */
    case 116: return 0x28; /* KEY_DOWN -> VK_DOWN */
    case 9: return 0x1B;   /* KEY_ESC -> VK_ESCAPE */
    case 36: return 0x0D;  /* KEY_ENTER -> VK_RETURN */
    case 10: return 0x31;  /* KEY_1 -> '1' */
    default: return 0;
    }
}

static Wc3DirectTouchRoute route_field(void *user)
{
    (void)user;
    return WC3_DIRECT_TOUCH_ROUTE_FIELD;
}

static Wc3DirectTouchRoute route_legacy(void *user)
{
    (void)user;
    return WC3_DIRECT_TOUCH_ROUTE_LEGACY;
}

static int portrait_rect(float *left, float *top, float *right,
                         float *bottom, void *user)
{
    (void)user;
    *left = 0.02f;
    *top = 0.80f;
    *right = 0.12f;
    *bottom = 0.90f;
    return 1;
}

static int minimap_rect(float *left, float *top, float *right,
                        float *bottom, void *user)
{
    (void)user;
    *left = 0.80f;
    *top = 0.75f;
    *right = 0.95f;
    *bottom = 0.95f;
    return 1;
}

/* Deliberately inside the FIELD rectangle: the beacon gate must win over
 * the published route, otherwise the press would become a semantic tap. */
static int beacon_rect(float *left, float *top, float *right,
                       float *bottom, void *user)
{
    (void)user;
    *left = 0.40f;
    *top = 0.40f;
    *right = 0.45f;
    *bottom = 0.45f;
    return 1;
}

/* The command card's top row overlaps the field rectangle; the command gate
 * must claim it for NATIVE so the guest command handler sees raw phases. */
static int command_rect(float *left, float *top, float *right,
                        float *bottom, void *user)
{
    (void)user;
    *left = 0.60f;
    *top = 0.78f;
    *right = 0.70f;
    *bottom = 0.84f;
    return 1;
}

static int has_pointer_move(int x, int y)
{
    for (int i = 0; i < g_action_count; i++) {
        if (g_actions[i].type == ACT_POINTER_MOVE &&
            g_actions[i].pointer_move.x == x &&
            g_actions[i].pointer_move.y == y)
            return 1;
    }
    return 0;
}

static int has_button(int button, int down)
{
    for (int i = 0; i < g_action_count; i++) {
        if (g_actions[i].type ==
                (down ? ACT_POINTER_BUTTON_PRESS
                      : ACT_POINTER_BUTTON_RELEASE) &&
            g_actions[i].pointer_button.button == button)
            return 1;
    }
    return 0;
}

static int has_key(int keycode, int down)
{
    for (int i = 0; i < g_action_count; i++) {
        if (g_actions[i].type == (down ? ACT_KEY_PRESS : ACT_KEY_RELEASE) &&
            g_actions[i].key.keycode == keycode)
            return 1;
    }
    return 0;
}

static const TouchElement *find_key(int keycode)
{
    for (int i = 0; i < g_state.element_count; i++) {
        const TouchElement *element = &g_state.elements[i];
        if (element->bindings[0].type >= KEYBOARD_FIRST &&
            element->bindings[0].type < KEYBOARD_LAST &&
            element->bindings[0].keycode == keycode)
            return element;
    }
    return NULL;
}

static char *read_asset(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    if (fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static void set_transform(float xform_x, float xform_y, float view_x,
                          float view_y)
{
    TP_STATE_LOCK();
    g_state.cfg.xform_scale_x = xform_x;
    g_state.cfg.xform_scale_y = xform_y;
    g_state.cfg.view_offset_x = view_x;
    g_state.cfg.view_offset_y = view_y;
    TP_STATE_UNLOCK();
}

static void assert_transform(float xform_x, float xform_y, float view_x,
                             float view_y)
{
    TP_STATE_LOCK();
    assert(g_state.cfg.xform_scale_x == xform_x);
    assert(g_state.cfg.xform_scale_y == xform_y);
    assert(g_state.cfg.view_offset_x == view_x);
    assert(g_state.cfg.view_offset_y == view_y);
    TP_STATE_UNLOCK();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <profile.icp>\n", argv[0]);
        return 2;
    }

    char *json = read_asset(argv[1]);
    if (!json) {
        fprintf(stderr, "controls_mode: asset missing\n");
        return 2;
    }

    assert(controls_init_ex(json, 1000, 1000, false, 0, 0, 0) == 0);
    assert(controls_layout_id() == 1);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_FULL);
    assert(controls_switch_safe() == 1);
    assert(g_state.element_count == 12);

    /* ESC and ENTER have equal widths and heights, a small gap between them,
     * and together span exactly SHIFT's width.  Keycodes from the authored
     * profile binding table: SHIFT_L=50, ENTER=36, ESC=9. */
    const TouchElement *shift = find_key(50);
    const TouchElement *enter = find_key(36);
    const TouchElement *esc = find_key(9);
    assert(shift && enter && esc);
    assert(fabsf(esc->hw - enter->hw) < 0.5f);
    assert(fabsf(esc->hh - enter->hh) < 0.5f);
    const float gap = (enter->x - enter->hw) - (esc->x + esc->hw);
    assert(gap > 0.5f);
    assert(gap < 0.05f * (2.0f * shift->hw));
    assert(fabsf(((enter->x + enter->hw) - (esc->x - esc->hw)) -
                 (2.0f * shift->hw)) < 0.5f);
    assert(fabsf((esc->x - esc->hw) - (shift->x - shift->hw)) < 1.0f);
    assert(fabsf((enter->x + enter->hw) - (shift->x + shift->hw)) < 1.0f);
    assert(fabsf((esc->y - esc->hh) - (enter->y - enter->hh)) < 0.5f);

    /* An active finger rejects the switch without changing mode. */
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 4.0f, 4.0f, 1000) == 0);
    assert(controls_switch_safe() == 0);
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_SIMPLE) == -2);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_FULL);
    controls_cancel_all();
    assert(controls_switch_safe() == 1);

    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_SIMPLE) == 0);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_SIMPLE);
    assert(controls_layout_id() == 2);
    /* SIMPLE has no on-screen controls; LMB stays the global single tap. */
    assert(g_state.element_count == 0);
    assert(g_state.cfg.touch_mode == TOUCHSCREEN);
    assert(g_state.cfg.gestures[SINGLE_TAP].count == 1);
    assert(g_state.cfg.gestures[SINGLE_TAP].arr[0].type == MOUSE_LEFT_BUTTON);
    for (int i = 0; i < GESTURE_TYPE_COUNT; i++) {
        if (i == SINGLE_TAP) continue;
        assert(g_state.cfg.gestures[i].count == 0);
    }

    /* With no elements every tap is a field tap: it moves the cursor and
     * presses LMB on finger-down; the release stays on finger-up.  The press
     * may only follow the move. */
    controls_set_action_sink(sink, NULL);
    actions_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 1000) == 0);
    controls_tick(1000);
    assert(has_pointer_move(500, 500));
    assert(has_button(0, 1));
    assert(!has_button(0, 0));

    int move_index = -1;
    int press_index = -1;
    for (int i = 0; i < g_action_count; i++) {
        if (move_index < 0 && g_actions[i].type == ACT_POINTER_MOVE &&
            g_actions[i].pointer_move.x == 500 &&
            g_actions[i].pointer_move.y == 500)
            move_index = i;
        if (press_index < 0 && g_actions[i].type == ACT_POINTER_BUTTON_PRESS &&
            g_actions[i].pointer_button.button == 0)
            press_index = i;
    }
    assert(move_index >= 0 && press_index > move_index);

    actions_clear();
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 1050) == 0);
    controls_tick(1200);
    assert(has_button(0, 0));
    assert(!has_button(0, 1));
    assert(!has_pointer_move(500, 500));

    /* A drag that starts after the down-click must not add another click on
     * finger-up.  Only the held press from the down event may be released. */
    actions_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 300.0f, 500.0f, 3000) == 0);
    controls_tick(3000);
    assert(has_button(0, 1));
    actions_clear();
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 300.0f, 100.0f, 3020) == 0);
    controls_tick(3030);
    assert(controls_touch(WC3_TOUCH_UP, 0, 300.0f, 100.0f, 3050) == 0);
    controls_tick(3060);
    assert(!has_button(0, 1));
    controls_cancel_all();
    assert(controls_switch_safe() == 1);

    /* The snapshot carries the exact layout id used by the engine. */
    Wc3Overlay snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    overlay_init(&snapshot, 1000, 1000, NULL);
    assert(controls_populate_overlay(&snapshot) == 0);
    assert(snapshot.layout_id == controls_layout_id());
    assert(snapshot.element_count == 0);
    Wc3Overlay other = snapshot;
    other.layout_id++;
    assert(!overlay_visual_state_equal(&snapshot, &other));
    overlay_destroy(&snapshot);

    /* Resize keeps the active variant and the host transform. */
    set_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_resize(800, 600) == 0);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_SIMPLE);
    assert(g_state.element_count == 0);
    assert_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_layout_id() == 3);

    /* Cinematic variant: one authored ESC element and the same simplified
     * global gestures.  The ESC element must send KEY_ESC, not LMB. */
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_SIMPLE_ESC) == 0);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_SIMPLE_ESC);
    assert(controls_layout_id() == 4);
    assert(g_state.element_count == 1);
    assert(g_state.elements[0].type == BUTTON);
    assert(g_state.elements[0].bindings[0].type == KEYBOARD_FIRST + 9);
    assert(g_state.elements[0].bindings[0].keycode == 9);
    assert(g_state.elements[0].bindings[1].type == NONE);
    assert(g_state.g_element_long_press_count[0] == 0);
    assert(g_state.g_element_gesture_count[0] == 0);
    assert(g_state.cfg.touch_mode == TOUCHSCREEN);
    assert(g_state.cfg.gestures[SINGLE_TAP].count == 1);
    assert(g_state.cfg.gestures[SINGLE_TAP].arr[0].type == MOUSE_LEFT_BUTTON);
    for (int i = 0; i < GESTURE_TYPE_COUNT; i++) {
        if (i == SINGLE_TAP) continue;
        assert(g_state.cfg.gestures[i].count == 0);
    }

    controls_set_action_sink(sink, NULL);
    actions_clear();
    const float ex = g_state.elements[0].x + g_state.elements[0].w * 0.5f;
    const float ey = g_state.elements[0].y + g_state.elements[0].h * 0.5f;
    assert(controls_touch(WC3_TOUCH_DOWN, 0, ex, ey, 2000) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, ex, ey, 2050) == 0);
    controls_tick(2200);
    assert(has_key(9, 1));
    assert(has_key(9, 0));
    assert(!has_button(0, 1));
    controls_cancel_all();
    assert(controls_switch_safe() == 1);

    /* Resize keeps SIMPLE_ESC and the host transform. */
    assert(controls_resize(900, 700) == 0);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_SIMPLE_ESC);
    assert(g_state.element_count == 1);
    assert(g_state.elements[0].bindings[0].keycode == 9);
    assert_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_layout_id() == 5);

    /* Switch back using the same preserved transform. */
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_FULL) == 0);
    assert(g_state.element_count == 12);
    assert_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_layout_id() == 6);

    /* The product opacity is one value for every control element and
     * replaces the authored per-element opacity. */
    controls_set_opacity_percent(30);
    assert(controls_opacity_percent() == 30);
    Wc3Overlay faded;
    memset(&faded, 0, sizeof faded);
    overlay_init(&faded, 1000, 1000, NULL);
    assert(controls_populate_overlay(&faded) == 0);
    assert(faded.element_count == 12);
    for (int i = 0; i < faded.element_count; i++) {
        assert(faded.elements[i].has_opacity);
        assert(fabsf(faded.elements[i].opacity - 0.30f) < 0.001f);
    }
    overlay_destroy(&faded);

    /* The native clamp repeats the settings-screen range. */
    controls_set_opacity_percent(5);
    assert(controls_opacity_percent() == 10);
    controls_set_opacity_percent(90);
    assert(controls_opacity_percent() == 50);

    /* 0 keeps the authored profile opacity. */
    controls_set_opacity_percent(0);
    assert(controls_opacity_percent() == 0);
    Wc3Overlay plain;
    memset(&plain, 0, sizeof plain);
    overlay_init(&plain, 1000, 1000, NULL);
    assert(controls_populate_overlay(&plain) == 0);
    for (int i = 0; i < plain.element_count; i++) {
        assert(!plain.elements[i].has_opacity);
    }
    overlay_destroy(&plain);

    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_FULL) == 0);
    assert(controls_layout_id() == 6);

    /* Direct-touch routing: an overlay element stays on the legacy engine
     * even inside the battlefield rectangle; a battlefield touch uses the
     * semantic recognizer; native HUD outside the rectangle is forwarded as
     * raw pointer phases.  This is the split the host gate publishes. */
    set_transform(1.0f, 1.0f, 0.0f, 0.0f);
    assert(controls_resize(1000, 1000) == 0);
    assert(g_state.element_count == 12);
    controls_set_action_sink(sink, NULL);
    controls_set_direct_touch_sink(direct_sink, NULL);
    controls_set_direct_touch_route_gate(route_field, NULL);
    controls_set_direct_touch_portrait_gate(portrait_rect, NULL);
    controls_set_direct_touch_minimap_gate(minimap_rect, NULL);
    controls_set_direct_touch_beacon_gate(beacon_rect, NULL);
    controls_set_direct_touch_command_gate(command_rect, NULL);
    controls_set_direct_touch_enabled(true);
    controls_set_key_signal_vk_map(test_key_vk, NULL);

    const TouchElement *shift_element = find_key(50);
    assert(shift_element);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, shift_element->x,
                          shift_element->y, 5000) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, shift_element->x,
                          shift_element->y, 5050) == 0);
    controls_tick(5100);
    /* Standalone SHIFT has no direct guest action: it stays on the normal
     * keyboard path. */
    assert(has_key(50, 1));
    assert(has_key(50, 0));
    assert(!direct_has_signal(0xA0, 1));
    assert(!direct_has_signal(0xA0, 0));

    /* Arrows have no verified direct guest action either; they stay on the
     * keyboard path, which is the game's own camera/arrow signal. */
    const TouchElement *dpad = NULL;
    for (int i = 0; i < g_state.element_count; i++) {
        if (g_state.elements[i].type == D_PAD) {
            dpad = &g_state.elements[i];
            break;
        }
    }
    assert(dpad);
    const float dpad_x = dpad->x - dpad->hw * 0.9f;
    const float dpad_y = dpad->y;
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, dpad_x, dpad_y, 5400) == 0);
    controls_tick(5410);
    assert(has_key(113, 1)); /* KEY_LEFT reaches the game keyboard path */
    assert(!direct_has_signal(0x25, 1));
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_UP, 0, dpad_x, dpad_y, 5420) == 0);
    controls_tick(5430);
    assert(has_key(113, 0));
    assert(!direct_has_signal(0x25, 0));

    /* The command-card buttons are key signals too: button 1 is bound to
     * KEY_1 in the authored profile and must not stay on the keyboard path. */
    const TouchElement *one = find_key(10);
    assert(one);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, one->x, one->y, 5440) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, one->x, one->y, 5450) == 0);
    controls_tick(5460);
    /* A digit is one direct group action, never a key plus a modifier.  Tap
     * is route 0 (recall); the release is swallowed by the chord filter. */
    assert(!has_key(10, 1));
    assert(!has_key(10, 0));
    int tap_group = direct_index(WC3_DIRECT_TOUCH_GROUP_SIGNAL);
    assert(tap_group >= 0);
    assert(g_direct_pointer_ids[tap_group] == 1);
    assert(g_direct_routes[tap_group] == 0);
    assert(!direct_has_signal('1', 1));

    /* Long press on the same button fires the authored KEY_1 + MOD_CTRL
     * chord as a single store action (route 1), with no modifier key signal. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, one->x, one->y, 5500) == 0);
    controls_tick(5700);
    assert(!has_key(10, 1));
    assert(!has_key(37, 1));
    int store_group = direct_index(WC3_DIRECT_TOUCH_GROUP_SIGNAL);
    assert(store_group >= 0);
    assert(g_direct_pointer_ids[store_group] == 1);
    assert(g_direct_routes[store_group] == 1);
    assert(!direct_has_signal(0x31, 1));
    assert(!direct_has_signal(0xA2, 1));
    assert(controls_touch(WC3_TOUCH_UP, 0, one->x, one->y, 5750) == 0);
    controls_tick(5800);

    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 5200) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 5250) == 0);
    controls_tick(5300);
    assert(g_action_count == 0);
    assert(direct_has(WC3_DIRECT_TOUCH_TAP));

    /* A LEGACY snapshot means the guest has no live CGameUI.  The main menu
     * uses the same rectangle as the battlefield, so a point inside that
     * rectangle must not be guessed as a FIELD gesture.  Forward it as raw
     * native phases: the HLE resolves a live FIELD at release when the
     * published snapshot was one pump stale, and otherwise posts the native
     * menu click.  The first tap after map load still reaches FIELD through
     * that live release resolver. */
    controls_set_direct_touch_route_gate(route_legacy, NULL);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 5310) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 5320) == 0);
    controls_tick(5330);
    assert(g_action_count == 0);
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_UP));
    controls_set_direct_touch_route_gate(route_field, NULL);

    /* The same snapshot outside the field is also a native click, not a
     * vendor touch.  Direct mode has no legacy fallback for game input. */
    controls_set_direct_touch_route_gate(route_legacy, NULL);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 900.0f, 900.0f, 5360) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 900.0f, 900.0f, 5370) == 0);
    controls_tick(5390);
    assert(g_action_count == 0);
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_UP));
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    controls_set_direct_touch_route_gate(route_field, NULL);

    /* The menu overlay may still be SIMPLE during a load.  That policy must
     * not send a battlefield contact to the legacy engine. */
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_SIMPLE) == 0);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 5395) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 5397) == 0);
    controls_tick(5399);
    assert(direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(!direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_FULL) == 0);

    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 900.0f, 900.0f, 5400) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 900.0f, 900.0f, 5450) == 0);
    controls_tick(5500);
    assert(g_action_count == 0);
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_UP));
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));

    /* Two field fingers can own the camera pan at the same time.  The second
     * finger is not forced to ACTION while the first camera gesture is live;
     * its own quick swipe emits a separate camera gesture, and releasing the
     * first finger leaves the second one panning. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 5460) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 530.0f, 500.0f, 5470) == 0);
    assert(controls_touch(WC3_TOUCH_DOWN, 1, 700.0f, 600.0f, 5475) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 1, 740.0f, 600.0f, 5480) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 530.0f, 500.0f, 5485) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 1, 780.0f, 620.0f, 5490) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 1, 780.0f, 620.0f, 5495) == 0);
    assert(g_direct_count == 5);
    assert(g_direct_kinds[0] == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_direct_pointer_ids[0] == 0);
    assert(g_direct_kinds[1] == WC3_DIRECT_TOUCH_FIELD_CAMERA_BEGIN);
    assert(g_direct_pointer_ids[1] == 1);
    assert(g_direct_gesture_ids[0] != 0u);
    assert(g_direct_gesture_ids[1] != 0u);
    assert(g_direct_gesture_ids[0] != g_direct_gesture_ids[1]);
    assert(g_direct_kinds[2] == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_direct_pointer_ids[2] == 0);
    assert(g_direct_kinds[3] == WC3_DIRECT_TOUCH_FIELD_CAMERA_MOVE);
    assert(g_direct_pointer_ids[3] == 1);
    assert(g_direct_kinds[4] == WC3_DIRECT_TOUCH_FIELD_CAMERA_END);
    assert(g_direct_pointer_ids[4] == 1);
    controls_tick(5500);

    /* A native button is pressed on DOWN and confirmed only on UP.  Dragging
     * off the button must forward the move and the release at the new point;
     * controls must not synthesize a click at the original press point, nor
     * keep the release at the button.  This is the menu press/slide-off
     * contract: activation is the guest UI's hit-test on WM_LBUTTONUP. */
    controls_set_direct_touch_route_gate(route_legacy, NULL);
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 900.0f, 224.0f, 5510) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 950.0f, 300.0f, 5520) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 950.0f, 300.0f, 5530) == 0);
    controls_tick(5540);
    assert(g_action_count == 0);
    assert(g_direct_count == 3);
    assert(g_direct_kinds[0] == WC3_DIRECT_TOUCH_POINTER_DOWN);
    assert(fabsf(g_direct_x[0] - 0.900f) < 0.0001f);
    assert(fabsf(g_direct_y[0] - 0.224f) < 0.0001f);
    assert(g_direct_kinds[1] == WC3_DIRECT_TOUCH_POINTER_MOVE);
    assert(fabsf(g_direct_x[1] - 0.950f) < 0.0001f);
    assert(fabsf(g_direct_y[1] - 0.300f) < 0.0001f);
    assert(g_direct_kinds[2] == WC3_DIRECT_TOUCH_POINTER_UP);
    assert(fabsf(g_direct_x[2] - 0.950f) < 0.0001f);
    assert(fabsf(g_direct_y[2] - 0.300f) < 0.0001f);
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    controls_set_direct_touch_route_gate(route_field, NULL);

    /* Portrait: follow starts immediately on DOWN and stops on UP.  No
     * long-press deadline and no separate tap action.  The portrait frame
     * overlaps the field's lower edge, so its own route must run before the
     * field/native split; without it this touch would be a dead native
     * mouse click. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 70.0f, 850.0f, 5600) == 0);
    assert(direct_has_route(WC3_DIRECT_TOUCH_FOLLOW_START,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!direct_has(WC3_DIRECT_TOUCH_LONG_PRESS));
    assert(controls_touch(WC3_TOUCH_UP, 0, 70.0f, 850.0f, 5650) == 0);
    controls_tick(5700);
    assert(g_action_count == 0);
    assert(direct_has_route(WC3_DIRECT_TOUCH_HOLD_RELEASE,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(!direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));

    /* Moving off the portrait keeps the follow; the tick must not turn it
     * into a long-press event. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 70.0f, 850.0f, 5800) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 500.0f, 500.0f, 5900) == 0);
    controls_tick(6250);
    assert(direct_has(WC3_DIRECT_TOUCH_FOLLOW_START));
    assert(!direct_has(WC3_DIRECT_TOUCH_LONG_PRESS));
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 6300) == 0);
    controls_tick(6350);
    assert(direct_has_route(WC3_DIRECT_TOUCH_HOLD_RELEASE,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));

    /* A second finger on the field does not cancel the portrait follow; it
     * is swallowed until the portrait finger is released. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 70.0f, 850.0f, 6400) == 0);
    assert(controls_touch(WC3_TOUCH_DOWN, 1, 500.0f, 500.0f, 6410) == 0);
    assert(direct_has_route(WC3_DIRECT_TOUCH_FOLLOW_START,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!direct_has(WC3_DIRECT_TOUCH_HOLD_RELEASE));
    assert(controls_touch(WC3_TOUCH_UP, 0, 70.0f, 850.0f, 6500) == 0);
    assert(direct_has_route(WC3_DIRECT_TOUCH_HOLD_RELEASE,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(controls_touch(WC3_TOUCH_UP, 1, 500.0f, 500.0f, 6510) == 0);
    assert(g_action_count == 0);

    /* An overlay element pressed while the portrait follow is active still
     * reaches the legacy engine; it must not release the follow. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 70.0f, 850.0f, 6520) == 0);
    assert(controls_touch(WC3_TOUCH_DOWN, 1, shift_element->x,
                          shift_element->y, 6530) == 0);
    controls_tick(6550);
    assert(has_key(50, 1));
    assert(!direct_has_signal(0xA0, 1));
    assert(direct_has_route(WC3_DIRECT_TOUCH_FOLLOW_START,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));
    assert(!direct_has(WC3_DIRECT_TOUCH_HOLD_RELEASE));
    assert(controls_touch(WC3_TOUCH_UP, 1, shift_element->x,
                          shift_element->y, 6560) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 70.0f, 850.0f, 6570) == 0);
    controls_tick(6580);
    assert(has_key(50, 0));
    assert(!direct_has_signal(0xA0, 0));
    assert(direct_has_route(WC3_DIRECT_TOUCH_HOLD_RELEASE,
                            WC3_DIRECT_TOUCH_ROUTE_PORTRAIT));

    /* Minimap: the finger that starts on the minimap keeps the drag until it
     * is released.  Moves outside the rectangle are clamped to its edge so
     * the native minimap handler still receives the drag. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 850.0f, 800.0f, 6600) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 100.0f, 100.0f, 6610) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 990.0f, 990.0f, 6620) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 100.0f, 100.0f, 6630) == 0);
    controls_tick(6700);
    assert(g_action_count == 0);
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(!direct_has(WC3_DIRECT_TOUCH_LONG_PRESS));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_MOVE));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_UP));
    const int mm_down = direct_index(WC3_DIRECT_TOUCH_POINTER_DOWN);
    const int mm_move1 = direct_nth_index(WC3_DIRECT_TOUCH_POINTER_MOVE, 0);
    const int mm_move2 = direct_nth_index(WC3_DIRECT_TOUCH_POINTER_MOVE, 1);
    const int mm_up = direct_index(WC3_DIRECT_TOUCH_POINTER_UP);
    assert(mm_down >= 0 && mm_move1 >= 0 && mm_move2 >= 0 && mm_up >= 0);
    assert(fabsf(g_direct_x[mm_down] - 0.85f) < 0.0001f);
    assert(fabsf(g_direct_y[mm_down] - 0.80f) < 0.0001f);
    assert(fabsf(g_direct_x[mm_move1] - 0.80f) < 0.0001f);
    assert(fabsf(g_direct_y[mm_move1] - 0.75f) < 0.0001f);
    assert(fabsf(g_direct_x[mm_move2] - 0.95f) < 0.0001f);
    assert(fabsf(g_direct_y[mm_move2] - 0.95f) < 0.0001f);
    assert(fabsf(g_direct_x[mm_up] - 0.80f) < 0.0001f);
    assert(fabsf(g_direct_y[mm_up] - 0.75f) < 0.0001f);

    /* The minimap owner is released with the pointer: the next field touch
     * is a normal semantic tap again. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 500.0f, 500.0f, 6800) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 500.0f, 500.0f, 6850) == 0);
    controls_tick(6900);
    assert(direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(!direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));

    /* Beacon button: its rectangle is inside the FIELD route on purpose.
     * The beacon gate must force the press to NATIVE, so it is not a
     * semantic tap and its drag is not clamped to the minimap.  The HLE
     * claims these raw phases and turns the release into a beacon. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 420.0f, 420.0f, 7000) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, 850.0f, 800.0f, 7010) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 850.0f, 800.0f, 7020) == 0);
    controls_tick(7100);
    assert(g_action_count == 0);
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    const int bc_down = direct_index(WC3_DIRECT_TOUCH_POINTER_DOWN);
    const int bc_move = direct_index(WC3_DIRECT_TOUCH_POINTER_MOVE);
    const int bc_up = direct_index(WC3_DIRECT_TOUCH_POINTER_UP);
    assert(bc_down >= 0 && bc_move >= 0 && bc_up >= 0);
    assert(fabsf(g_direct_x[bc_down] - 0.42f) < 0.0001f);
    assert(fabsf(g_direct_y[bc_down] - 0.42f) < 0.0001f);
    assert(fabsf(g_direct_x[bc_move] - 0.85f) < 0.0001f);
    assert(fabsf(g_direct_y[bc_move] - 0.80f) < 0.0001f);
    assert(fabsf(g_direct_x[bc_up] - 0.85f) < 0.0001f);
    assert(fabsf(g_direct_y[bc_up] - 0.80f) < 0.0001f);

    /* Outside the beacon rectangle the field route is unchanged. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 550.0f, 550.0f, 7200) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 550.0f, 550.0f, 7250) == 0);
    controls_tick(7300);
    assert(direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(!direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));

    /* Command card: the top row starts above the field bottom, so the
     * field test would claim it.  The command gate must force the press to
     * NATIVE; the guest command handler then hit-tests the real buttons. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, 650.0f, 800.0f, 7400) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, 650.0f, 800.0f, 7450) == 0);
    controls_tick(7500);
    assert(g_action_count == 0);
    assert(!direct_has(WC3_DIRECT_TOUCH_TAP));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_DOWN));
    assert(direct_has(WC3_DIRECT_TOUCH_POINTER_UP));

    /* Swipe on a digit folds its authored KEY_N + MOD_SHIFT chord into one
     * direct group action: group 1, route 2 (add to the control group). */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, one->x, one->y, 7510) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, one->x - 80.0f, one->y, 7520) == 0);
    assert(controls_touch(WC3_TOUCH_MOVE, 0, one->x - 120.0f, one->y, 7530) == 0);
    controls_tick(7540);
    assert(!has_key(10, 1));
    assert(!has_key(50, 1));
    int group_index = direct_index(WC3_DIRECT_TOUCH_GROUP_SIGNAL);
    assert(group_index >= 0);
    assert(g_direct_pointer_ids[group_index] == 1);
    assert(g_direct_routes[group_index] == 2);
    assert(controls_touch(WC3_TOUCH_UP, 0, one->x - 120.0f, one->y, 7550) == 0);
    controls_tick(7560);

    /* Holding an overlay control must not lock out the second finger.  The
     * D-pad finger owns only the vendor overlay path; a battlefield contact
     * with another finger stays a direct game action, and a native HUD
     * contact still reaches the raw pointer route. */
    actions_clear();
    direct_clear();
    assert(controls_touch(WC3_TOUCH_DOWN, 0, dpad_x, dpad_y, 7600) == 0);
    controls_tick(7610);
    assert(has_key(113, 1));
    assert(!direct_has_signal(0x25, 1));
    actions_clear(); /* the held arrow action is not a second-finger action */
    assert(controls_touch(WC3_TOUCH_DOWN, 1, 500.0f, 500.0f, 7620) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 1, 500.0f, 500.0f, 7630) == 0);
    controls_tick(7640);
    const int held_tap = direct_index(WC3_DIRECT_TOUCH_TAP);
    assert(held_tap >= 0);
    assert(g_direct_pointer_ids[held_tap] == 1);
    assert(g_direct_routes[held_tap] == WC3_DIRECT_TOUCH_ROUTE_FIELD);
    assert(g_action_count == 0);
    assert(controls_touch(WC3_TOUCH_DOWN, 2, 650.0f, 800.0f, 7650) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 2, 650.0f, 800.0f, 7660) == 0);
    controls_tick(7670);
    assert(direct_has_route(WC3_DIRECT_TOUCH_POINTER_DOWN,
                            WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    assert(direct_has_route(WC3_DIRECT_TOUCH_POINTER_UP,
                            WC3_DIRECT_TOUCH_ROUTE_NATIVE));
    assert(controls_touch(WC3_TOUCH_UP, 0, dpad_x, dpad_y, 7680) == 0);
    controls_tick(7690);
    assert(has_key(113, 0));
    assert(!direct_has_signal(0x25, 0));

    controls_set_direct_touch_enabled(false);
    controls_set_direct_touch_sink(NULL, NULL);
    controls_set_direct_touch_route_gate(NULL, NULL);
    controls_set_direct_touch_portrait_gate(NULL, NULL);
    controls_set_direct_touch_minimap_gate(NULL, NULL);
    controls_set_direct_touch_beacon_gate(NULL, NULL);
    controls_set_direct_touch_command_gate(NULL, NULL);
    controls_set_action_sink(NULL, NULL);
    controls_cancel_all();
    free(json);
    printf("controls_mode: okay\n");
    return 0;
}
