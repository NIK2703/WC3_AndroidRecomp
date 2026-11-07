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

    printf("overlay OK (cached compositing only)\n");
    return 0;
}
