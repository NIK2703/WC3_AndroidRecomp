/*
 * window_facts_hle.h - target-side USER32 window-creation facts.
 *
 * The as-built handlers still own the input queue, but the target records the
 * handle, class, parent and placement they create.  Query APIs then answer from
 * one target table instead of guessing from a constant handle.
 */
#ifndef WC3_HLE_USER32_WINDOW_FACTS_H
#define WC3_HLE_USER32_WINDOW_FACTS_H

#ifdef __cplusplus
extern "C" {
#endif

int wc3_hle_user32_window_facts_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_USER32_WINDOW_FACTS_H */
