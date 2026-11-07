// Mouse move tick and delayed action processing.
// Handles: process_delayed_actions.

#include "core/internal.h"
#include "xserver/core/x11_dispatch.h"

extern void udp_mouse_send(int flags, int dx, int dy, int wheel);
#define MF_MOVE 0x0001

extern TouchProcessorState g_state;

void process_delayed_actions(TouchActionResult* restrict result, uint64_t now_tick) {
    if (g_state.sim_click_press_tick == 0
        && g_state.sim_click_release_tick == 0)
        return;

    if (g_state.sim_click_press_tick > 0 && now_tick >= g_state.sim_click_press_tick) {
        TouchFinger* sf = find_finger(g_state.sim_click_ptr_id);
        if (sf) {
            add_action(result, ACT_POINTER_MOVE, g_state.last_touch_x, g_state.last_touch_y, 0);
            if (g_state.sim_continue_click) {
                add_action(result, ACT_POINTER_BUTTON_PRESS, MOUSE_BTN_LEFT, 0, 0);
                g_state.sim_click_release_tick = now_tick + g_state.cfg.sim_click_release_ticks;
            }
        }
        g_state.sim_click_press_tick = 0;
        g_state.sim_click_ptr_id = -1;
    }

    if (g_state.sim_click_release_tick > 0 && now_tick >= g_state.sim_click_release_tick) {
        if (g_state.sim_continue_click)
            add_action(result, ACT_POINTER_BUTTON_RELEASE, MOUSE_BTN_LEFT, 0, 0);
        g_state.sim_click_release_tick = 0;
        g_state.sim_continue_click = false;
        g_state.sim_click_ptr_id = -1;
    }
}
