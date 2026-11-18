#ifndef WC3_PRESENT_TARGET_H
#define WC3_PRESENT_TARGET_H

#include <android/native_window.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Private present target for the presentation pass.
 *
 * DXVK must render at the guest display mode and not at the screen size.  The
 * Android WSI always asks its ANativeWindow for a size and DXVK always takes
 * the swap chain extent from the surface capabilities, so DXVK is handed a
 * window of our own: an AImageReader producer surface created at the guest
 * extent.  The real SurfaceView window stays the monitor and the consumer of
 * the final, upscaled image.
 *
 * The target exists only when presentation needs it.  A half-resolution grid
 * run needs the reader and screen pass for its exact 2x nearest image.  A
 * lower guest mode outside the grid uses the real window with guest-sized
 * buffer geometry and lets the compositor scale.  Native retires both paths.
 * Screen present mode and buffering apply to DXVK's real-window swap chain,
 * not to a forced 1:1 pass.  Retirement unpublishes immediately but keeps the
 * reader until DXVK destroys the swap chain that references it.
 *
 * What this module guarantees, and what it does not.
 *
 * Guaranteed: the guest extent published here is the client rect the Win32
 * shim reports, so DXVK's present rectangle equals the guest image
 * (d3d9_swapchain.cpp UpdatePresentRegion -> GetClientRect).  With
 * dstRect.extent == srcRect.extent, DxvkSwapchainBlitter::draw takes
 * sameSize -> m_fsCopy (dxvk_swapchain_blitter.cpp:47,51): the guest image is
 * written 1:1 at the origin of the swap chain image, texel fetch, no
 * filtering.  That is the pixel-exact input the present pass upscales.
 *
 * Not guaranteed: that the surface capabilities report the guest extent as
 * currentExtent.  That value depends on the driver's Android WSI and on what
 * ANativeWindow_getWidth/Height returns for an AImageReader surface, which is
 * not part of any contract we control.  The pass therefore never assumes the
 * swap chain is exactly guest-sized: it always blits the guest sub-rectangle
 * (0,0,guest) and works for any swap chain extent that contains it.  A probe
 * (see present_pass.cpp) rejects the target if it cannot contain the guest
 * image, and the shim then keeps the real window, so the game stays visible
 * through the ordinary stretched path.
 *
 * Resize protocol: a guest mode change replaces the producer through
 * start()/set_extent().  The extent of an AImageReader is fixed at creation and
 * its surface caps follow that extent, so the old reader is parked and a new
 * one is published at the new size.  DXVK sees the new window pointer and
 * rebuilds its presenter; the old reader is reaped after that swap chain is
 * destroyed.
 *
 * Threading: start/stop/ensure/set_extent/retire run on the guest display-mode
 * thread, drain runs on the guest render thread inside vkQueuePresentKHR, and
 * reap runs on that same render thread after the guest swap chain is destroyed.
 * The extent is published with release/acquire atomics; the reader lock
 * protects only the producer pointers and is never held across a Vulkan call, a
 * queue wait, a guest dispatch or a wait on a guest-visible object.  retire does
 * not delete anything, so it is safe while a frame is in flight; delete is
 * deferred to reap, which only runs after the referencing swap chain is gone.
 */

/* Create the producer at the guest extent and publish it to the DXVK shim.
 * Returns 0 on success, -errno on failure.  Idempotent at the same extent; a
 * different extent parks the old reader and publishes a new one. */
int wc3_present_target_start(uint32_t width, uint32_t height);

/* Follow the display mode.  A half-resolution grid target is started (or
 * resized) when the guest extent differs from the surface extent; grid off
 * below native publishes a direct real-window extent instead of a reader.
 * Native size retires both, because the 1:1 pass changes nothing.  Returns 0
 * on success (including "not needed"), -errno on failure. */
int wc3_present_target_ensure(uint32_t guest_w, uint32_t guest_h,
                              uint32_t surface_w, uint32_t surface_h);

/* Unpublish, drain and destroy.  Safe when not started. */
void wc3_present_target_stop(void);

/* Unpublish now but keep the reader alive: DXVK may still own a swap chain
 * that references its buffers.  The next destroy of that swap chain calls
 * wc3_present_target_reap.  Safe when not started. */
void wc3_present_target_retire(void);

/* Destroy readers retired by wc3_present_target_retire.  Called from the pass
 * after the guest swap chain is destroyed, and at device shutdown, so the
 * reader cannot be freed while Vulkan still uses its window. */
void wc3_present_target_reap(void);

/* 1 when a producer is published and frames are expected to arrive. */
int wc3_present_target_active(void);

/* 1 when grid off below native is presenting directly into the real window.
 * Not a producer: no reader and no screen swap chain exist on this path. */
int wc3_present_target_direct(void);

/* Current guest extent.  Returns 0 and leaves the outputs untouched when the
 * target is not active. */
int wc3_present_target_extent(uint32_t *width, uint32_t *height);

/* Surface extent last passed to ensure(): the size the mode layer derived the
 * guest mode from.  Stable across screen-swap-chain creation, unlike a live
 * ANativeWindow query.  Returns 0 before the first ensure(). */
int wc3_present_target_surface_extent(uint32_t *width, uint32_t *height);

/* Replace the producer at a new guest extent.  Returns 0 when the extent was
 * applied or was already current, -errno on failure.  A no-op for an inactive
 * target. */
int wc3_present_target_set_extent(uint32_t width, uint32_t height);

/* The producer window, for the capability probe of the present pass.  The
 * reference stays owned by this module. */
ANativeWindow *wc3_present_target_window(void);

/* Selects the half-resolution grid path (present_scale = half): a 2x nearest
 * upscale of an exactly half-sized guest image.  With 0 (the default) the
 * present pass never filters and leaves scaling to the compositor.  The flag
 * lives here rather than in the pass so that boot, HLE and the pass all read it
 * without a Vulkan header. */
void wc3_present_target_set_half_grid(int enabled);
int wc3_present_target_half_grid(void);

/* Drop every queued image without reading it.  Called from the present pass
 * every frame: an undrained producer blocks DXVK's queueBuffer once the reader
 * holds its last image.  Bounded work, no Vulkan calls. */
void wc3_present_target_drain(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PRESENT_TARGET_H */
