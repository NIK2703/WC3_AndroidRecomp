/*
 * hle_android_mouse_private.h - raw/legacy mouse entry points.
 *
 * Not part of the Native Input Gateway.  These exist only for the Win32
 * mouse_event imports and the old one-finger path in d3d9_thin.cpp.  Direct
 * touch must include native_input_gateway.h instead and never see this header.
 */
#ifndef WC3_HLE_ANDROID_MOUSE_PRIVATE_H
#define WC3_HLE_ANDROID_MOUSE_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Relative motion in client pixels (trackpad bindings). */
void hle_android_mouse_move_delta(int dx, int dy);
/* Legacy one-finger button and wheel imports. */
void hle_android_mouse_button(int down);
void hle_android_guest_mouse_button(int button, int down);
void hle_android_mouse_wheel(int delta);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_ANDROID_MOUSE_PRIVATE_H */
