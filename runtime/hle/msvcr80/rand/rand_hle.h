/*
 * rand_hle.h - target-side MSVCR80 deterministic pseudo-random generator.
 *
 * The game uses srand/rand as a deterministic key generator, not as a random
 * source.  The guest and the host must see the exact MSVC LCG sequence, so
 * this stays in target code instead of delegating to bionic's rand().
 */
#ifndef WC3_HLE_MSVCR80_RAND_H
#define WC3_HLE_MSVCR80_RAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_msvcr80_rand_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_RAND_H */
