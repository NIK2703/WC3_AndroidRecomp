#include "core/internal.h"


// X11 keycodes (not keysyms): keycode = scancode + 8 on most X servers
int range_keycode(int ordinal, int index) {
    // alphabet[0]=a(38), [1]=b(56), [2]=c(54), [3]=d(40), [4]=e(26), [5]=f(41), [6]=g(42), [7]=h(43),
    // [8]=i(31), [9]=j(44), [10]=k(45), [11]=l(46), [12]=m(58), [13]=n(57), [14]=o(32), [15]=p(33),
    // [16]=q(24), [17]=r(27), [18]=s(39), [19]=t(28), [20]=u(30), [21]=v(55), [22]=w(25), [23]=x(53),
    // [24]=y(29), [25]=z(52)
    static const int alphabet[26] = {38,56,54,40,26,41,42,43,31,44,45,46,58,57,32,33,24,27,39,28,30,55,25,53,29,52};
    // number[0]=0(10), [1]=1(11), [2]=2(12), [3]=3(13), [4]=4(14), [5]=5(15), [6]=6(16), [7]=7(17), [8]=8(18), [9]=9(19)
    static const int number[10]  = {10,11,12,13,14,15,16,17,18,19};
    // function[0]=F1(67), [1]=F2(68), [2]=F3(69), [3]=F4(70), [4]=F5(71), [5]=F6(72),
    // [6]=F7(73), [7]=F8(74), [8]=F9(75), [9]=F10(76), [10]=F11(95), [11]=F12(96)
    static const int function[12]= {67,68,69,70,71,72,73,74,75,76,95,96};
    // numpad[0]=NumPad0(87), [1]=NumPad1(88), [2]=NumPad2(89), [3]=NumPad3(83), [4]=NumPad4(84),
    // [5]=NumPad5(85), [6]=NumPad6(79), [7]=NumPad7(80), [8]=NumPad8(81), [9]=NumPad9(90)
    static const int numpad[10]  = {87,88,89,83,84,85,79,80,81,90};
    switch (ordinal) {
        case 0: if (index >= 0 && index < 26) { int kc = alphabet[index]; return kc; } break;
        case 1: if (index >= 0 && index < 10) { int kc = number[index]; return kc; } break;
        case 2: if (index >= 0 && index < 12) { int kc = function[index]; return kc; } break;
        case 3: if (index >= 0 && index < 10) { int kc = numpad[index]; return kc; } break;
    }
    return 0;
}

static inline void range_reset_state(TouchElement* e) {
    e->range_scrolling = false;
    e->range_has_binding = false;
    e->range_hold_pressed = false;
}

static inline void range_handle_hold_release(TouchElement* e, TouchActionResult* restrict result) {
    int kc = e->range_initial_kc;
    if (kc > 0)
        add_action(result, ACT_KEY_RELEASE, kc, 0, 0);
    range_reset_state(e);
}

static inline void range_handle_tap(TouchElement* e, uint64_t now_tick, TouchActionResult* restrict result) {
    uint64_t duration = now_tick - e->down_tick;
    bool is_tap = duration < (uint64_t)g_state.cfg.long_press_delay_ticks && !e->range_scrolling;

    int kc = e->range_initial_kc;


    if (kc > 0) {
        if (is_tap) {
            // Tap (duration < RANGE_TAP_TIMEOUT_MS, no scroll): press immediately, schedule release for next tick
            // (matches Java postDelayed 30ms, but non-blocking)
            add_action(result, ACT_KEY_PRESS, kc, 1, 0);
            // Consumed by touch_processor_tick() deferred release handler
            e->range_pending_tap_release = true;
            e->range_tap_release_tick = now_tick + g_state.cfg.range_tap_release_ticks;
            // Don't clear flags yet — deferred release in tick will do it
        } else {
            // Hold (>=RANGE_TAP_TIMEOUT_MS without hold-press, or scroll): press + release immediately
            add_action(result, ACT_KEY_PRESS, kc, 1, 0);
            add_action(result, ACT_KEY_RELEASE, kc, 0, 0);
            range_reset_state(e);
        }
    } else {
        range_reset_state(e);
    }
}

void element_range_button_down(TouchElement* e, int ptr_id, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)ptr_id;
    (void)result;
    if (__builtin_expect(e->range_max <= 0, 0)) return;
    g_state.has_active_ranges = true;
    range_reset_state(e);
    e->range_pending_tap_release = false;
    e->range_tap_release_tick = 0;
    e->range_last_position = e->range_orientation == 0 ? x : y;

    float cw = e->cached_range_cw;
    float ch = e->cached_range_ch;
    float element_size = e->cached_range_element_size;
    if (element_size <= 0.0f) return;
    float left = e->x - cw;
    float top  = e->y - ch;

    float offset = e->range_orientation == 0 ? x - left + e->range_current_offset : y - top + e->range_current_offset;
    int index = (int)(offset / element_size);
    if (__builtin_expect(offset < 0, 0)) index--;
    int range_max_val = e->range_max;
    index %= range_max_val;
    if (index < 0) index += range_max_val;

    e->range_index = index;
    e->range_pressed_index = index;
    e->range_finger_down = true;
    int kc = range_keycode(e->range_ordinal, index);
    e->range_initial_kc = kc;
    
    if (kc > 0) e->range_has_binding = true;
}

void element_range_button_move(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)result;
    (void)now_tick;
    if (__builtin_expect(e->range_max <= 0, 0)) return;
    float pos = e->range_orientation == 0 ? x : y;
    float delta = pos - e->range_last_position;

    if (!e->range_scrolling && fabsf(delta) >= g_state.cfg.range_scroll_threshold_px) {
        e->range_scrolling = true;
        e->range_has_binding = false;
        e->range_pressed_index = -1;
    }

    if (e->range_scrolling) {
        float element_size = e->cached_range_element_size;
        float scroll_size = element_size * (float)e->range_max;
        if (scroll_size == 0.0f) return;

        e->range_scroll_offset -= delta;
        if (e->range_scroll_offset >= scroll_size) e->range_scroll_offset -= scroll_size;
        else if (e->range_scroll_offset < 0) e->range_scroll_offset += scroll_size;
        e->range_current_offset = e->range_scroll_offset;
        e->range_last_position = pos;
        mark_element_dirty(e);
    }
}

void element_range_button_up(TouchElement* e, float x, float y, uint64_t now_tick, TouchActionResult* restrict result) {
    (void)x;
    (void)y;

    e->range_finger_down = false;
    e->range_pressed_index = -1;

    if (__builtin_expect(e->range_hold_pressed, 0)) {
        range_handle_hold_release(e, result);
        return;
    }

    if (__builtin_expect(!e->range_has_binding, 0)) {
        // Scroll release: press+release the key at the current scroll position.
        // The initial binding was never pressed during scroll (no ACT_KEY_PRESS at down),
        // so we fire the binding under the finger now, matching picker semantics.
        float element_size = e->cached_range_element_size;
        if (element_size > 0.0f) {
            float cw = e->cached_range_cw;
            float ch = e->cached_range_ch;
            float left = e->x - cw;
            float top  = e->y - ch;
            float offset = e->range_orientation == 0
                ? x - left + e->range_current_offset
                : y - top  + e->range_current_offset;
            int idx = (int)(offset / element_size);
            if (offset < 0) idx--;
            idx %= e->range_max;
            if (idx < 0) idx += e->range_max;
            int kc = range_keycode(e->range_ordinal, idx);
            if (kc > 0) {
                add_action(result, ACT_KEY_PRESS, kc, 1, 0);
                add_action(result, ACT_KEY_RELEASE, kc, 0, 0);
            }
        }
        e->range_scrolling = false;
        return;
    }

    range_handle_tap(e, now_tick, result);
}
