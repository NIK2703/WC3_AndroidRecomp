#include "wc3_overlay_android.h"

int wc3_overlay_android_configure(ANativeWindow *window, int width, int height) {
    if (!window || width <= 0 || height <= 0) return -1;
    return ANativeWindow_setBuffersGeometry(window, width, height,
                                            WINDOW_FORMAT_RGBA_8888);
}

int wc3_overlay_android_present(ANativeWindow *window, Wc3Overlay *overlay) {
    if (!window || !overlay) return -1;

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) return -1;
    if (!buffer.bits || buffer.stride <= 0) {
        ANativeWindow_unlockAndPost(window);
        return -1;
    }

    if (buffer.width != overlay->width || buffer.height != overlay->height)
        wc3_overlay_resize(overlay, buffer.width, buffer.height);

    wc3_overlay_render(overlay, (uint8_t *)buffer.bits,
                       buffer.stride * 4);
    ANativeWindow_unlockAndPost(window);
    return 0;
}
