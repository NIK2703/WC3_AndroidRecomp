/*
 * dxvk_probe_cache_real_stub.c - stand-in for the as-built physical-device
 * probe.  It lives in a separate translation unit so the test's call remains
 * an undefined reference and the linker --wrap option is exercised exactly as
 * it is for dxvk_entry.cpp in the product.
 */
#include <stddef.h>

unsigned g_probe_calls;

int wc3_vulkan_major_minor(int *major, int *minor)
{
    g_probe_calls++;
    if (major) *major = 3;
    if (minor) *minor = 1;
    return 1;
}
