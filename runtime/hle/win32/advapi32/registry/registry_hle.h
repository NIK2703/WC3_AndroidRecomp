/*
 * registry_hle.h - target-side ADVAPI32 registry store.
 *
 * The as-built registry is a flat in-memory store with only open/query/set/
 * create/close.  The game also imports delete, flush, enumeration and key
 * information queries, which the resolver bound to zero-returning stubs.
 * This slice replaces the whole registry surface with one target store so
 * those operations see the same keys and values.
 */
#ifndef WC3_HLE_ADVAPI32_REGISTRY_H
#define WC3_HLE_ADVAPI32_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 when a row is missing. */
int wc3_hle_advapi32_registry_install(void);

/* Bracket a group of related writes so the persistent store is rewritten
 * once at the end instead of once per value. */
void wc3_hle_registry_begin_batch(void);
void wc3_hle_registry_end_batch(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_ADVAPI32_REGISTRY_H */
