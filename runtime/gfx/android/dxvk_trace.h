/*
 * dxvk_trace.h - debug-only Vulkan boundary tracer for DXVK.
 *
 * DXVK resolves every Vulkan entry through the vkGetInstanceProcAddr pointer
 * supplied by wc3_dxvk_prepare().  Wrapping that one pointer gives a view of
 * what DXVK asks the selected ICD to do without editing the vendored trees or
 * rebuilding them per variant.  The implementation is compiled only under
 * WC3_DEBUG && WC3_DEBUG_DXVK_DEBUG; release keeps a pass-through.
 */
#ifndef WC3_GFX_ANDROID_DXVK_TRACE_H
#define WC3_GFX_ANDROID_DXVK_TRACE_H

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The returned pointer is safe to hand to both DXVK copies.  When the trace
 * tag is off it is exactly real_gipa. */
PFN_vkGetInstanceProcAddr wc3_dxvk_trace_wrap_gipa(
    PFN_vkGetInstanceProcAddr real_gipa);

/* Label the trace with the selected archive ("1.10.3" or "3.1.1"). */
void wc3_dxvk_trace_set_version(const char *version);

/* Emit a trace line at a DXVK entry/exit point. */
void wc3_dxvk_trace_mark(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GFX_ANDROID_DXVK_TRACE_H */
