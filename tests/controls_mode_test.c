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
    assert(g_state.element_count == 1);
    assert(g_state.elements[0].type == BUTTON);
    /* The element itself is ESC.  LMB is the global single tap, not slot 0. */
    assert(g_state.elements[0].bindings[0].type == KEYBOARD_FIRST + 9);
    assert(g_state.elements[0].bindings[0].keycode == 9);
    assert(g_state.elements[0].bindings[1].type == NONE);
    assert(g_state.elements[0].x == 91);
    assert(g_state.elements[0].y == 111);
    assert(g_state.g_element_long_press_count[0] == 0);
    assert(g_state.g_element_gesture_count[0] == 0);
    assert(g_state.cfg.touch_mode == TOUCHSCREEN);
    assert(g_state.cfg.gestures[SINGLE_TAP].count == 1);
    assert(g_state.cfg.gestures[SINGLE_TAP].arr[0].type == MOUSE_LEFT_BUTTON);
    for (int i = 0; i < GESTURE_TYPE_COUNT; i++) {
        if (i == SINGLE_TAP) continue;
        assert(g_state.cfg.gestures[i].count == 0);
    }

    /* A tap outside ESC moves the cursor and presses LMB on finger-down; the
     * release stays on finger-up.  The press may only follow the move, and a
     * tap on the ESC element must send KEY_ESC instead. */
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

    actions_clear();
    const float ex = g_state.elements[0].x + g_state.elements[0].w * 0.5f;
    const float ey = g_state.elements[0].y + g_state.elements[0].h * 0.5f;
    assert(controls_touch(WC3_TOUCH_DOWN, 0, ex, ey, 2000) == 0);
    assert(controls_touch(WC3_TOUCH_UP, 0, ex, ey, 2050) == 0);
    controls_tick(2200);
    assert(has_key(9, 1));
    assert(has_key(9, 0));
    assert(!has_button(0, 1));

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
    Wc3Overlay other = snapshot;
    other.layout_id++;
    assert(!overlay_visual_state_equal(&snapshot, &other));
    overlay_destroy(&snapshot);

    /* Resize keeps the active variant and the host transform. */
    set_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_resize(800, 600) == 0);
    assert(controls_overlay_mode() == WC3_OVERLAY_MODE_SIMPLE);
    assert(g_state.element_count == 1);
    assert(g_state.elements[0].x == 73);
    assert(g_state.elements[0].y == 67);
    assert_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_layout_id() == 3);

    /* Switch back using the same preserved transform. */
    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_FULL) == 0);
    assert(g_state.element_count == 12);
    assert_transform(1.5f, 1.25f, 7.0f, -3.0f);
    assert(controls_layout_id() == 4);

    assert(controls_set_overlay_mode(WC3_OVERLAY_MODE_FULL) == 0);
    assert(controls_layout_id() == 4);

    controls_cancel_all();
    free(json);
    printf("controls_mode: okay\n");
    return 0;
}
