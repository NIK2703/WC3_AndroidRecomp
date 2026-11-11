#include "overlay.h"

#include <stdlib.h>
#include <string.h>

#define WC3_OVERLAY_DPAD_RADIUS_MULT 7.0f
#define WC3_OVERLAY_CIRCLE_RADIUS_MULT 3.0f
#define WC3_OVERLAY_STROKE_MULT_DEFAULT 0.15f
#define WC3_OVERLAY_CORNER_RADIUS_DEFAULT 0.8f
#define WC3_OVERLAY_OVERLAY_OPACITY_DEFAULT 0.4f

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t mul_u8(uint8_t a, uint8_t b) {
    return (uint8_t)(((unsigned)a * (unsigned)b + 127u) / 255u);
}

static uint8_t effective_element_alpha(const Wc3Overlay *o,
                                       const Wc3OverlayElement *e) {
    float opacity = o->style.overlay_opacity;
    if (e && e->has_opacity) opacity = e->opacity;
    opacity = clampf(opacity, 0.0f, 1.0f);
    /* ElementRenderer.getEffectiveAlphaInt() truncates here. */
    return (uint8_t)(opacity * 255.0f);
}

static void blend_rgba_straight(uint8_t *dst, Wc3OverlayColor color,
                                uint8_t alpha) {
    if (alpha == 0) return;

    int sa = alpha;
    int da = dst[3];
    int da2 = (da * (255 - sa) + 127) / 255;
    int outa = sa + da2;
    if (outa <= 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }

    int r = ((int)color.r * sa + (int)dst[0] * da2 + outa / 2) / outa;
    int g = ((int)color.g * sa + (int)dst[1] * da2 + outa / 2) / outa;
    int b = ((int)color.b * sa + (int)dst[2] * da2 + outa / 2) / outa;

    dst[0] = (uint8_t)(r > 255 ? 255 : r);
    dst[1] = (uint8_t)(g > 255 ? 255 : g);
    dst[2] = (uint8_t)(b > 255 ? 255 : b);
    dst[3] = (uint8_t)(outa > 255 ? 255 : outa);
}

/* SurfaceFlinger expects premultiplied RGBA.  Keep this in the layer loop:
 * the destination starts at zero, so each source-over pass can add its
 * premultiplied color directly without a separate full-screen pass. */
static void blend_rgba_premultiplied(uint8_t *dst, Wc3OverlayColor color,
                                     uint8_t alpha) {
    if (alpha == 0) return;

    int inv = 255 - alpha;
    int r = ((int)color.r * alpha + 127) / 255;
    int g = ((int)color.g * alpha + 127) / 255;
    int b = ((int)color.b * alpha + 127) / 255;
    r += ((int)dst[0] * inv + 127) / 255;
    g += ((int)dst[1] * inv + 127) / 255;
    b += ((int)dst[2] * inv + 127) / 255;
    int a = alpha + ((int)dst[3] * inv + 127) / 255;

    dst[0] = (uint8_t)(r > 255 ? 255 : r);
    dst[1] = (uint8_t)(g > 255 ? 255 : g);
    dst[2] = (uint8_t)(b > 255 ? 255 : b);
    dst[3] = (uint8_t)(a > 255 ? 255 : a);
}

static void blend_rgba(uint8_t *dst, Wc3OverlayColor color, uint8_t alpha,
                       Wc3OverlayAlphaMode mode) {
    if (mode == WC3_OVERLAY_ALPHA_PREMULTIPLIED)
        blend_rgba_premultiplied(dst, color, alpha);
    else
        blend_rgba_straight(dst, color, alpha);
}

/* ------------------------------------------------------------------------- */
/* Cached-layer atlas.                                                       */
/* ------------------------------------------------------------------------- */

static const Wc3OverlayCache *cache_source(const Wc3Overlay *o) {
    if (!o) return NULL;
    return o->cache_view ? o->cache_view : &o->cache;
}

static bool cache_ready(const Wc3Overlay *o) {
    const Wc3OverlayCache *cache = cache_source(o);
    return cache && cache->pixels && cache->width > 0 &&
           cache->height > 0 && cache->stride_bytes >= cache->width * 4 &&
           cache->element_count == o->element_count &&
           cache->overlay_width == o->width &&
           cache->overlay_height == o->height;
}

static const Wc3OverlayCacheLayer *cache_find_layer(const Wc3Overlay *o,
                                                    int element, int slot) {
    if (!cache_ready(o)) return NULL;
    if (element < 0 || element >= WC3_OVERLAY_MAX_ELEMENTS || slot < 0 ||
        slot >= WC3_OVERLAY_CACHE_SLOT_COUNT)
        return NULL;
    const Wc3OverlayCache *cache = cache_source(o);
    int index = cache->layer_index[element][slot];
    if (index >= 0 && index < cache->layer_count) {
        const Wc3OverlayCacheLayer *l = &cache->layers[index];
        if (l->element == element && l->slot == slot) return l;
    }
    /* Caches built by older callers may not have layer_index populated. */
    for (int i = 0; i < cache->layer_count; i++) {
        const Wc3OverlayCacheLayer *l = &cache->layers[i];
        if (l->element == element && l->slot == slot) return l;
    }
    return NULL;
}

static bool cache_layer_rotated_source(int x, int y, int w, int h,
                                       int rotation, int *sx, int *sy) {
    switch (rotation) {
        case 0:
            *sx = x;
            *sy = y;
            return true;
        case 90:
            *sx = y;
            *sy = h - 1 - x;
            return true;
        case 180:
            *sx = w - 1 - x;
            *sy = h - 1 - y;
            return true;
        case 270:
            *sx = w - 1 - y;
            *sy = x;
            return true;
        default:
            return false;
    }
}

static void cache_blit(Wc3Overlay *o, uint8_t *rgba, int stride_bytes,
                       const Wc3OverlayCacheLayer *layer, int rotation,
                       uint8_t alpha) {
    if (!layer || alpha == 0) return;
    const Wc3OverlayCache *cache = cache_source(o);
    if (!cache) return;
    int w = layer->src_w;
    int h = layer->src_h;
    if (w <= 0 || h <= 0) return;
    if (rotation != 0 && w != h) return;
    rotation %= 360;
    if (rotation < 0) rotation += 360;
    if (rotation != 0 && rotation != 90 && rotation != 180 &&
        rotation != 270)
        return;

    int x0 = layer->dst_x;
    int y0 = layer->dst_y;
    int x1 = x0 + w;
    int y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width) x1 = o->width;
    if (y1 > o->height) y1 = o->height;
    if (x0 >= x1 || y0 >= y1) return;

    for (int dy = y0; dy < y1; dy++) {
        int ly = dy - layer->dst_y;
        for (int dx = x0; dx < x1; dx++) {
            int lx = dx - layer->dst_x;
            int sx, sy;
            if (!cache_layer_rotated_source(lx, ly, w, h, rotation, &sx,
                                            &sy))
                continue;
            int ax = layer->src_x + sx;
            int ay = layer->src_y + sy;
            if (ax < 0 || ay < 0 || ax >= cache->width ||
                ay >= cache->height)
                continue;
            const uint8_t *src = cache->pixels +
                                 (size_t)ay *
                                     (size_t)cache->stride_bytes +
                                 (size_t)ax * 4u;
            uint8_t sa = mul_u8(src[3], alpha);
            if (sa == 0) continue;
            Wc3OverlayColor color = {src[0], src[1], src[2], 255};
            blend_rgba(rgba + (size_t)dy * (size_t)stride_bytes +
                           (size_t)dx * 4u,
                       color, sa, o->alpha_mode);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Element compositing from cached layers.                                  */
/* ------------------------------------------------------------------------- */

static bool draw_button_cached(Wc3Overlay *o, uint8_t *rgba, int stride,
                               const Wc3OverlayElement *e, int index) {
    if (!cache_ready(o)) return false;

    const Wc3OverlayCacheLayer *fill =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_FILL);
    const Wc3OverlayCacheLayer *stroke =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_STROKE_TEXT);
    const Wc3OverlayCacheLayer *glow =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_GLOW);
    const Wc3OverlayCacheLayer *outer =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_OUTER_STROKE);
    if (!stroke) return false;

    uint8_t layers = e->visual_layers;
    uint8_t base_alpha = effective_element_alpha(o, e);
    uint8_t fill_alpha = 0;
    if (layers & WC3_OVERLAY_LAYER_FILL)
        fill_alpha = mul_u8(base_alpha, e->alpha_fill);
    else
        fill_alpha = mul_u8(base_alpha,
                            (uint8_t)(o->style.fill_alpha_inactive < 0
                                          ? 0
                                          : (o->style.fill_alpha_inactive >
                                                     255
                                                 ? 255
                                                 : o->style.fill_alpha_inactive)));
    if (fill_alpha > 0 && !fill) return false;

    bool draw_stroke = false;
    if (layers == 0) {
        draw_stroke = true;
    } else if (layers & WC3_OVERLAY_LAYER_STROKE_TEXT) {
        draw_stroke = (layers & WC3_OVERLAY_LAYER_GLOW) ||
                      !(layers & WC3_OVERLAY_LAYER_FILL);
    }
    if (draw_stroke && !stroke) return false;

    if ((layers & WC3_OVERLAY_LAYER_GLOW) &&
        mul_u8(base_alpha, e->alpha_glow) > 0 && !glow)
        return false;
    if ((layers & WC3_OVERLAY_LAYER_OUTER_STROKE) &&
        mul_u8(base_alpha, e->alpha_outer) > 0 && !outer)
        return false;

    /* Shipped compositor order: fill, stroke/text, glow, outer stroke. */
    if (fill_alpha > 0) cache_blit(o, rgba, stride, fill, 0, fill_alpha);
    if (draw_stroke)
        cache_blit(o, rgba, stride, stroke, 0, base_alpha);
    if (layers & WC3_OVERLAY_LAYER_GLOW)
        cache_blit(o, rgba, stride, glow, 0,
                   mul_u8(base_alpha, e->alpha_glow));
    if (layers & WC3_OVERLAY_LAYER_OUTER_STROKE)
        cache_blit(o, rgba, stride, outer, 0,
                   mul_u8(base_alpha, e->alpha_outer));
    return true;
}

static bool draw_dpad_cached(Wc3Overlay *o, uint8_t *rgba, int stride,
                             const Wc3OverlayElement *e, int index) {
    if (!cache_ready(o)) return false;

    const Wc3OverlayCacheLayer *active =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_DPAD_ACTIVE);
    const Wc3OverlayCacheLayer *inactive =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_DPAD_INACTIVE);
    const Wc3OverlayCacheLayer *fill =
        cache_find_layer(o, index, WC3_OVERLAY_CACHE_FILL);
    if (!inactive) return false;

    uint8_t base_alpha = effective_element_alpha(o, e);
    uint8_t inactive_fill = mul_u8(base_alpha,
                                   (uint8_t)(o->style.fill_alpha_inactive < 0
                                                 ? 0
                                                 : (o->style.fill_alpha_inactive >
                                                            255
                                                        ? 255
                                                        : o->style.fill_alpha_inactive)));
    if (inactive_fill > 0 && !fill) return false;

    for (int p = 0; p < 4; p++) {
        bool is_active = e->engaged && (e->active_petals & (1u << p));
        int rotation = p * 90;
        if (is_active) {
            if (!active) return false;
            cache_blit(o, rgba, stride, active,
                       active->rotation + rotation, base_alpha);
        } else {
            if (inactive_fill > 0)
                cache_blit(o, rgba, stride, fill,
                           fill->rotation + rotation, inactive_fill);
            cache_blit(o, rgba, stride, inactive,
                       inactive->rotation + rotation, base_alpha);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API.                                                               */
/* ------------------------------------------------------------------------- */

void overlay_style_defaults(Wc3OverlayStyle *style) {
    if (!style) return;
    memset(style, 0, sizeof(*style));
    style->primary = (Wc3OverlayColor){255, 255, 255, 255};
    style->secondary = (Wc3OverlayColor){2, 119, 189, 255};
    style->overlay_opacity = WC3_OVERLAY_OVERLAY_OPACITY_DEFAULT;
    style->stroke_width_multiplier = WC3_OVERLAY_STROKE_MULT_DEFAULT;
    style->corner_radius = WC3_OVERLAY_CORNER_RADIUS_DEFAULT;
    style->fill_alpha_inactive = 0;
}

void overlay_cache_clear(Wc3Overlay *overlay) {
    if (!overlay) return;
    free(overlay->cache.pixels);
    memset(&overlay->cache, 0, sizeof(overlay->cache));
}

int overlay_cache_set_atlas(Wc3Overlay *overlay, uint8_t *pixels,
                                int width, int height, int stride_bytes,
                                int element_count) {
    if (!overlay || !pixels || width <= 0 || height <= 0 ||
        stride_bytes < width * 4 || element_count < 0)
        return -1;
    overlay_cache_clear(overlay);
    overlay->cache.pixels = pixels;
    overlay->cache.width = width;
    overlay->cache.height = height;
    overlay->cache.stride_bytes = stride_bytes;
    overlay->cache.element_count = element_count;
    overlay->cache.overlay_width = overlay->width;
    overlay->cache.overlay_height = overlay->height;
    for (int e = 0; e < WC3_OVERLAY_MAX_ELEMENTS; e++)
        for (int s = 0; s < WC3_OVERLAY_CACHE_SLOT_COUNT; s++)
            overlay->cache.layer_index[e][s] = -1;
    return 0;
}

int overlay_cache_add_layer(Wc3Overlay *overlay, int element, int slot,
                                int src_x, int src_y, int src_w, int src_h,
                                int dst_x, int dst_y, int rotation) {
    if (!overlay || !overlay->cache.pixels || element < 0 ||
        element >= overlay->cache.element_count ||
        element >= overlay->element_count || slot < 0 ||
        slot >= WC3_OVERLAY_CACHE_SLOT_COUNT ||
        overlay->cache.layer_count >= WC3_OVERLAY_CACHE_MAX_LAYERS)
        return -1;
    if (src_w <= 0 || src_h <= 0 || src_x < 0 || src_y < 0 ||
        src_x + src_w > overlay->cache.width ||
        src_y + src_h > overlay->cache.height)
        return -1;
    if (rotation != 0 && rotation != 90 && rotation != 180 &&
        rotation != 270)
        return -1;
    if (rotation != 0 && src_w != src_h) return -1;

    int layer_index = overlay->cache.layer_count;
    Wc3OverlayCacheLayer *layer = &overlay->cache.layers[layer_index];
    overlay->cache.layer_count = layer_index + 1;
    layer->element = element;
    layer->slot = slot;
    layer->src_x = src_x;
    layer->src_y = src_y;
    layer->src_w = src_w;
    layer->src_h = src_h;
    layer->dst_x = dst_x;
    layer->dst_y = dst_y;
    layer->rotation = rotation;
    if (overlay->cache.layer_index[element][slot] < 0)
        overlay->cache.layer_index[element][slot] = (int16_t)layer_index;
    return 0;
}

void overlay_destroy(Wc3Overlay *overlay) {
    overlay_cache_clear(overlay);
}

void overlay_init(Wc3Overlay *overlay, int width, int height,
                      const Wc3OverlayStyle *style) {
    if (!overlay) return;
    overlay_cache_clear(overlay);
    memset(overlay, 0, sizeof(*overlay));
    if (style) overlay->style = *style;
    else overlay_style_defaults(&overlay->style);
    overlay->width = width;
    overlay->height = height;
    overlay_commit(overlay);
}

void overlay_resize(Wc3Overlay *overlay, int width, int height) {
    if (!overlay) return;
    overlay->width = width;
    overlay->height = height;
    overlay_commit(overlay);
}

void overlay_clear_elements(Wc3Overlay *overlay) {
    if (!overlay) return;
    overlay_cache_clear(overlay);
    overlay->element_count = 0;
    memset(overlay->elements, 0, sizeof(overlay->elements));
}

int overlay_add_element(Wc3Overlay *overlay,
                            const Wc3OverlayElement *element) {
    if (!overlay || !element) return -1;
    if (overlay->element_count >= WC3_OVERLAY_MAX_ELEMENTS) return -1;

    Wc3OverlayElement copy = *element;
    copy.text[WC3_OVERLAY_TEXT_MAX - 1] = '\0';
    if (copy.scale <= 0.0f) copy.scale = 1.0f;
    if (copy.width_units <= 0.0f) copy.width_units = 8.0f;
    if (copy.height_units <= 0.0f) copy.height_units = 4.0f;
    if (copy.alpha_fill == 0) copy.alpha_fill = 255;
    if (copy.alpha_glow == 0) copy.alpha_glow = 255;
    if (copy.alpha_stroke == 0) copy.alpha_stroke = 255;
    if (copy.alpha_outer == 0) copy.alpha_outer = 255;
    if (copy.type == WC3_OVERLAY_ELEMENT_DPAD && copy.visual_layers == 0)
        copy.visual_layers = WC3_OVERLAY_LAYER_FILL |
                             WC3_OVERLAY_LAYER_STROKE_TEXT;

    int index = overlay->element_count++;
    overlay->elements[index] = copy;
    overlay_commit(overlay);
    return index;
}

void overlay_commit(Wc3Overlay *overlay) {
    if (!overlay) return;
    /* Geometry changed: cached layer rectangles no longer match. */
    overlay_cache_clear(overlay);
    int snap = overlay->width / 100;
    if (snap <= 0) snap = 10;
    overlay->snapping_size = (float)snap;

    for (int i = 0; i < overlay->element_count; i++) {
        Wc3OverlayElement *e = &overlay->elements[i];
        e->x = (int)(e->nx * (float)overlay->width + 0.5f);
        e->y = (int)(e->ny * (float)overlay->height + 0.5f);

        if (e->type == WC3_OVERLAY_ELEMENT_DPAD) {
            e->hw = overlay->snapping_size * WC3_OVERLAY_DPAD_RADIUS_MULT *
                    e->scale;
            e->hh = e->hw;
        } else if (e->shape == WC3_OVERLAY_SHAPE_CIRCLE) {
            e->hw = overlay->snapping_size * WC3_OVERLAY_CIRCLE_RADIUS_MULT *
                    e->scale;
            e->hh = e->hw;
        } else {
            e->hw = overlay->snapping_size * e->width_units * 0.5f * e->scale;
            e->hh = overlay->snapping_size * e->height_units * 0.5f * e->scale;
        }

        e->left = (float)e->x - e->hw;
        e->right = (float)e->x + e->hw;
        e->top = (float)e->y - e->hh;
        e->bottom = (float)e->y + e->hh;
    }
}

void overlay_set_layers(Wc3Overlay *overlay, int index, uint8_t layers) {
    if (!overlay || index < 0 || index >= overlay->element_count) return;
    overlay->elements[index].visual_layers = layers;
}

void overlay_set_engaged(Wc3Overlay *overlay, int index, bool engaged) {
    if (!overlay || index < 0 || index >= overlay->element_count) return;
    overlay->elements[index].engaged = engaged;
}

void overlay_set_active_petals(Wc3Overlay *overlay, int index,
                                   uint8_t petal_mask) {
    if (!overlay || index < 0 || index >= overlay->element_count) return;
    overlay->elements[index].active_petals = petal_mask & 0x0F;
}

void overlay_clear_active(Wc3Overlay *overlay) {
    if (!overlay) return;
    for (int i = 0; i < overlay->element_count; i++) {
        overlay->elements[i].visual_layers = 0;
        overlay->elements[i].engaged = false;
        overlay->elements[i].active_petals = 0;
        if (overlay->elements[i].type == WC3_OVERLAY_ELEMENT_DPAD)
            overlay->elements[i].visual_layers = WC3_OVERLAY_LAYER_FILL |
                                                 WC3_OVERLAY_LAYER_STROKE_TEXT;
    }
}

static bool element_visual_state_equal(const Wc3OverlayElement *shown,
                                       const Wc3OverlayElement *candidate) {
    if (shown->type != candidate->type) return false;
    if (shown->has_opacity != candidate->has_opacity) return false;
    if (shown->has_opacity && shown->opacity != candidate->opacity)
        return false;

    if (shown->type == WC3_OVERLAY_ELEMENT_DPAD) {
        /* draw_dpad_cached() only reads the engaged mask and the petals; the
         * layer mask and per-layer alphas belong to the button path. */
        return shown->engaged == candidate->engaged &&
               shown->active_petals == candidate->active_petals;
    }

    /*
     * Reduce the raw mask to the draw calls draw_button_cached() actually
     * makes.  For example, STROKE_TEXT is redundant with FILL and a zero mask
     * already draws the stroke; those differences must not wake the render
     * thread.
     */
    const uint8_t a = shown->visual_layers;
    const uint8_t b = candidate->visual_layers;
    const bool a_fill = (a & WC3_OVERLAY_LAYER_FILL) != 0;
    const bool b_fill = (b & WC3_OVERLAY_LAYER_FILL) != 0;
    const bool a_glow = (a & WC3_OVERLAY_LAYER_GLOW) != 0;
    const bool b_glow = (b & WC3_OVERLAY_LAYER_GLOW) != 0;
    const bool a_outer = (a & WC3_OVERLAY_LAYER_OUTER_STROKE) != 0;
    const bool b_outer = (b & WC3_OVERLAY_LAYER_OUTER_STROKE) != 0;
    const bool a_stroke =
        a == 0 || ((a & WC3_OVERLAY_LAYER_STROKE_TEXT) &&
                   (a_glow || !a_fill));
    const bool b_stroke =
        b == 0 || ((b & WC3_OVERLAY_LAYER_STROKE_TEXT) &&
                   (b_glow || !b_fill));

    if (a_fill != b_fill || a_stroke != b_stroke || a_glow != b_glow ||
        a_outer != b_outer)
        return false;
    if (a_fill && shown->alpha_fill != candidate->alpha_fill) return false;
    if (a_glow && shown->alpha_glow != candidate->alpha_glow) return false;
    if (a_outer && shown->alpha_outer != candidate->alpha_outer) return false;
    return true;
}

bool overlay_visual_state_equal(const Wc3Overlay *shown,
                                const Wc3Overlay *candidate) {
    if (shown == candidate) return true;
    if (!shown || !candidate) return false;
    if (shown->element_count < 0 ||
        shown->element_count > WC3_OVERLAY_MAX_ELEMENTS)
        return false;

    if (shown->width != candidate->width ||
        shown->height != candidate->height ||
        shown->alpha_mode != candidate->alpha_mode ||
        shown->element_count != candidate->element_count ||
        shown->style.overlay_opacity != candidate->style.overlay_opacity ||
        shown->style.fill_alpha_inactive !=
            candidate->style.fill_alpha_inactive)
        return false;

    for (int i = 0; i < shown->element_count; i++) {
        if (!element_visual_state_equal(&shown->elements[i],
                                        &candidate->elements[i]))
            return false;
    }
    return true;
}

void overlay_render(Wc3Overlay *overlay, uint8_t *rgba, int stride_bytes) {
    if (!overlay || !rgba || overlay->width <= 0 || overlay->height <= 0)
        return;
    if (stride_bytes < overlay->width * 4) return;

    for (int y = 0; y < overlay->height; y++)
        memset(rgba + (size_t)y * (size_t)stride_bytes, 0,
               (size_t)overlay->width * 4u);

    /*
     * The Canvas atlas is the only rendering source.  Without a usable cache
     * the frame stays transparent instead of falling back to a second,
     * software-only rasteriser with different pixels.
     */
    if (!cache_ready(overlay)) return;

    for (int i = 0; i < overlay->element_count; i++) {
        const Wc3OverlayElement *e = &overlay->elements[i];
        if (e->type == WC3_OVERLAY_ELEMENT_DPAD)
            draw_dpad_cached(overlay, rgba, stride_bytes, e, i);
        else
            draw_button_cached(overlay, rgba, stride_bytes, e, i);
    }
}
