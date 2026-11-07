/*
 * gdi_obj_hle.h - target-side GDI32 region, pen and line state.
 *
 * The game uses rectangular regions for clipping and hit testing, creates pens
 * for its line paths, and tracks the current position through MoveToEx/LineTo.
 * The as-built table did not define any of these rows, so the resolver bound
 * zero-returning stubs.  This slice supplies the GDI object and region
 * contracts; pixel rasterisation remains outside the target renderer.
 */
#ifndef WC3_HLE_GDI32_OBJECTS_H
#define WC3_HLE_GDI32_OBJECTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of rows installed, or -1 when a row is missing. */
int wc3_hle_gdi32_objects_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_GDI32_OBJECTS_H */
