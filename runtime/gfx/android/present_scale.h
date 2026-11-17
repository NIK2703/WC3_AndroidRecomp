#ifndef WC3_GFX_ANDROID_PRESENT_SCALE_H
#define WC3_GFX_ANDROID_PRESENT_SCALE_H

#include <stdint.h>

/*
 * Scaling policy of the presentation pass.
 *
 * The pass moves the guest image from DXVK's swap chain image to a swap chain
 * image of the real SurfaceView window.  There are exactly two shapes, and the
 * choice between them is not "is the ratio whole" but "is this the
 * half-resolution grid path":
 *
 *   half-resolution grid            the game renders at half the surface and
 *   (present_scale = half, and      the pass upscales it 2x with
 *    the guest is exactly half)     VK_FILTER_NEAREST.  This is the only place
 *                                   where the pass filters, and the reason is
 *                                   the grid: the mask is drawn per doubled
 *                                   cell, so every cell must be an exact 2x2
 *                                   block of the guest image.  Destination
 *                                   pixel i takes source texel floor(i / 2)
 *                                   under nearest sampling, which is exactly
 *                                   that block - no shader, no intermediate
 *                                   colour, one vkCmdBlitImage.
 *
 *   everything else                 the pass does not scale at all.  Its swap
 *                                   chain image is created at the guest extent,
 *                                   the guest image is copied 1:1 into it, and
 *                                   the Android compositor stretches the layer
 *                                   to the view.  This holds for any mode that
 *                                   is not the half-resolution grid, *including*
 *                                   modes whose ratio happens to be a whole
 *                                   number: 800x360 on 2400x1080 is handed to
 *                                   the compositor, not upscaled 3x by us.
 *                                   The system scaler is the general-case
 *                                   answer; ours exists for the grid.
 *
 * Everything here is pure arithmetic: no Vulkan, no Android, no allocation, so
 * the same header is compiled into the product and into the T0 test.
 */

typedef enum PresentScaleFilter {
    /* The pass copies the source rectangle, no sampling at all. */
    PRESENT_SCALE_FILTER_NONE = 0,
    /* The pass upscales by 2 with VK_FILTER_NEAREST. */
    PRESENT_SCALE_FILTER_NEAREST = 1
} PresentScaleFilter;

typedef struct PresentScalePlan {
    /* Extent of the swap chain image the pass writes: the surface extent for
     * the grid path, the guest extent for the compositor path. */
    uint32_t target_w;
    uint32_t target_h;

    /* Source rectangle inside DXVK's image.  Always the whole guest image at
     * the origin: the pass never assumes the producer image is exactly
     * guest-sized, it reads the guest sub-rectangle. */
    uint32_t src_w;
    uint32_t src_h;

    unsigned filter;
    int system_scale;
} PresentScalePlan;

/*
 * Decide the whole frame.
 *
 * `surface_w`/`surface_h` are the real window size, i.e. what the user sees.
 * `half_grid` says whether the half-resolution grid path is active; it comes
 * from the launch setting, not from the ratio.
 */
static inline PresentScalePlan present_scale_plan(uint32_t src_w, uint32_t src_h,
                                                 uint32_t surface_w,
                                                 uint32_t surface_h,
                                                 int half_grid)
{
    PresentScalePlan plan;

    plan.target_w = 0;
    plan.target_h = 0;
    plan.src_w = 0;
    plan.src_h = 0;
    plan.filter = PRESENT_SCALE_FILTER_NONE;
    plan.system_scale = 0;

    if (!src_w || !src_h || !surface_w || !surface_h)
        return plan;

    plan.src_w = src_w;
    plan.src_h = src_h;

    /* Half-resolution grid: the guest occupies exactly half of the surface on
     * both axes, which is also the only configuration the grid mask is drawn
     * for.  An odd surface cannot have an exact half, so it can never land
     * here and no rounding rule is needed. */
    if (half_grid && (surface_w & 1u) == 0u && (surface_h & 1u) == 0u &&
        src_w == surface_w / 2u && src_h == surface_h / 2u) {
        plan.target_w = surface_w;
        plan.target_h = surface_h;
        plan.filter = PRESENT_SCALE_FILTER_NEAREST;
        plan.system_scale = 0;
        return plan;
    }

    /* General case: 1:1 canvas at the game's own resolution.  Whatever the
     * compositor does with it is its business; the pass only guarantees that
     * the pixels it hands over are the guest pixels, unscaled and unfiltered. */
    plan.target_w = src_w;
    plan.target_h = src_h;
    plan.filter = PRESENT_SCALE_FILTER_NONE;
    plan.system_scale = (src_w != surface_w || src_h != surface_h) ? 1 : 0;
    return plan;
}

#endif /* WC3_GFX_ANDROID_PRESENT_SCALE_H */
