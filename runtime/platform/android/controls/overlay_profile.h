#ifndef WC3_OVERLAY_PROFILE_H
#define WC3_OVERLAY_PROFILE_H

#include "overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    Wc3OverlayStyle style;
    Wc3OverlayElement elements[WC3_OVERLAY_MAX_ELEMENTS];
    int element_count;
} Wc3OverlayProfile;

/*
 * Load the subset of the Winlator .icp profile that the detached overlay
 * renders.  This is intentionally a small reader, not a general JSON library
 * and not the Winlator engine loader.  It accepts the Warcraft 3.icp schema and
 * derives the same fallback labels as ControlElement.getDisplayText().
 *
 * Returns 0 on success.  On failure writes a short message to err.
 */
int overlay_profile_load_icp(const char *path, Wc3OverlayProfile *out,
                                 char *err, size_t err_cap);

/* Parse a profile already resident in memory (Android asset, embedded copy). */
int overlay_profile_load_icp_text(const char *data, size_t size,
                                      Wc3OverlayProfile *out, char *err,
                                      size_t err_cap);

/* Copy a loaded profile into an overlay and compute its pixel geometry. */
void overlay_apply_profile(Wc3Overlay *overlay,
                               const Wc3OverlayProfile *profile,
                               int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_PROFILE_H */
