#ifndef WC3_GFX_ANDROID_PRESENT_PASS_H
#define WC3_GFX_ANDROID_PRESENT_PASS_H

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Present pass: keeps DXVK at the game's resolution and puts the final scaling
 * where the decision says it belongs.
 *
 * DXVK is handed a producer surface instead of the SurfaceView window (see
 * present_target.h) and presents there, so its swap chain image is the game's
 * resolution and its own swapchain blitter copies the back buffer 1:1 instead
 * of resampling it (present_target.h explains why GetClientRect makes that
 * happen).  This pass wraps the Vulkan entry points DXVK resolves, adds
 * VK_IMAGE_USAGE_TRANSFER_SRC_BIT to its swap chain, and for every present:
 *
 *   1. moves the guest image into a swap chain image of the real window -
 *      either a 2x nearest-neighbour upscale (the half-resolution grid path,
 *      where every grid cell must be an exact 2x2 block) or a plain 1:1 copy
 *      into a canvas of the game's own size, which the compositor then scales
 *      to the view (every other case, including whole ratios);
 *   2. presents the guest image back to DXVK's producer so the WSI releases it
 *      and DXVK's own acquireNextImage keeps working;
 *   3. presents the screen image to the real window.
 *
 * All of it is GPU work: one command buffer with two layout transitions and a
 * single copy or blit per frame.  No pixel is read or written by the CPU, no
 * shader is compiled, no image goes through host memory, and no DXVK source is
 * edited.
 *
 * The pass is bypassed when it would be a pure 1:1 copy: no present target is
 * published at native resolution and DXVK presents straight into the real
 * window.  A target can be started or retired at runtime when the game changes
 * its display mode, so the wrapper is always installed and forwards to the
 * real entry points while the target is dormant.
 *
 * The wrapper is installed last in wc3_dxvk_prepare(), after the optional
 * boundary trace, so the trace still sees the guest's own calls.
 */

/* Always installs the wrapper; individual entry points forward while no
 * present target is active. */
PFN_vkGetInstanceProcAddr wc3_present_pass_wrap_gipa(
    PFN_vkGetInstanceProcAddr real_gipa);

/* 1 after the pass was activated on the device (capability probe accepted and
 * device objects exist).  It stays 1 while a target is retired, because the
 * objects are reused when the target starts again.  Debug and test hook. */
int wc3_present_pass_active(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GFX_ANDROID_PRESENT_PASS_H */
