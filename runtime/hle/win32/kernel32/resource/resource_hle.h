/*
 * resource_hle.h - target-side KERNEL32 PE resource lookup.
 *
 * FindResourceA and its load/lock/size/free companions only read the mapped
 * guest image.  The resource directory is part of the PE, so the target can
 * serve the whole group without the legacy resource table.
 */
#ifndef WC3_HLE_KERNEL32_RESOURCE_H
#define WC3_HLE_KERNEL32_RESOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_kernel32_resource_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_KERNEL32_RESOURCE_H */
