/*
 * T0: the half-resolution grid pattern keeps the source 2x2 phase mapping and
 * the truncated darken alpha.  This test is asset-free and Android-free: it
 * compiles the pure header that the Android presenter also uses.
 */
#include <stdint.h>
#include <stdio.h>

#include "grid_pattern.h"

static int failures;

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #condition);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    CHECK(grid_pattern_alpha(0) == 0);
    CHECK(grid_pattern_alpha(1) == 2);
    CHECK(grid_pattern_alpha(50) == 127);
    CHECK(grid_pattern_alpha(100) == 255);
    CHECK(grid_pattern_alpha(-10) == 0);
    CHECK(grid_pattern_alpha(120) == 255);

    CHECK(grid_pattern_phase(0) == 0);
    CHECK(grid_pattern_phase(3) == 3);
    CHECK(grid_pattern_phase(4) == 0);
    CHECK(grid_pattern_phase(5) == 1);
    CHECK(grid_pattern_phase(-1) == 3);

    for (int phase = 0; phase < GRID_PATTERN_PHASES; phase++) {
        int holes = 0;
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                const int hole = grid_pattern_is_hole(phase, x, y);
                const uint32_t pixel =
                    grid_pattern_pixel(phase, 50, x, y);
                CHECK((pixel == 0u) == (hole != 0));
                CHECK(grid_pattern_pixel(phase, 50, x + 2, y) == pixel);
                CHECK(grid_pattern_pixel(phase, 50, x, y + 2) == pixel);
                if (hole) {
                    holes++;
                    CHECK(pixel == 0u);
                } else {
                    CHECK(pixel == 0x7F000000u);
                }
            }
        }
        CHECK(holes == 1);
        CHECK(grid_pattern_is_hole(phase,
                                   grid_pattern_hole_x(phase),
                                   grid_pattern_hole_y(phase)));
    }

    /* The hole rotates through all four positions once per cycle. */
    CHECK(grid_pattern_hole_x(0) == 0 && grid_pattern_hole_y(0) == 0);
    CHECK(grid_pattern_hole_x(1) == 1 && grid_pattern_hole_y(1) == 0);
    CHECK(grid_pattern_hole_x(2) == 0 && grid_pattern_hole_y(2) == 1);
    CHECK(grid_pattern_hole_x(3) == 1 && grid_pattern_hole_y(3) == 1);

    /* 100 percent darken is the hard grid; zero darken keeps the layer
     * fully transparent. */
    CHECK(grid_pattern_pixel(0, 100, 1, 0) == 0xFF000000u);
    CHECK(grid_pattern_pixel(0, 0, 1, 0) == 0u);
    CHECK(grid_pattern_pixel(0, 0, 0, 0) == 0u);

    if (failures) {
        fprintf(stderr, "grid pattern: %d failure(s)\n", failures);
        return 1;
    }
    puts("grid pattern: ok");
    return 0;
}
