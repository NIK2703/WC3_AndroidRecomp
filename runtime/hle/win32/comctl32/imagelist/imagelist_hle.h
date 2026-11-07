/*
 * imagelist_hle.h - target-side COMCTL32 image-list handles.
 *
 * The game uses image lists as opaque handles for UI bookkeeping.  The
 * imported surface only creates, counts, replaces, destroys and drags lists;
 * no draw/image extraction entry point is imported, so the target owns the
 * observable handle/count contract without pretending to store pixels.
 */
#ifndef WC3_HLE_COMCTL32_IMAGELIST_H
#define WC3_HLE_COMCTL32_IMAGELIST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 if a row is missing. */
int wc3_hle_comctl32_imagelist_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_COMCTL32_IMAGELIST_H */
