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

#include <stdint.h>

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
 * Phase-1 owner rule: the local player and the three neutral non-hostile
 * owners select; every other owner is an order target.  This is deliberately
 * not an alliance query (see the analysis document, section 4.4).
 */
static inline int wc3_dt_owner_is_friendly(uint16_t owner, uint16_t local)
{
    return owner == local || owner == 12u || owner == 14u || owner == 15u;
}

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
 * Client pixel float to pick-event coordinate.  0x6F8B38A0 multiplies its
 * two stack arguments by 0x6FAA9328 (0.8f) and 0x6FAA9324 (0.6f).  The
 * native input event carries integer client pixels, so truncate first.
 */
static inline float wc3_dt_pick_axis(float client_px, float scale)
{
    return (float)(int)client_px * scale;
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
 * A friendly unit always selects; an enemy/neutral-hostile unit always
 * takes the ghost-order path.  With an empty selection that order is a
 * native no-op, not a request to select an enemy as if it were friendly.
 * selection_nonempty therefore only affects the terrain fallback.
 */
static inline Wc3DtTapIntent wc3_dt_tap_intent(int pick_result,
                                               int validated_object,
                                               int is_unit,
                                               int selection_nonempty,
                                               int order_unit)
{
    if (pick_result == WC3_DT_PICK_OBJECT) {
        if (validated_object) {
            if (!is_unit) return WC3_DT_INTENT_NONE;
            if (order_unit) return WC3_DT_INTENT_ORDER_UNIT;
            return WC3_DT_INTENT_SELECT_UNIT;
        }
        return selection_nonempty ? WC3_DT_INTENT_ORDER_GROUND
                                  : WC3_DT_INTENT_CLEAR_SELECTION;
    }
    if (pick_result == WC3_DT_PICK_TERRAIN)
        return selection_nonempty ? WC3_DT_INTENT_ORDER_GROUND
                                  : WC3_DT_INTENT_CLEAR_SELECTION;
    return WC3_DT_INTENT_NONE;
}

typedef enum {
    WC3_DT_LONG_INTENT_NONE = 0,
    WC3_DT_LONG_INTENT_ORDER_UNIT,
    WC3_DT_LONG_INTENT_ORDER_GROUND
} Wc3DtLongPressIntent;

/* Long press is the right-click analogue: unit target -> ghost order,
 * terrain -> ground smart order. */
static inline Wc3DtLongPressIntent wc3_dt_long_press_intent(int pick_result)
{
    if (pick_result == WC3_DT_PICK_OBJECT)
        return WC3_DT_LONG_INTENT_ORDER_UNIT;
    if (pick_result == WC3_DT_PICK_TERRAIN)
        return WC3_DT_LONG_INTENT_ORDER_GROUND;
    return WC3_DT_LONG_INTENT_NONE;
}

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_DIRECT_TOUCH_INTENT_H */
