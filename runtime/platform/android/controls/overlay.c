#include "overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static uint8_t alpha_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
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

static void blend_at(const Wc3Overlay *o, uint8_t *rgba, int stride,
                     int x, int y, Wc3OverlayColor color, float coverage,
                     uint8_t alpha) {
    if (x < 0 || y < 0 || x >= o->width || y >= o->height) return;
    float a = (float)alpha * clampf(coverage, 0.0f, 1.0f);
    blend_rgba(rgba + (size_t)y * (size_t)stride + (size_t)x * 4u,
               color, alpha_u8(a), o->alpha_mode);
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
    const Wc3OverlayCache *cache = cache_source(o);
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
/* Rounded-rectangle / circle signed-distance rendering.                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool circle;
    float cx, cy;
    float hw, hh;
    float radius;
} Wc3Shape;

static float shape_sdf(const Wc3Shape *s, float px, float py) {
    float dx = px - s->cx;
    float dy = py - s->cy;
    if (s->circle) {
        return sqrtf(dx * dx + dy * dy) - s->radius;
    }

    float qx = fabsf(dx) - s->hw + s->radius;
    float qy = fabsf(dy) - s->hh + s->radius;
    float ax = qx > 0.0f ? qx : 0.0f;
    float ay = qy > 0.0f ? qy : 0.0f;
    float inside = qx > qy ? qx : qy;
    if (inside > 0.0f) inside = 0.0f;
    return sqrtf(ax * ax + ay * ay) + inside - s->radius;
}

static void fill_shape(Wc3Overlay *o, uint8_t *rgba, int stride,
                       const Wc3Shape *s, Wc3OverlayColor color,
                       uint8_t alpha) {
    if (alpha == 0) return;

    float extent_x = s->circle ? s->radius : s->hw;
    float extent_y = s->circle ? s->radius : s->hh;
    int x0 = (int)floorf(s->cx - extent_x - 2.0f);
    int x1 = (int)ceilf(s->cx + extent_x + 2.0f);
    int y0 = (int)floorf(s->cy - extent_y - 2.0f);
    int y1 = (int)ceilf(s->cy + extent_y + 2.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width - 1) x1 = o->width - 1;
    if (y1 > o->height - 1) y1 = o->height - 1;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float d = shape_sdf(s, px, py);
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_at(o, rgba, stride, x, y,
                         color, cov, alpha);
        }
    }
}

static void stroke_shape(Wc3Overlay *o, uint8_t *rgba, int stride,
                         const Wc3Shape *s, Wc3OverlayColor color,
                         uint8_t alpha, float stroke_width) {
    if (alpha == 0 || stroke_width <= 0.0f) return;

    float half = stroke_width * 0.5f;
    float extent_x = (s->circle ? s->radius : s->hw) + half + 2.0f;
    float extent_y = (s->circle ? s->radius : s->hh) + half + 2.0f;
    int x0 = (int)floorf(s->cx - extent_x);
    int x1 = (int)ceilf(s->cx + extent_x);
    int y0 = (int)floorf(s->cy - extent_y);
    int y1 = (int)ceilf(s->cy + extent_y);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width - 1) x1 = o->width - 1;
    if (y1 > o->height - 1) y1 = o->height - 1;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float d = fabsf(shape_sdf(s, px, py));
            float cov = clampf(half + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_at(o, rgba, stride, x, y,
                         color, cov, alpha);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Arbitrary rounded polygon path (D-pad petals).                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    float x, y;
} Wc3Point;

typedef struct {
    Wc3Point points[160];
    int count;
} Wc3Path;

static void path_push(Wc3Path *p, float x, float y) {
    if (p->count > 0) {
        float dx = p->points[p->count - 1].x - x;
        float dy = p->points[p->count - 1].y - y;
        if (dx * dx + dy * dy < 0.0001f) return;
    }
    if (p->count < (int)(sizeof(p->points) / sizeof(p->points[0]))) {
        p->points[p->count].x = x;
        p->points[p->count].y = y;
        p->count++;
    }
}

static float point_segment_distance(float px, float py,
                                    float ax, float ay,
                                    float bx, float by) {
    float vx = bx - ax;
    float vy = by - ay;
    float wx = px - ax;
    float wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    float dx = px - (ax + vx * t);
    float dy = py - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

static bool point_in_polygon(float px, float py,
                             const Wc3Point *v, int n) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float yi = v[i].y;
        float yj = v[j].y;
        if (((yi > py) != (yj > py)) &&
            (px < (v[j].x - v[i].x) * (py - yi) / (yj - yi) + v[i].x)) {
            inside = !inside;
        }
    }
    return inside;
}

static float path_min_distance(const Wc3Path *p, float px, float py) {
    float best = 1.0e30f;
    for (int i = 0; i < p->count; i++) {
        int j = (i + 1) % p->count;
        float d = point_segment_distance(px, py,
                                         p->points[i].x, p->points[i].y,
                                         p->points[j].x, p->points[j].y);
        if (d < best) best = d;
    }
    return best;
}

static void build_rounded_polygon(const Wc3Point *v, int n,
                                  float radius, Wc3Path *out) {
    out->count = 0;
    if (n < 3) return;

    if (radius <= 0.5f) {
        for (int i = 0; i < n; i++) path_push(out, v[i].x, v[i].y);
        return;
    }

    for (int i = 0; i < n; i++) {
        Wc3Point prev = v[(i + n - 1) % n];
        Wc3Point cur = v[i];
        Wc3Point next = v[(i + 1) % n];

        float in_x = cur.x - prev.x;
        float in_y = cur.y - prev.y;
        float out_x = next.x - cur.x;
        float out_y = next.y - cur.y;
        float in_len = sqrtf(in_x * in_x + in_y * in_y);
        float out_len = sqrtf(out_x * out_x + out_y * out_y);
        if (in_len < 0.001f || out_len < 0.001f) {
            path_push(out, cur.x, cur.y);
            continue;
        }
        in_x /= in_len;
        in_y /= in_len;
        out_x /= out_len;
        out_y /= out_len;

        float dot = in_x * out_x + in_y * out_y;
        float cross = in_x * out_y - in_y * out_x;
        float turn = atan2f(cross, dot);
        float interior = (float)M_PI - fabsf(turn);
        if (interior < 0.01f || interior > (float)M_PI - 0.01f) {
            path_push(out, cur.x, cur.y);
            continue;
        }

        float tan_half = tanf(interior * 0.5f);
        float sin_half = sinf(interior * 0.5f);
        float tangent = radius / tan_half;
        float max_tangent = 0.49f * (in_len < out_len ? in_len : out_len);
        if (tangent > max_tangent) tangent = max_tangent;
        float arc_radius = tangent * tan_half;

        float p1x = cur.x - in_x * tangent;
        float p1y = cur.y - in_y * tangent;
        float p2x = cur.x + out_x * tangent;
        float p2y = cur.y + out_y * tangent;

        float bx = out_x - in_x;
        float by = out_y - in_y;
        float blen = sqrtf(bx * bx + by * by);
        if (blen < 0.001f) {
            path_push(out, cur.x, cur.y);
            continue;
        }
        bx /= blen;
        by /= blen;
        float center_dist = arc_radius / sin_half;
        float ccx = cur.x + bx * center_dist;
        float ccy = cur.y + by * center_dist;
        if (!point_in_polygon(ccx, ccy, v, n)) {
            ccx = cur.x - bx * center_dist;
            ccy = cur.y - by * center_dist;
        }

        float a0 = atan2f(p1y - ccy, p1x - ccx);
        float a1 = atan2f(p2y - ccy, p2x - ccx);
        float delta = a1 - a0;
        while (delta > (float)M_PI) delta -= 2.0f * (float)M_PI;
        while (delta < -(float)M_PI) delta += 2.0f * (float)M_PI;

        int steps = (int)ceilf(fabsf(delta) * 6.0f);
        if (steps < 2) steps = 2;
        if (steps > 18) steps = 18;

        path_push(out, p1x, p1y);
        for (int s = 1; s < steps; s++) {
            float a = a0 + delta * (float)s / (float)steps;
            path_push(out, ccx + cosf(a) * arc_radius,
                      ccy + sinf(a) * arc_radius);
        }
        path_push(out, p2x, p2y);
    }
}

static void fill_path(Wc3Overlay *o, uint8_t *rgba, int stride,
                      const Wc3Path *p, Wc3OverlayColor color,
                      uint8_t alpha) {
    if (alpha == 0 || p->count < 3) return;

    float min_x = p->points[0].x, max_x = p->points[0].x;
    float min_y = p->points[0].y, max_y = p->points[0].y;
    for (int i = 1; i < p->count; i++) {
        if (p->points[i].x < min_x) min_x = p->points[i].x;
        if (p->points[i].x > max_x) max_x = p->points[i].x;
        if (p->points[i].y < min_y) min_y = p->points[i].y;
        if (p->points[i].y > max_y) max_y = p->points[i].y;
    }

    int x0 = (int)floorf(min_x - 1.0f);
    int x1 = (int)ceilf(max_x + 1.0f);
    int y0 = (int)floorf(min_y - 1.0f);
    int y1 = (int)ceilf(max_y + 1.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width - 1) x1 = o->width - 1;
    if (y1 > o->height - 1) y1 = o->height - 1;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            bool inside = point_in_polygon(px, py, p->points, p->count);
            float d = path_min_distance(p, px, py);
            float cov = inside ? clampf(0.5f + d, 0.0f, 1.0f)
                               : clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_at(o, rgba, stride, x, y,
                         color, cov, alpha);
        }
    }
}

static void stroke_path(Wc3Overlay *o, uint8_t *rgba, int stride,
                        const Wc3Path *p, Wc3OverlayColor color,
                        uint8_t alpha, float stroke_width) {
    if (alpha == 0 || p->count < 3 || stroke_width <= 0.0f) return;

    float half = stroke_width * 0.5f;
    float min_x = p->points[0].x, max_x = p->points[0].x;
    float min_y = p->points[0].y, max_y = p->points[0].y;
    for (int i = 1; i < p->count; i++) {
        if (p->points[i].x < min_x) min_x = p->points[i].x;
        if (p->points[i].x > max_x) max_x = p->points[i].x;
        if (p->points[i].y < min_y) min_y = p->points[i].y;
        if (p->points[i].y > max_y) max_y = p->points[i].y;
    }

    int x0 = (int)floorf(min_x - half - 2.0f);
    int x1 = (int)ceilf(max_x + half + 2.0f);
    int y0 = (int)floorf(min_y - half - 2.0f);
    int y1 = (int)ceilf(max_y + half + 2.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width - 1) x1 = o->width - 1;
    if (y1 > o->height - 1) y1 = o->height - 1;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float d = path_min_distance(p, px, py);
            float cov = clampf(half + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_at(o, rgba, stride, x, y,
                         color, cov, alpha);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Small monoline stroke font.                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    int8_t x1, y1, x2, y2;
} Wc3GlyphSeg;

typedef struct {
    const Wc3GlyphSeg *segs;
    int count;
} Wc3Glyph;

#define GLYPH(name) \
    static const Wc3GlyphSeg name##_segs[]

GLYPH(g0) = {{0,1,0,5},{0,5,1,6},{1,6,3,6},{3,6,4,5},{4,5,4,1},{4,1,3,0},{3,0,1,0},{1,0,0,1}};
GLYPH(g1) = {{1,1,2,0},{2,0,2,6},{1,6,3,6}};
GLYPH(g2) = {{0,1,1,0},{1,0,3,0},{3,0,4,1},{4,1,4,2},{4,2,0,6},{0,6,4,6}};
GLYPH(g3) = {{0,0,4,0},{4,0,2,3},{2,3,3,3},{3,3,4,4},{4,4,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5}};
GLYPH(g4) = {{3,6,3,0},{3,0,0,4},{0,4,4,4}};
GLYPH(g5) = {{4,0,0,0},{0,0,0,3},{0,3,3,3},{3,3,4,4},{4,4,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5}};
GLYPH(g6) = {{4,0,1,0},{1,0,0,1},{0,1,0,5},{0,5,1,6},{1,6,3,6},{3,6,4,5},{4,5,4,4},{4,4,3,3},{3,3,1,3},{1,3,0,4}};
GLYPH(g7) = {{0,0,4,0},{4,0,1,6}};
GLYPH(g8) = {{1,0,3,0},{3,0,4,1},{4,1,4,2},{4,2,3,3},{3,3,1,3},{1,3,0,2},{0,2,0,1},{0,1,1,0},
             {1,3,3,3},{3,3,4,4},{4,4,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5},{0,5,0,4},{0,4,1,3}};
GLYPH(g9) = {{0,6,3,6},{3,6,4,5},{4,5,4,1},{4,1,3,0},{3,0,1,0},{1,0,0,1},{0,1,0,2},{0,2,1,3},{1,3,3,3},{3,3,4,2}};
GLYPH(gA) = {{0,6,0,2},{0,2,2,0},{2,0,4,2},{4,2,4,6},{0,4,4,4}};
GLYPH(gB) = {{0,0,0,6},{0,0,3,0},{3,0,4,1},{4,1,4,2},{4,2,3,3},{3,3,0,3},
             {0,3,3,3},{3,3,4,4},{4,4,4,5},{4,5,3,6},{3,6,0,6}};
GLYPH(gC) = {{4,1,3,0},{3,0,1,0},{1,0,0,1},{0,1,0,5},{0,5,1,6},{1,6,3,6},{3,6,4,5}};
GLYPH(gD) = {{0,0,0,6},{0,0,3,0},{3,0,4,1},{4,1,4,5},{4,5,3,6},{3,6,0,6}};
GLYPH(gE) = {{4,0,0,0},{0,0,0,6},{0,6,4,6},{0,3,3,3}};
GLYPH(gF) = {{4,0,0,0},{0,0,0,6},{0,3,3,3}};
GLYPH(gG) = {{4,1,3,0},{3,0,1,0},{1,0,0,1},{0,1,0,5},{0,5,1,6},{1,6,3,6},{3,6,4,5},{4,5,4,3},{4,3,2,3}};
GLYPH(gH) = {{0,0,0,6},{4,0,4,6},{0,3,4,3}};
GLYPH(gI) = {{1,0,3,0},{2,0,2,6},{1,6,3,6}};
GLYPH(gJ) = {{4,0,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5}};
GLYPH(gK) = {{0,0,0,6},{4,0,0,4},{1,3,4,6}};
GLYPH(gL) = {{0,0,0,6},{0,6,4,6}};
GLYPH(gM) = {{0,6,0,0},{0,0,2,3},{2,3,4,0},{4,0,4,6}};
GLYPH(gN) = {{0,6,0,0},{0,0,4,6},{4,6,4,0}};
GLYPH(gO) = {{1,0,3,0},{3,0,4,1},{4,1,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5},{0,5,0,1},{0,1,1,0}};
GLYPH(gP) = {{0,6,0,0},{0,0,3,0},{3,0,4,1},{4,1,4,2},{4,2,3,3},{3,3,0,3}};
GLYPH(gQ) = {{1,0,3,0},{3,0,4,1},{4,1,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5},{0,5,0,1},{0,1,1,0},{2,4,4,6}};
GLYPH(gR) = {{0,6,0,0},{0,0,3,0},{3,0,4,1},{4,1,4,2},{4,2,3,3},{3,3,0,3},{2,3,4,6}};
GLYPH(gS) = {{4,1,3,0},{3,0,1,0},{1,0,0,1},{0,1,0,2},{0,2,1,3},{1,3,3,3},{3,3,4,4},{4,4,4,5},{4,5,3,6},{3,6,1,6},{1,6,0,5}};
GLYPH(gT) = {{0,0,4,0},{2,0,2,6}};
GLYPH(gU) = {{0,0,0,5},{0,5,1,6},{1,6,3,6},{3,6,4,5},{4,5,4,0}};
GLYPH(gV) = {{0,0,2,6},{2,6,4,0}};
GLYPH(gW) = {{0,0,1,6},{1,6,2,3},{2,3,3,6},{3,6,4,0}};
GLYPH(gX) = {{0,0,4,6},{4,0,0,6}};
GLYPH(gY) = {{0,0,2,3},{4,0,2,3},{2,3,2,6}};
GLYPH(gZ) = {{0,0,4,0},{4,0,0,6},{0,6,4,6}};

static const Wc3Glyph k_glyphs[36] = {
    {g0_segs, (int)(sizeof(g0_segs) / sizeof(g0_segs[0]))},
    {g1_segs, (int)(sizeof(g1_segs) / sizeof(g1_segs[0]))},
    {g2_segs, (int)(sizeof(g2_segs) / sizeof(g2_segs[0]))},
    {g3_segs, (int)(sizeof(g3_segs) / sizeof(g3_segs[0]))},
    {g4_segs, (int)(sizeof(g4_segs) / sizeof(g4_segs[0]))},
    {g5_segs, (int)(sizeof(g5_segs) / sizeof(g5_segs[0]))},
    {g6_segs, (int)(sizeof(g6_segs) / sizeof(g6_segs[0]))},
    {g7_segs, (int)(sizeof(g7_segs) / sizeof(g7_segs[0]))},
    {g8_segs, (int)(sizeof(g8_segs) / sizeof(g8_segs[0]))},
    {g9_segs, (int)(sizeof(g9_segs) / sizeof(g9_segs[0]))},
    {gA_segs, (int)(sizeof(gA_segs) / sizeof(gA_segs[0]))},
    {gB_segs, (int)(sizeof(gB_segs) / sizeof(gB_segs[0]))},
    {gC_segs, (int)(sizeof(gC_segs) / sizeof(gC_segs[0]))},
    {gD_segs, (int)(sizeof(gD_segs) / sizeof(gD_segs[0]))},
    {gE_segs, (int)(sizeof(gE_segs) / sizeof(gE_segs[0]))},
    {gF_segs, (int)(sizeof(gF_segs) / sizeof(gF_segs[0]))},
    {gG_segs, (int)(sizeof(gG_segs) / sizeof(gG_segs[0]))},
    {gH_segs, (int)(sizeof(gH_segs) / sizeof(gH_segs[0]))},
    {gI_segs, (int)(sizeof(gI_segs) / sizeof(gI_segs[0]))},
    {gJ_segs, (int)(sizeof(gJ_segs) / sizeof(gJ_segs[0]))},
    {gK_segs, (int)(sizeof(gK_segs) / sizeof(gK_segs[0]))},
    {gL_segs, (int)(sizeof(gL_segs) / sizeof(gL_segs[0]))},
    {gM_segs, (int)(sizeof(gM_segs) / sizeof(gM_segs[0]))},
    {gN_segs, (int)(sizeof(gN_segs) / sizeof(gN_segs[0]))},
    {gO_segs, (int)(sizeof(gO_segs) / sizeof(gO_segs[0]))},
    {gP_segs, (int)(sizeof(gP_segs) / sizeof(gP_segs[0]))},
    {gQ_segs, (int)(sizeof(gQ_segs) / sizeof(gQ_segs[0]))},
    {gR_segs, (int)(sizeof(gR_segs) / sizeof(gR_segs[0]))},
    {gS_segs, (int)(sizeof(gS_segs) / sizeof(gS_segs[0]))},
    {gT_segs, (int)(sizeof(gT_segs) / sizeof(gT_segs[0]))},
    {gU_segs, (int)(sizeof(gU_segs) / sizeof(gU_segs[0]))},
    {gV_segs, (int)(sizeof(gV_segs) / sizeof(gV_segs[0]))},
    {gW_segs, (int)(sizeof(gW_segs) / sizeof(gW_segs[0]))},
    {gX_segs, (int)(sizeof(gX_segs) / sizeof(gX_segs[0]))},
    {gY_segs, (int)(sizeof(gY_segs) / sizeof(gY_segs[0]))},
    {gZ_segs, (int)(sizeof(gZ_segs) / sizeof(gZ_segs[0]))},
};

static const Wc3Glyph *glyph_for(char c) {
    if (c >= '0' && c <= '9') return &k_glyphs[c - '0'];
    if (c >= 'A' && c <= 'Z') return &k_glyphs[10 + (c - 'A')];
    if (c >= 'a' && c <= 'z') return &k_glyphs[10 + (c - 'a')];
    return NULL;
}

static void draw_capsule(Wc3Overlay *o, uint8_t *rgba, int stride,
                         float ax, float ay, float bx, float by,
                         float radius, Wc3OverlayColor color, uint8_t alpha) {
    if (radius < 0.5f) radius = 0.5f;
    float min_x = (ax < bx ? ax : bx) - radius - 2.0f;
    float max_x = (ax > bx ? ax : bx) + radius + 2.0f;
    float min_y = (ay < by ? ay : by) - radius - 2.0f;
    float max_y = (ay > by ? ay : by) + radius + 2.0f;
    int x0 = (int)floorf(min_x);
    int x1 = (int)ceilf(max_x);
    int y0 = (int)floorf(min_y);
    int y1 = (int)ceilf(max_y);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > o->width - 1) x1 = o->width - 1;
    if (y1 > o->height - 1) y1 = o->height - 1;

    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float d = point_segment_distance(px, py, ax, ay, bx, by);
            float cov = clampf(radius + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend_at(o, rgba, stride, x, y,
                         color, cov, alpha);
        }
    }
}

static void draw_text(Wc3Overlay *o, uint8_t *rgba, int stride,
                      const Wc3OverlayElement *e, Wc3OverlayColor color,
                      uint8_t alpha, float stroke_width) {
    if (e->text[0] == '\0' || alpha == 0) return;

    int n = 0;
    while (e->text[n] != '\0' && n < WC3_OVERLAY_TEXT_MAX - 1) n++;
    if (n == 0) return;

    float avail_w = (e->right - e->left) - stroke_width * 2.0f;
    float avail_h = (e->bottom - e->top) - stroke_width * 2.0f;
    if (avail_w <= 1.0f || avail_h <= 1.0f) return;

    /*
     * The monoline font is wider than Android's sans face at the same cap
     * height.  Leave a visible side margin instead of letting labels touch the
     * button stroke, while keeping the reference's 2 * snap * scale height cap.
     */
    float max_h = o->snapping_size * 2.0f * e->scale;
    float width_h = avail_w * 0.70f * 6.0f / (float)(n * 5 - 1);
    float text_h = width_h < max_h ? width_h : max_h;
    if (text_h > avail_h) text_h = avail_h;
    if (text_h < 4.0f) text_h = 4.0f;

    float unit = text_h / 6.0f;
    float total_w = (float)(n * 5 - 1) * unit;
    float cx = (e->left + e->right) * 0.5f;
    float cy = (e->top + e->bottom) * 0.5f;
    float start_x = cx - total_w * 0.5f;
    float start_y = cy - text_h * 0.5f;
    float line_w = text_h * 0.11f;
    if (line_w < 1.0f) line_w = 1.0f;
    float line_r = line_w * 0.5f;

    for (int i = 0; i < n; i++) {
        const Wc3Glyph *g = glyph_for(e->text[i]);
        if (!g) continue;
        float gx = start_x + (float)i * 5.0f * unit;
        for (int s = 0; s < g->count; s++) {
            const Wc3GlyphSeg *seg = &g->segs[s];
            draw_capsule(o, rgba, stride,
                         gx + (float)seg->x1 * unit,
                         start_y + (float)seg->y1 * unit,
                         gx + (float)seg->x2 * unit,
                         start_y + (float)seg->y2 * unit,
                         line_r, color, alpha);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Element drawing.                                                          */
/* ------------------------------------------------------------------------- */

static float effective_corner_radius(const Wc3Overlay *o,
                                     const Wc3OverlayElement *e) {
    float r = e->corner_radius;
    if (r < 0.0f) r = o->style.corner_radius;
    return r;
}

static float effective_dpad_radius(const Wc3Overlay *o,
                                   const Wc3OverlayElement *e) {
    /* The shipped renderer ignores the profile's dpadCornerRadius field. */
    return effective_corner_radius(o, e);
}

static bool draw_button_cached(Wc3Overlay *o, uint8_t *rgba, int stride,
                               const Wc3OverlayElement *e, int index) {
    if (!cache_ready(o) || e->selected) return false;

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

static void draw_button(Wc3Overlay *o, uint8_t *rgba, int stride,
                        const Wc3OverlayElement *e) {
    float stroke_w = o->snapping_size * o->style.stroke_width_multiplier;
    if (stroke_w < 1.0f) stroke_w = 1.0f;
    float half_stroke = stroke_w * 0.5f;
    float r = effective_corner_radius(o, e) * o->snapping_size * e->scale;
    float max_r = (e->hw < e->hh ? e->hw : e->hh);
    if (r > max_r) r = max_r;
    if (r < 0.0f) r = 0.0f;

    Wc3Shape shape;
    shape.circle = (e->shape == WC3_OVERLAY_SHAPE_CIRCLE);
    shape.cx = (e->left + e->right) * 0.5f;
    shape.cy = (e->top + e->bottom) * 0.5f;
    shape.hw = e->hw;
    shape.hh = e->hh;
    shape.radius = shape.circle ? e->hw : r;

    uint8_t base_alpha = effective_element_alpha(o, e);
    Wc3OverlayColor primary = o->style.primary;
    Wc3OverlayColor secondary = o->style.secondary;
    /*
     * Selected is an editor/inactive affordance in the shipped renderer.  An
     * engaged element always uses the primary colour.
     */
    bool use_secondary = e->selected && e->visual_layers == 0;
    Wc3OverlayColor stroke_color = use_secondary ? secondary : primary;
    Wc3OverlayColor text_color = use_secondary ? secondary : primary;

    uint8_t inactive_fill_alpha = 0;
    if (o->style.fill_alpha_inactive > 0) {
        inactive_fill_alpha = alpha_u8((float)base_alpha *
                                       (float)o->style.fill_alpha_inactive /
                                       255.0f);
    }

    if (e->visual_layers == 0) {
        if (inactive_fill_alpha > 0) {
            Wc3Shape fill = shape;
            fill.hw += half_stroke;
            fill.hh += half_stroke;
            if (fill.circle) fill.radius += half_stroke;
            else fill.radius = r + half_stroke;
            fill_shape(o, rgba, stride, &fill, primary, inactive_fill_alpha);
        }
        stroke_shape(o, rgba, stride, &shape, stroke_color, base_alpha,
                     stroke_w);
        draw_text(o, rgba, stride, e, text_color, base_alpha, stroke_w);
        return;
    }

    /* Shipped compositor order: fill, stroke/text, glow, outer stroke. */
    if (e->visual_layers & WC3_OVERLAY_LAYER_FILL) {
        uint8_t fill_alpha = mul_u8(base_alpha, e->alpha_fill);
        if (fill_alpha > 0) {
            Wc3Shape fill = shape;
            fill.hw += half_stroke;
            fill.hh += half_stroke;
            if (fill.circle) fill.radius += half_stroke;
            else fill.radius = r + half_stroke;
            fill_shape(o, rgba, stride, &fill, primary, fill_alpha);
        }
    }

    if ((e->visual_layers & WC3_OVERLAY_LAYER_STROKE_TEXT) &&
        ((e->visual_layers & WC3_OVERLAY_LAYER_GLOW) ||
         (e->visual_layers & WC3_OVERLAY_LAYER_FILL) == 0)) {
        uint8_t stroke_alpha = mul_u8(base_alpha, e->alpha_stroke);
        stroke_shape(o, rgba, stride, &shape, stroke_color, stroke_alpha,
                     stroke_w);
        draw_text(o, rgba, stride, e, text_color, stroke_alpha, stroke_w);
    }

    if (e->visual_layers & WC3_OVERLAY_LAYER_GLOW) {
        uint8_t glow_alpha = mul_u8(base_alpha, e->alpha_glow);
        float glow_thickness = o->snapping_size * 3.0f;
        float glow_w = glow_thickness * 0.7f;
        float glow_off = glow_thickness * 0.45f;
        Wc3Shape glow = shape;
        glow.hw += glow_off;
        glow.hh += glow_off;
        if (glow.circle) glow.radius += glow_off;
        else glow.radius = r + glow_off;
        /*
         * The reference uses a blurred mask.  A two-pass soft ring gives the
         * same visual role without an offscreen blur and stays resolution
         * independent.
         */
        stroke_shape(o, rgba, stride, &glow, primary,
                     alpha_u8((float)mul_u8(glow_alpha, 96)), glow_w);
        Wc3Shape glow2 = glow;
        glow2.hw += glow_w * 0.6f;
        glow2.hh += glow_w * 0.6f;
        if (glow2.circle) glow2.radius += glow_w * 0.6f;
        else glow2.radius += glow_w * 0.6f;
        stroke_shape(o, rgba, stride, &glow2, primary,
                     alpha_u8((float)mul_u8(glow_alpha, 96) * 0.50f),
                     glow_w * 0.8f);
    }

    if (e->visual_layers & WC3_OVERLAY_LAYER_OUTER_STROKE) {
        uint8_t outer_alpha = mul_u8(base_alpha, e->alpha_outer);
        float outer_off = stroke_w * 0.5f + o->snapping_size * 0.1f +
                          stroke_w * 0.5f;
        Wc3Shape outer = shape;
        outer.hw += outer_off;
        outer.hh += outer_off;
        if (outer.circle) outer.radius += outer_off;
        else outer.radius = r + outer_off;
        stroke_shape(o, rgba, stride, &outer, primary, outer_alpha, stroke_w);
    }
}

static void dpad_petal_vertices(const Wc3OverlayElement *e, float snap,
                                int index, Wc3Point out[5]) {
    float start = snap * e->scale;
    float ox = snap * 2.0f * e->scale;
    float oy = snap * 3.0f * e->scale;
    float cx = (e->left + e->right) * 0.5f;
    float cy = (e->top + e->bottom) * 0.5f;

    if (index == 0) {
        out[0] = (Wc3Point){cx, cy - start};
        out[1] = (Wc3Point){cx - ox, cy - oy};
        out[2] = (Wc3Point){cx - ox, e->top};
        out[3] = (Wc3Point){cx + ox, e->top};
        out[4] = (Wc3Point){cx + ox, cy - oy};
    } else if (index == 1) {
        out[0] = (Wc3Point){cx + start, cy};
        out[1] = (Wc3Point){cx + oy, cy - ox};
        out[2] = (Wc3Point){e->right, cy - ox};
        out[3] = (Wc3Point){e->right, cy + ox};
        out[4] = (Wc3Point){cx + oy, cy + ox};
    } else if (index == 2) {
        out[0] = (Wc3Point){cx, cy + start};
        out[1] = (Wc3Point){cx - ox, cy + oy};
        out[2] = (Wc3Point){cx - ox, e->bottom};
        out[3] = (Wc3Point){cx + ox, e->bottom};
        out[4] = (Wc3Point){cx + ox, cy + oy};
    } else {
        out[0] = (Wc3Point){cx - start, cy};
        out[1] = (Wc3Point){cx - oy, cy - ox};
        out[2] = (Wc3Point){e->left, cy - ox};
        out[3] = (Wc3Point){e->left, cy + ox};
        out[4] = (Wc3Point){cx - oy, cy + ox};
    }
}

static void draw_dpad(Wc3Overlay *o, uint8_t *rgba, int stride,
                      const Wc3OverlayElement *e) {
    float stroke_w = o->snapping_size * o->style.stroke_width_multiplier;
    if (stroke_w < 1.0f) stroke_w = 1.0f;
    float r = effective_dpad_radius(o, e) * o->snapping_size * e->scale;
    uint8_t base_alpha = effective_element_alpha(o, e);

    Wc3Path paths[4];
    for (int i = 0; i < 4; i++) {
        Wc3Point verts[5];
        dpad_petal_vertices(e, o->snapping_size, i, verts);
        build_rounded_polygon(verts, 5, r, &paths[i]);
    }

    uint8_t inactive_fill_alpha = 0;
    if (o->style.fill_alpha_inactive > 0) {
        inactive_fill_alpha = alpha_u8((float)base_alpha *
                                       (float)o->style.fill_alpha_inactive /
                                       255.0f);
    }
    Wc3OverlayColor stroke_color = e->selected && !e->engaged
                                       ? o->style.secondary
                                       : o->style.primary;

    /*
     * The shipped D-pad cache has one inactive petal (fillAlphaInactive fill
     * plus inactive stroke) and one active petal (primary fill, no stroke).
     */
    for (int i = 0; i < 4; i++) {
        bool active = e->engaged && (e->active_petals & (1u << i));
        if (active) {
            fill_path(o, rgba, stride, &paths[i], o->style.primary,
                      base_alpha);
        } else {
            if (inactive_fill_alpha > 0)
                fill_path(o, rgba, stride, &paths[i], o->style.primary,
                          inactive_fill_alpha);
            stroke_path(o, rgba, stride, &paths[i], stroke_color, base_alpha,
                        stroke_w);
        }
    }
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

    Wc3OverlayCacheLayer *layer =
        &overlay->cache.layers[overlay->cache.layer_count++];
    layer->element = element;
    layer->slot = slot;
    layer->src_x = src_x;
    layer->src_y = src_y;
    layer->src_w = src_w;
    layer->src_h = src_h;
    layer->dst_x = dst_x;
    layer->dst_y = dst_y;
    layer->rotation = rotation;
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

void overlay_render(Wc3Overlay *overlay, uint8_t *rgba, int stride_bytes) {
    if (!overlay || !rgba || overlay->width <= 0 || overlay->height <= 0)
        return;
    if (stride_bytes < overlay->width * 4) return;

    for (int y = 0; y < overlay->height; y++)
        memset(rgba + (size_t)y * (size_t)stride_bytes, 0,
               (size_t)overlay->width * 4u);

    for (int i = 0; i < overlay->element_count; i++) {
        const Wc3OverlayElement *e = &overlay->elements[i];
        if (e->type == WC3_OVERLAY_ELEMENT_DPAD) {
            if (!draw_dpad_cached(overlay, rgba, stride_bytes, e, i))
                draw_dpad(overlay, rgba, stride_bytes, e);
        } else {
            if (!draw_button_cached(overlay, rgba, stride_bytes, e, i))
                draw_button(overlay, rgba, stride_bytes, e);
        }
    }
}


