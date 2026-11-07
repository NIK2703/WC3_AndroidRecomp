/*
 * string_hle.h - target-side kernel32 narrow-string subset.
 *
 * Only the two string primitives that do not depend on the locale layer are
 * moved here.  MultiByteToWideChar, LCMapString and the character-type APIs
 * remain in the quarantine until Wc3LocaleApi owns their encoding decisions.
 */
#ifndef WC3_HLE_KERNEL32_STRING_H
#define WC3_HLE_KERNEL32_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_kernel32_string_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_KERNEL32_STRING_H */
