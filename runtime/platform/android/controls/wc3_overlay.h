#ifndef WC3_OVERLAY_H
#define WC3_OVERLAY_H

/*
 * Detached Warcraft III touch-control overlay renderer.
 *
 * This module is deliberately self-contained: it has no Android, JNI, game or
 * HLE dependency.  It renders a Warcraft 3.icp-style element set into RGBA8
 * memory.  The Android integration will later lock the overlay SurfaceView and
 * pass its buffer here; the existing recompiler build does not reference this
 * file yet.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_OVERLAY_MAX_ELEMENTS 64
#define WC3_OVERLAY_TEXT_MAX 32

/* Cached visual layers, matching the shipped Winlator atlas layers. */
typedef enum {
    WC3_OVERLAY_CACHE_FILL = 0,
    WC3_OVERLAY_CACHE_GLOW = 1,
    WC3_OVERLAY_CACHE_STROKE_TEXT = 2,
    WC3_OVERLAY_CACHE_OUTER_STROKE = 3,
    WC3_OVERLAY_CACHE_DPAD_ACTIVE = 4,
    WC3_OVERLAY_CACHE_DPAD_INACTIVE = 5,
    WC3_OVERLAY_CACHE_SLOT_COUNT = 6
} Wc3OverlayCacheSlot;

#define WC3_OVERLAY_CACHE_MAX_LAYERS \
    (WC3_OVERLAY_MAX_ELEMENTS * WC3_OVERLAY_CACHE_SLOT_COUNT)

typedef struct {
    int element;
    int slot;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
    int dst_x;
    int dst_y;
    int rotation; /* 0, 90, 180 or 270 degrees, clockwise. */
} Wc3OverlayCacheLayer;

/*
 * One RGBA8 atlas and the layer rectangles cut from it.  Pixels are straight
 * (non-premultiplied) alpha.  The overlay owns `pixels`; set_atlas() takes
 * ownership and cache_clear()/destroy() free it.
 */
typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    int stride_bytes;
    int element_count;
    int layer_count;
    /* Overlay geometry this atlas was built for.  A cache view is only
     * usable while the consuming overlay has the same dimensions. */
    int overlay_width;
    int overlay_height;
    Wc3OverlayCacheLayer layers[WC3_OVERLAY_CACHE_MAX_LAYERS];
} Wc3OverlayCache;

typedef enum {
    WC3_OVERLAY_ELEMENT_BUTTON = 0,
    WC3_OVERLAY_ELEMENT_DPAD = 1
} Wc3OverlayElementType;

typedef enum {
    WC3_OVERLAY_SHAPE_RECT = 0,
    WC3_OVERLAY_SHAPE_CIRCLE = 1
} Wc3OverlayShape;

/*
 * Pixel format written by wc3_overlay_render().  Android SurfaceFlinger
 * consumes premultiplied RGBA, while the detached preview/tests use straight
 * alpha.  The default (zero) is straight so old callers keep their format.
 */
typedef enum {
    WC3_OVERLAY_ALPHA_STRAIGHT = 0,
    WC3_OVERLAY_ALPHA_PREMULTIPLIED = 1
} Wc3OverlayAlphaMode;

/* Visual layers mirror VISUAL_LAYER_* in the Winlator C engine. */
#define WC3_OVERLAY_LAYER_FILL         (1u << 0)
#define WC3_OVERLAY_LAYER_STROKE_TEXT  (1u << 1)
#define WC3_OVERLAY_LAYER_GLOW         (1u << 2)
#define WC3_OVERLAY_LAYER_OUTER_STROKE (1u << 3)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} Wc3OverlayColor;

typedef struct {
    Wc3OverlayColor primary;        /* default: opaque white */
    Wc3OverlayColor secondary;      /* default: opaque rgb(2,119,189) */
    float overlay_opacity;          /* default: 0.4 */
    float stroke_width_multiplier;  /* default: 0.15, matching ElementRenderer */
    float corner_radius;            /* default: 0.8 */
    int fill_alpha_inactive;        /* default: 0 for this profile */
} Wc3OverlayStyle;

typedef struct {
    Wc3OverlayElementType type;
    Wc3OverlayShape shape;

    /* Normalised profile position in 0..1. */
    float nx;
    float ny;

    /* Profile dimensions in snapping units (RECT buttons only). */
    float width_units;
    float height_units;

    float scale;
    float corner_radius;       /* < 0: use style.corner_radius */
    float dpad_corner_radius;  /* < 0: use effective corner radius */

    /*
     * Optional per-element opacity.  has_opacity is kept separate from the
     * value so a zeroed element keeps the profile/global overlay opacity.
     */
    bool has_opacity;
    float opacity;             /* 0..1 when has_opacity is true */

    char text[WC3_OVERLAY_TEXT_MAX];

    /*
     * Visual state.  visual_layers is a WC3_OVERLAY_LAYER_* mask.  A BUTTON
     * with visual_layers == 0 is inactive and draws its inactive fill (if any)
     * plus stroke and text.  The layer alphas are 0..255 multipliers; 255 is
     * the normal fully-applied layer.
     */
    uint8_t visual_layers;
    uint8_t alpha_fill;
    uint8_t alpha_glow;
    uint8_t alpha_stroke;
    uint8_t alpha_outer;

    /* D-pad only: bit 0 up, 1 right, 2 down, 3 left. */
    bool engaged;
    uint8_t active_petals;
    bool selected;

    /* Derived by wc3_overlay_commit(). */
    int x;
    int y;
    float hw;
    float hh;
    float left;
    float top;
    float right;
    float bottom;
} Wc3OverlayElement;

typedef struct {
    Wc3OverlayStyle style;
    Wc3OverlayElement elements[WC3_OVERLAY_MAX_ELEMENTS];
    int element_count;
    int width;
    int height;
    float snapping_size;
    Wc3OverlayCache cache;
    /*
     * Optional borrowed atlas.  The game host uses this to share one Canvas
     * atlas across its visual snapshots.  The pointed-to cache remains owned
     * by its producer; init/resize/destroy never free it.
     */
    const Wc3OverlayCache *cache_view;
    Wc3OverlayAlphaMode alpha_mode;
} Wc3Overlay;

void wc3_overlay_style_defaults(Wc3OverlayStyle *style);

/* The caller must zero-initialise the overlay before the first init. */
void wc3_overlay_init(Wc3Overlay *overlay, int width, int height,
                      const Wc3OverlayStyle *style);

void wc3_overlay_resize(Wc3Overlay *overlay, int width, int height);

void wc3_overlay_clear_elements(Wc3Overlay *overlay);

int wc3_overlay_add_element(Wc3Overlay *overlay,
                            const Wc3OverlayElement *element);

/* Recompute snapping size, pixel centres and half-extents. */
void wc3_overlay_commit(Wc3Overlay *overlay);

void wc3_overlay_set_layers(Wc3Overlay *overlay, int index, uint8_t layers);
void wc3_overlay_set_engaged(Wc3Overlay *overlay, int index, bool engaged);
void wc3_overlay_set_active_petals(Wc3Overlay *overlay, int index,
                                   uint8_t petal_mask);
void wc3_overlay_clear_active(Wc3Overlay *overlay);

/*
 * Optional cached-layer atlas.  Build it after populate/resize, not from
 * visual-state setters.  The overlay takes ownership of the pixel buffer.
 * Calls are single-threaded: Wc3Overlay is not internally locked.
 */
int wc3_overlay_cache_set_atlas(Wc3Overlay *overlay, uint8_t *pixels,
                                int width, int height, int stride_bytes,
                                int element_count);
int wc3_overlay_cache_add_layer(Wc3Overlay *overlay, int element, int slot,
                                int src_x, int src_y, int src_w, int src_h,
                                int dst_x, int dst_y, int rotation);
void wc3_overlay_cache_clear(Wc3Overlay *overlay);
void wc3_overlay_destroy(Wc3Overlay *overlay);

/*
 * Render into an RGBA8 buffer.  The whole buffer is cleared to transparent
 * first.  stride_bytes is the row pitch in bytes and must be >= width * 4.
 * The caller owns the buffer.
 */
void wc3_overlay_render(Wc3Overlay *overlay, uint8_t *rgba, int stride_bytes);

#ifdef __cplusplus
}
#endif

#endif /* WC3_OVERLAY_H */

