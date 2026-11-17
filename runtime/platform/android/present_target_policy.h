#ifndef WC3_PRESENT_TARGET_POLICY_H
#define WC3_PRESENT_TARGET_POLICY_H

#include <stdint.h>

/*
 * Whether the presentation pass has any work to do.
 *
 * When the guest extent equals the surface extent, present_scale_plan() takes
 * the general branch with target == source and system_scale == 0: a pure 1:1
 * copy that changes no pixel.  The pass can be skipped entirely and DXVK can
 * present into the real window, which removes a full-frame transfer from the
 * default native path.
 *
 * When the extents differ the pass is required: exact half becomes a 2x
 * nearest blit; every other mode is copied 1:1 into a guest-sized canvas so
 * the compositor, not DXVK, does the scaling.
 *
 * This is pure arithmetic.  It is compiled into the product (present_target.c)
 * and into the T0 test, so the bypass decision and the pass policy cannot drift
 * apart silently.
 */
static inline int wc3_present_pass_needed(uint32_t guest_w, uint32_t guest_h,
                                          uint32_t surface_w,
                                          uint32_t surface_h)
{
    if (!guest_w || !guest_h || !surface_w || !surface_h) return 0;
    return guest_w != surface_w || guest_h != surface_h;
}

#endif /* WC3_PRESENT_TARGET_POLICY_H */
