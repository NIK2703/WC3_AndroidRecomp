/* winmm_fast_bind.h - product fast binding for as-built WINMM rows.
 *
 * O-91: the rejected v2 sink is debug/headless evidence only.  The product
 * fast registry must not depend on that source, so its row list lives here and
 * is compiled in every variant.  Binding keeps the as-built handler, argc and
 * cdecl_abi; it only fills the dense fast bridge.
 */
#ifndef WC3_HLE_WINMM_FAST_BIND_H
#define WC3_HLE_WINMM_FAST_BIND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows bound (0..11), never negative.  Must run before
 * the first guest thread. */
int wc3_hle_winmm_fast_bind_rows(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WINMM_FAST_BIND_H */
