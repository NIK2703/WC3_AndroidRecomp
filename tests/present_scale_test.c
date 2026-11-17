/*
 * T0: scaling policy of the presentation pass.
 *
 * The pass itself is Android/Vulkan code, but every decision it takes comes
 * from present_scale.h.  This test is asset-free and Android-free: it compiles
 * that exact header.
 *
 * The two required shapes:
 *
 *   half-resolution grid  -> 2x nearest into a surface-sized image, the only
 *                            case where the pass filters anything;
 *   everything else       -> 1:1 copy into an image of the guest size, the
 *                            compositor scales the layer.  This includes modes
 *                            whose ratio is a whole number (800x360 on
 *                            2400x1080), because our integer scaler exists for
 *                            the grid, not as a general resampler.
 *
 * It also checks the bypass decision: when the guest extent equals the surface
 * extent the plan is a pure 1:1 copy, so the pass is not started at all and
 * DXVK presents straight into the real window.  The decision and the plan are
 * checked together so they cannot drift apart.
 */
#include <stdint.h>
#include <stdio.h>

#include "present_scale.h"
#include "present_target_policy.h"

static int failures;

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #condition);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void check_plan(uint32_t src_w, uint32_t src_h,
                       uint32_t surface_w, uint32_t surface_h, int half_grid,
                       uint32_t target_w, uint32_t target_h,
                       unsigned filter, int system_scale)
{
    const PresentScalePlan plan =
        present_scale_plan(src_w, src_h, surface_w, surface_h, half_grid);

    CHECK(plan.target_w == target_w);
    CHECK(plan.target_h == target_h);
    CHECK(plan.filter == filter);
    CHECK(plan.system_scale == system_scale);

    if (!src_w || !src_h || !surface_w || !surface_h) {
        /* Degenerate input: an empty plan, never a division by zero. */
        CHECK(plan.target_w == 0 && plan.target_h == 0);
        CHECK(plan.filter == PRESENT_SCALE_FILTER_NONE);
        CHECK(!plan.system_scale);
        return;
    }

    /* The source rectangle is always the whole guest image at the origin: the
     * pass reads the guest sub-rectangle of DXVK's image and never assumes
     * that image is exactly guest-sized. */
    CHECK(plan.src_w == src_w && plan.src_h == src_h);

    if (filter == PRESENT_SCALE_FILTER_NEAREST) {
        /* Only the half-resolution grid path filters, and it fills the surface
         * exactly on both axes: no bars, no offsets, no leftovers. */
        CHECK(half_grid);
        CHECK(plan.target_w == surface_w && plan.target_h == surface_h);
        CHECK(src_w * 2u == surface_w && src_h * 2u == surface_h);
        CHECK(plan.system_scale == 0);
    } else {
        /* Copy path: our canvas is the guest image and nobody filters. */
        CHECK(plan.target_w == src_w && plan.target_h == src_h);
        CHECK(plan.system_scale ==
              ((src_w != surface_w || src_h != surface_h) ? 1 : 0));
    }
}

static void check_policy(uint32_t guest_w, uint32_t guest_h,
                         uint32_t surface_w, uint32_t surface_h, int half_grid,
                         int expected_needed)
{
    const int needed =
        wc3_present_pass_needed(guest_w, guest_h, surface_w, surface_h);
    CHECK(needed == expected_needed);

    if (!guest_w || !guest_h || !surface_w || !surface_h)
        return;

    const PresentScalePlan plan =
        present_scale_plan(guest_w, guest_h, surface_w, surface_h, half_grid);

    if (!needed) {
        /* Bypass is only valid when the plan really is a pure 1:1 copy. */
        CHECK(guest_w == surface_w && guest_h == surface_h);
        CHECK(plan.target_w == guest_w && plan.target_h == guest_h);
        CHECK(plan.filter == PRESENT_SCALE_FILTER_NONE);
        CHECK(plan.system_scale == 0);
    } else {
        /* A needed pass must do one of its two jobs. */
        CHECK(plan.filter == PRESENT_SCALE_FILTER_NEAREST ||
              plan.system_scale == 1);
    }
}

int main(void)
{
    /* The half-resolution grid case: 1200x540 on 2400x1080, nearest 2x. */
    check_plan(1200, 540, 2400, 1080, 1, 2400, 1080,
               PRESENT_SCALE_FILTER_NEAREST, 0);

    /* Same numbers without the grid path: no integer scaling, the compositor
     * stretches the 1200x540 layer.  Our scaler exists for the grid only. */
    check_plan(1200, 540, 2400, 1080, 0, 1200, 540,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* A whole ratio that is not the grid path - 800x360 is exactly one third
     * of the surface - still goes to the compositor, by decision. */
    check_plan(800, 360, 2400, 1080, 0, 800, 360,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(800, 360, 2400, 1080, 1, 800, 360,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* Fractional modes: 1600x720 (1.5x) and 2000x900 (1.2x). */
    check_plan(1600, 720, 2400, 1080, 0, 1600, 720,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(1600, 720, 2400, 1080, 1, 1600, 720,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(2000, 900, 2400, 1080, 1, 2000, 900,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* Guest mode equals the surface: a plain copy, the compositor has nothing
     * to do, and the half flag cannot invent a factor. */
    check_plan(2400, 1080, 2400, 1080, 0, 2400, 1080,
               PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(2400, 1080, 2400, 1080, 1, 2400, 1080,
               PRESENT_SCALE_FILTER_NONE, 0);

    /* Portrait surface, same rule. */
    check_plan(1080, 2400, 1080, 2400, 0, 1080, 2400,
               PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(540, 1200, 1080, 2400, 1, 1080, 2400,
               PRESENT_SCALE_FILTER_NEAREST, 0);
    check_plan(540, 1200, 1080, 2400, 0, 540, 1200,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* The guest asks for more pixels than the screen has: no crop and no
     * resample, the compositor downscales.  With half_grid set the guest is by
     * definition not half of the surface, so the same path applies. */
    check_plan(2560, 1440, 2400, 1080, 0, 2560, 1440,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(2560, 1440, 2400, 1080, 1, 2560, 1440,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(2400, 1440, 2400, 1080, 1, 2400, 1440,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* An odd surface cannot have an exact half, so even with half_grid set the
     * plan falls back to the compositor instead of rounding a pixel away. */
    check_plan(1200, 540, 2401, 1081, 1, 1200, 540,
               PRESENT_SCALE_FILTER_NONE, 1);
    check_plan(1200, 540, 2400, 1081, 1, 1200, 540,
               PRESENT_SCALE_FILTER_NONE, 1);

    /* Bypass decision: the pass exists only when it has a job.  The native
     * mode is a pure 1:1 copy with or without the half flag, so it is skipped;
     * every other mode either filters (exact half) or hands a guest-sized layer
     * to the compositor. */
    check_policy(2400, 1080, 2400, 1080, 0, 0);
    check_policy(2400, 1080, 2400, 1080, 1, 0);
    check_policy(1200, 540, 2400, 1080, 1, 1);
    check_policy(1200, 540, 2400, 1080, 0, 1);
    check_policy(800, 360, 2400, 1080, 0, 1);
    check_policy(1600, 720, 2400, 1080, 0, 1);
    check_policy(1080, 2400, 1080, 2400, 0, 0);
    check_policy(540, 1200, 1080, 2400, 1, 1);
    check_policy(2560, 1440, 2400, 1080, 0, 1);
    check_policy(1200, 540, 2401, 1081, 1, 1);
    check_policy(0, 540, 2400, 1080, 1, 0);
    check_policy(1200, 540, 0, 1080, 1, 0);
    check_policy(0, 0, 0, 0, 0, 0);

    /* Degenerate inputs. */
    check_plan(0, 540, 2400, 1080, 1, 0, 0, PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(1200, 0, 2400, 1080, 1, 0, 0, PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(1200, 540, 0, 1080, 1, 0, 0, PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(1200, 540, 2400, 0, 1, 0, 0, PRESENT_SCALE_FILTER_NONE, 0);
    check_plan(0, 0, 0, 0, 0, 0, 0, PRESENT_SCALE_FILTER_NONE, 0);

    /* The device mode table of this screen: heights 360..1080 in steps of 180,
     * widths on the 20:9 ratio of 2400x1080.  Only the exact half of the
     * surface may ever be filtered, and only with the grid path active. */
    for (uint32_t height = 360; height <= 1080; height += 180) {
        const uint32_t width = (height * 20u) / 9u;

        for (int half = 0; half <= 1; half++) {
            const PresentScalePlan plan =
                present_scale_plan(width, height, 2400, 1080, half);
            const int is_half_of_surface = (width * 2u == 2400u && height * 2u == 1080u);
            const int is_native = (width == 2400u && height == 1080u);

            CHECK(wc3_present_pass_needed(width, height, 2400, 1080) ==
                  !is_native);

            if (half && is_half_of_surface) {
                CHECK(plan.filter == PRESENT_SCALE_FILTER_NEAREST);
                CHECK(plan.target_w == 2400 && plan.target_h == 1080);
                CHECK(plan.system_scale == 0);
            } else {
                CHECK(plan.filter == PRESENT_SCALE_FILTER_NONE);
                CHECK(plan.target_w == width && plan.target_h == height);
                CHECK(plan.system_scale ==
                      ((width != 2400u || height != 1080u) ? 1 : 0));
            }
        }
    }

    if (failures) {
        fprintf(stderr, "present scale: %d failure(s)\n", failures);
        return 1;
    }
    puts("present scale: ok");
    return 0;
}
