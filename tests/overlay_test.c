/*
 * overlay_test.c - T0 contract for the cached Canvas compositor.
 *
 * The overlay hot path must never rasterise shapes; it blits a Canvas-built
 * atlas and refuses a cache whose geometry or element count does not match the
 * snapshot it is asked to draw.  These checks run without Android.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static uint8_t *new_atlas(int width, int height, int stride)
{
    uint8_t *pixels = (uint8_t *)calloc((size_t)stride * (size_t)height, 1);
    if (!pixels) return NULL;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *p = pixels + (size_t)y * (size_t)stride +
                         (size_t)x * 4u;
            p[0] = 255;
            p[1] = 0;
            p[2] = 0;
            p[3] = 255;
        }
    }
    return pixels;
}

static int add_test_button(Wc3Overlay *overlay)
{
    Wc3OverlayElement element;
    memset(&element, 0, sizeof element);
    element.type = WC3_OVERLAY_ELEMENT_BUTTON;
    element.shape = WC3_OVERLAY_SHAPE_RECT;
    element.nx = 0.5f;
    element.ny = 0.5f;
    element.width_units = 1.0f;
    element.height_units = 1.0f;
    element.scale = 1.0f;
    return overlay_add_element(overlay, &element);
}

static int all_transparent(const uint8_t *rgba, int width, int height,
                           int stride)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t *p = rgba + (size_t)y * (size_t)stride +
                               (size_t)x * 4u;
            if (p[3] != 0) return 0;
        }
    }
    return 1;
}

static int add_test_dpad(Wc3Overlay *overlay)
{
    Wc3OverlayElement element;
    memset(&element, 0, sizeof element);
    element.type = WC3_OVERLAY_ELEMENT_DPAD;
    element.shape = WC3_OVERLAY_SHAPE_CIRCLE;
    element.nx = 0.5f;
    element.ny = 0.5f;
    element.scale = 1.0f;
    return overlay_add_element(overlay, &element);
}

static int test_visual_state_equality(void)
{
    Wc3Overlay shown;
    Wc3Overlay candidate;
    memset(&shown, 0, sizeof shown);
    memset(&candidate, 0, sizeof candidate);
    overlay_init(&shown, 32, 32, NULL);
    overlay_init(&candidate, 32, 32, NULL);
    CHECK(add_test_button(&shown) == 0, "add shown button");
    CHECK(add_test_button(&candidate) == 0, "add candidate button");

    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "identical buttons compare equal");

    /* The current button compositor does not read these engine flags. */
    shown.elements[0].selected = true;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "button selected is not rendered");
    candidate.elements[0].engaged = true;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "button engaged is not rendered");

    /* alpha_fill is inert without the FILL layer. */
    shown.elements[0].visual_layers = WC3_OVERLAY_LAYER_STROKE_TEXT;
    candidate.elements[0].visual_layers = WC3_OVERLAY_LAYER_STROKE_TEXT;
    shown.elements[0].alpha_fill = 1;
    candidate.elements[0].alpha_fill = 2;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "inactive fill alpha is not rendered");

    shown.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    candidate.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "active fill alpha is rendered");
    candidate.elements[0].alpha_fill = shown.elements[0].alpha_fill;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "matching active fill alpha compares equal");

    shown.elements[0].visual_layers = WC3_OVERLAY_LAYER_GLOW;
    candidate.elements[0].visual_layers = WC3_OVERLAY_LAYER_GLOW;
    shown.elements[0].alpha_glow = 1;
    candidate.elements[0].alpha_glow = 2;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "active glow alpha is rendered");
    candidate.elements[0].alpha_glow = shown.elements[0].alpha_glow;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "matching active glow alpha compares equal");

    shown.style.overlay_opacity = 0.5f;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "global overlay opacity is rendered");
    shown.style.overlay_opacity = candidate.style.overlay_opacity;

    /* STROKE_TEXT is redundant on an otherwise empty mask. */
    shown.elements[0].visual_layers = 0;
    candidate.elements[0].visual_layers = WC3_OVERLAY_LAYER_STROKE_TEXT;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "redundant stroke bit is not a frame change");
    candidate.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "fill changes the frame");

    /* D-pad state is engaged + petals; the button layer mask is inert. */
    overlay_clear_elements(&shown);
    overlay_clear_elements(&candidate);
    CHECK(add_test_dpad(&shown) == 0, "add shown dpad");
    CHECK(add_test_dpad(&candidate) == 0, "add candidate dpad");
    shown.elements[0].has_opacity = true;
    shown.elements[0].opacity = 0.5f;
    candidate.elements[0].has_opacity = true;
    candidate.elements[0].opacity = 0.5f;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "identical dpads compare equal");

    candidate.elements[0].engaged = true;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "dpad engaged is rendered");
    candidate.elements[0].engaged = false;
    candidate.elements[0].active_petals = 1;
    CHECK(!overlay_visual_state_equal(&shown, &candidate),
          "dpad active petals are rendered");
    candidate.elements[0].active_petals = 0;
    shown.elements[0].selected = true;
    CHECK(overlay_visual_state_equal(&shown, &candidate),
          "dpad selected is not rendered");

    overlay_destroy(&shown);
    overlay_destroy(&candidate);
    return 0;
}

static int test_publish_cadence(void)
{
    Wc3Overlay snapshots[2];
    memset(snapshots, 0, sizeof snapshots);
    overlay_init(&snapshots[0], 32, 32, NULL);
    overlay_init(&snapshots[1], 32, 32, NULL);
    CHECK(add_test_button(&snapshots[0]) == 0, "add active button");
    CHECK(add_test_button(&snapshots[1]) == 0, "add staging button");

    Wc3Overlay *shown = &snapshots[0];
    Wc3Overlay *staging = &snapshots[1];
    int published = 0;

    /* One visible press: exactly one publish. */
    staging->elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    staging->elements[0].alpha_fill = 255;
    if (!overlay_visual_state_equal(shown, staging)) {
        Wc3Overlay *swap = shown;
        shown = staging;
        staging = swap;
        published++;
    }
    CHECK(published == 1, "visible press publishes once");
    CHECK(shown->elements[0].visual_layers == WC3_OVERLAY_LAYER_FILL,
          "press became the displayed snapshot");

    /* The next engine tick is unchanged: no second present. */
    staging->elements[0].visual_layers = shown->elements[0].visual_layers;
    staging->elements[0].alpha_fill = shown->elements[0].alpha_fill;
    if (!overlay_visual_state_equal(shown, staging)) {
        Wc3Overlay *swap = shown;
        shown = staging;
        staging = swap;
        published++;
    }
    CHECK(published == 1, "duplicate tick does not publish");

    /* Release is a real transition and must not be lost. */
    staging->elements[0].visual_layers = 0;
    if (!overlay_visual_state_equal(shown, staging)) {
        Wc3Overlay *swap = shown;
        shown = staging;
        staging = swap;
        published++;
    }
    CHECK(published == 2, "release publishes once");
    CHECK(shown->elements[0].visual_layers == 0,
          "release became the displayed snapshot");

    /* Engine-only flags are not a frame change. */
    staging->elements[0].visual_layers = shown->elements[0].visual_layers;
    staging->elements[0].alpha_fill = shown->elements[0].alpha_fill;
    staging->elements[0].selected = true;
    if (!overlay_visual_state_equal(shown, staging)) {
        Wc3Overlay *swap = shown;
        shown = staging;
        staging = swap;
        published++;
    }
    CHECK(published == 2, "invisible selected change does not publish");

    overlay_destroy(&snapshots[0]);
    overlay_destroy(&snapshots[1]);
    return 0;
}

static uint8_t *new_pattern_atlas(void)
{
    /* Left 2x2 is the visible pattern; right 2x2 stays transparent for the
     * inactive D-pad layer. */
    uint8_t *pixels = (uint8_t *)calloc(16u * 2u, 1);
    if (!pixels) return NULL;
    static const uint8_t colors[4][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}
    };
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint8_t *p = pixels + (size_t)y * 16u + (size_t)x * 4u;
            p[0] = colors[y * 2 + x][0];
            p[1] = colors[y * 2 + x][1];
            p[2] = colors[y * 2 + x][2];
            p[3] = 255;
        }
    }
    return pixels;
}

static int pixel_is(const uint8_t *frame, int stride, int x, int y,
                    int r, int g, int b)
{
    const uint8_t *p = frame + (size_t)y * (size_t)stride + (size_t)x * 4u;
    int ok = p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
    if (!ok) {
        printf("pixel (%d,%d) got %u,%u,%u,%u expected %d,%d,%d,255\n",
               x, y, p[0], p[1], p[2], p[3], r, g, b);
    }
    return ok;
}

static int test_rotation_blit(void)
{
    static const uint8_t expected[4][4][3] = {
        {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}},
        {{0, 0, 255}, {255, 0, 0}, {255, 255, 255}, {0, 255, 0}},
        {{255, 255, 255}, {0, 0, 255}, {0, 255, 0}, {255, 0, 0}},
        {{0, 255, 0}, {255, 255, 255}, {255, 0, 0}, {0, 0, 255}},
    };
    static const int points[4][2] = {{10, 10}, {11, 10}, {10, 11}, {11, 11}};
    Wc3Overlay overlay;
    uint8_t frame[32 * 32 * 4];
    memset(&overlay, 0, sizeof overlay);
    overlay_init(&overlay, 32, 32, NULL);
    overlay.style.overlay_opacity = 1.0f;
    CHECK(add_test_dpad(&overlay) == 0, "add rotation dpad");
    overlay.elements[0].engaged = true;
    overlay.elements[0].active_petals = 1u;
    overlay.alpha_mode = WC3_OVERLAY_ALPHA_STRAIGHT;

    uint8_t *atlas = new_pattern_atlas();
    CHECK(atlas != NULL, "allocate pattern atlas");
    CHECK(overlay_cache_set_atlas(&overlay, atlas, 4, 2, 16, 1) == 0,
          "set pattern atlas");
    CHECK(overlay_cache_add_layer(&overlay, 0,
                                  WC3_OVERLAY_CACHE_DPAD_ACTIVE,
                                  0, 0, 2, 2, 10, 10, 0) == 0,
          "add active rotation layer");
    CHECK(overlay_cache_add_layer(&overlay, 0,
                                  WC3_OVERLAY_CACHE_DPAD_INACTIVE,
                                  2, 0, 2, 2, 10, 10, 0) == 0,
          "add inactive rotation layer");

    for (int rotation = 0; rotation < 4; rotation++) {
        overlay.cache.layers[0].rotation = rotation * 90;
        overlay_render(&overlay, frame, 32 * 4);
        for (int p = 0; p < 4; p++) {
            const uint8_t *c = expected[rotation][p];
            CHECK(pixel_is(frame, 32 * 4, points[p][0], points[p][1],
                           c[0], c[1], c[2]),
                  "rotation blit keeps source orientation");
        }
    }
    overlay_destroy(&overlay);
    return 0;
}

static uint32_t hash_frame(const uint8_t *pixels, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= pixels[i];
        hash *= 16777619u;
    }
    return hash;
}

static int test_pixel_hash(void)
{
    Wc3Overlay overlay;
    memset(&overlay, 0, sizeof overlay);
    overlay_init(&overlay, 48, 48, NULL);
    overlay.style.overlay_opacity = 0.73f;
    overlay.style.fill_alpha_inactive = 37;
    overlay.alpha_mode = WC3_OVERLAY_ALPHA_PREMULTIPLIED;
    CHECK(add_test_button(&overlay) == 0, "add hash button");
    CHECK(add_test_dpad(&overlay) == 1, "add hash dpad");
    overlay.elements[0].has_opacity = true;
    overlay.elements[0].opacity = 0.81f;
    overlay.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL |
                                        WC3_OVERLAY_LAYER_STROKE_TEXT |
                                        WC3_OVERLAY_LAYER_GLOW |
                                        WC3_OVERLAY_LAYER_OUTER_STROKE;
    overlay.elements[0].alpha_fill = 200;
    overlay.elements[0].alpha_stroke = 180;
    overlay.elements[0].alpha_glow = 160;
    overlay.elements[0].alpha_outer = 140;
    overlay.elements[1].engaged = true;
    overlay.elements[1].active_petals = 0x5u;

    uint8_t *atlas = (uint8_t *)calloc(256u * 64u, 1);
    CHECK(atlas != NULL, "allocate hash atlas");
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t *p = atlas + (size_t)y * 256u + (size_t)x * 4u;
            p[0] = (uint8_t)(x * 7 + y * 3);
            p[1] = (uint8_t)(x * 5 + y * 11);
            p[2] = (uint8_t)(x * 13 + y * 17);
            p[3] = 255;
        }
    }
    CHECK(overlay_cache_set_atlas(&overlay, atlas, 64, 64, 256, 2) == 0,
          "set hash atlas");
    CHECK(overlay_cache_add_layer(&overlay, 0, WC3_OVERLAY_CACHE_FILL,
                                  0, 0, 16, 16, 16, 16, 0) == 0,
          "hash fill");
    CHECK(overlay_cache_add_layer(&overlay, 0,
                                  WC3_OVERLAY_CACHE_STROKE_TEXT,
                                  16, 0, 16, 16, 16, 16, 0) == 0,
          "hash stroke");
    CHECK(overlay_cache_add_layer(&overlay, 0, WC3_OVERLAY_CACHE_GLOW,
                                  32, 0, 16, 16, 16, 16, 0) == 0,
          "hash glow");
    CHECK(overlay_cache_add_layer(&overlay, 0,
                                  WC3_OVERLAY_CACHE_OUTER_STROKE,
                                  48, 0, 16, 16, 16, 16, 0) == 0,
          "hash outer");
    CHECK(overlay_cache_add_layer(&overlay, 1,
                                  WC3_OVERLAY_CACHE_DPAD_ACTIVE,
                                  0, 16, 16, 16, 4, 4, 0) == 0,
          "hash dpad active");
    CHECK(overlay_cache_add_layer(&overlay, 1,
                                  WC3_OVERLAY_CACHE_DPAD_INACTIVE,
                                  16, 16, 16, 16, 4, 4, 0) == 0,
          "hash dpad inactive");
    CHECK(overlay_cache_add_layer(&overlay, 1, WC3_OVERLAY_CACHE_FILL,
                                  32, 16, 16, 16, 4, 4, 0) == 0,
          "hash dpad fill");

    uint8_t frame[48 * 48 * 4];
    overlay_render(&overlay, frame, 48 * 4);
    uint32_t hash = hash_frame(frame, sizeof frame);
    CHECK(hash == 0xec1eb1adu,
          "pixel hash matches the locked compositor output");
    overlay_destroy(&overlay);
    return 0;
}

int main(void)
{
    uint8_t frame[32 * 32 * 4];

    Wc3Overlay overlay;
    memset(&overlay, 0, sizeof overlay);
    overlay_init(&overlay, 32, 32, NULL);
    CHECK(add_test_button(&overlay) == 0, "add button");

    overlay_render(&overlay, frame, 32 * 4);
    CHECK(all_transparent(frame, 32, 32, 32 * 4),
          "no cache leaves the frame transparent");

    uint8_t *atlas = new_atlas(8, 8, 32);
    CHECK(atlas != NULL, "allocate atlas");
    CHECK(overlay_cache_set_atlas(&overlay, atlas, 8, 8, 32, 1) == 0,
          "set atlas");
    CHECK(overlay_cache_add_layer(&overlay, 0, WC3_OVERLAY_CACHE_FILL,
                                  0, 0, 8, 8, 11, 11, 0) == 0,
          "add fill layer");
    CHECK(overlay_cache_add_layer(&overlay, 0, WC3_OVERLAY_CACHE_STROKE_TEXT,
                                  0, 0, 8, 8, 11, 11, 0) == 0,
          "add stroke/text layer");

    overlay.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    overlay.elements[0].alpha_fill = 255;
    overlay.alpha_mode = WC3_OVERLAY_ALPHA_STRAIGHT;
    overlay_render(&overlay, frame, 32 * 4);
    const uint8_t *center = frame + (size_t)12 * 32 * 4 + 12u * 4u;
    CHECK(center[0] == 255 && center[1] == 0 && center[2] == 0,
          "cached fill colour");
    CHECK(center[3] == 102, "default overlay opacity truncates to 102");
    CHECK(all_transparent(frame, 11, 32, 32 * 4),
          "left of the layer stays transparent");

    overlay.alpha_mode = WC3_OVERLAY_ALPHA_PREMULTIPLIED;
    overlay_render(&overlay, frame, 32 * 4);
    center = frame + (size_t)12 * 32 * 4 + 12u * 4u;
    CHECK(center[0] == 102 && center[1] == 0 && center[2] == 0 &&
              center[3] == 102,
          "premultiplied SurfaceView output");

    /* A cache built for a different surface size must be ignored. */
    uint8_t frame64[64 * 64 * 4];
    overlay_resize(&overlay, 64, 64);
    overlay_render(&overlay, frame64, 64 * 4);
    CHECK(all_transparent(frame64, 64, 64, 64 * 4),
          "resize rejects the owned cache");

    /* Borrowed views follow the same geometry and element-count rule and are
     * never freed by the consuming overlay. */
    Wc3OverlayCache borrowed;
    memset(&borrowed, 0, sizeof borrowed);
    borrowed.pixels = new_atlas(8, 8, 32);
    CHECK(borrowed.pixels != NULL, "allocate borrowed atlas");
    borrowed.width = 8;
    borrowed.height = 8;
    borrowed.stride_bytes = 32;
    borrowed.element_count = 1;
    borrowed.overlay_width = 32;
    borrowed.overlay_height = 32;
    borrowed.layer_count = 2;
    borrowed.layers[0].element = 0;
    borrowed.layers[0].slot = WC3_OVERLAY_CACHE_FILL;
    borrowed.layers[0].src_w = 8;
    borrowed.layers[0].src_h = 8;
    borrowed.layers[0].dst_x = 11;
    borrowed.layers[0].dst_y = 11;
    borrowed.layers[1].element = 0;
    borrowed.layers[1].slot = WC3_OVERLAY_CACHE_STROKE_TEXT;
    borrowed.layers[1].src_w = 8;
    borrowed.layers[1].src_h = 8;
    borrowed.layers[1].dst_x = 11;
    borrowed.layers[1].dst_y = 11;

    Wc3Overlay view;
    memset(&view, 0, sizeof view);
    overlay_init(&view, 32, 32, NULL);
    CHECK(add_test_button(&view) == 0, "add view button");
    view.elements[0].visual_layers = WC3_OVERLAY_LAYER_FILL;
    view.cache_view = &borrowed;
    overlay_render(&view, frame, 32 * 4);
    center = frame + (size_t)12 * 32 * 4 + 12u * 4u;
    CHECK(center[3] != 0, "borrowed cache renders");

    /* A borrowed cache has no index table; the linear fallback must still
     * find the stroke layer when the fill layer is not the requested slot. */
    view.elements[0].visual_layers = 0;
    overlay_render(&view, frame, 32 * 4);
    center = frame + (size_t)12 * 32 * 4 + 12u * 4u;
    CHECK(center[3] != 0, "borrowed cache linear fallback renders stroke");

    borrowed.element_count = 2;
    overlay_render(&view, frame, 32 * 4);
    CHECK(all_transparent(frame, 32, 32, 32 * 4),
          "element-count mismatch rejects the cache");

    borrowed.element_count = 1;
    view.cache_view = &borrowed;
    overlay_resize(&view, 64, 64);
    overlay_render(&view, frame64, 64 * 4);
    CHECK(all_transparent(frame64, 64, 64, 64 * 4),
          "borrowed cache rejects a resized overlay");

    overlay_destroy(&view);
    CHECK(borrowed.pixels != NULL && borrowed.pixels[0] == 255,
          "destroy does not free a borrowed cache");

    free(borrowed.pixels);
    overlay_destroy(&overlay);

    CHECK(test_visual_state_equality() == 0,
          "visual-state equality contract");
    CHECK(test_publish_cadence() == 0, "publish-cadence contract");
    CHECK(test_rotation_blit() == 0, "rotation blit contract");
    CHECK(test_pixel_hash() == 0, "pixel hash contract");

    printf("overlay OK (cached compositing only)\n");
    return 0;
}
