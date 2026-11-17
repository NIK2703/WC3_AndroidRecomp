#ifndef WC3_GRID_PATTERN_H
#define WC3_GRID_PATTERN_H

#include <stdint.h>

/*
 * Half-resolution grid pattern.
 *
 * The pattern is a 2x2 cell anchored at the top-left pixel of the presentation
 * layer: one pixel of every cell stays transparent and the other three are
 * premultiplied black.  The transparent pixel selects the phase:
 *
 *   phase 0 -> (0, 0)
 *   phase 1 -> (1, 0)
 *   phase 2 -> (0, 1)
 *   phase 3 -> (1, 1)
 *
 * `darken_percent` is the 0..100 slider value.  The source feature stores a
 * float and truncates `darken * 255`; an integer percent keeps the same alpha
 * (50 percent -> 127) without a floating point dependency in the render path.
 */

#define GRID_PATTERN_PHASES 4

static inline int grid_pattern_phase(int phase)
{
    phase %= GRID_PATTERN_PHASES;
    return phase < 0 ? phase + GRID_PATTERN_PHASES : phase;
}

static inline int grid_pattern_alpha(int darken_percent)
{
    if (darken_percent <= 0) return 0;
    if (darken_percent >= 100) return 255;
    return (darken_percent * 255) / 100;
}

static inline int grid_pattern_hole_x(int phase)
{
    return grid_pattern_phase(phase) & 1;
}

static inline int grid_pattern_hole_y(int phase)
{
    return (grid_pattern_phase(phase) >> 1) & 1;
}

static inline int grid_pattern_is_hole(int phase, int x, int y)
{
    return (x & 1) == grid_pattern_hole_x(phase) &&
           (y & 1) == grid_pattern_hole_y(phase);
}

/*
 * One pixel as it is written to an RGBA8 Android buffer.  R, G and B are zero
 * for every value, so only the alpha byte in the high byte of the little-endian
 * word changes; the transparent hole is the zero word.
 */
static inline uint32_t grid_pattern_pixel(int phase, int darken_percent,
                                          int x, int y)
{
    if (grid_pattern_is_hole(phase, x, y)) return 0u;
    return (uint32_t)grid_pattern_alpha(darken_percent) << 24;
}

#endif /* WC3_GRID_PATTERN_H */
