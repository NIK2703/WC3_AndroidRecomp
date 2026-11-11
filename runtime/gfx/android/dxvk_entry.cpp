/*
 * dxvk_entry.cpp - thin bridge between the DXVK archives and Wc3Platform.
 *
 * Both isolated DXVK copies resolve Vulkan through their own internal
 * vkGetInstanceProcAddr pointer.  The app-layer VulkanDriverApi owns ICD
 * selection and the throw-away version probe; this file only forwards the
 * resulting pointer before either Direct3DCreate9 runs.
 */
#include "platform.h"
#include "wc3_debug.h"
#include "dxvk3_gate.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <stdlib.h>
#include <exception>
#include <string>
#include <typeinfo>
#include <vulkan/vulkan.h>

/* The DXVK archives export a C-linkage entry taking the D3D SDK UINT.  Only
 * the pointer type is needed here; including the Win32 D3D9 headers would pull
 * the Windows host surface into this target. */
struct IDirect3D9;

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
#include <android/log.h>
/* Keep the developer channel in run.txt as well as logcat.  The app redirects
 * stderr line-by-line, so a startup failure survives the crash handler even
 * when the log daemon is not readable from the device session. */
#define DX_LOG(...)                                                     \
    do {                                                                \
        __android_log_print(ANDROID_LOG_INFO, "GPU_DRIVERS", __VA_ARGS__); \
        fprintf(stderr, "[dxvk] " __VA_ARGS__);                         \
        fputc('\n', stderr);                                            \
        fflush(stderr);                                                 \
    } while (0)
#else
#define DX_LOG(...) ((void)0)
#endif

extern "C" void wc3_dxvk1_dxvk_set_vulkan_get_instance_proc_addr(void *fn);
extern "C" void wc3_dxvk3_dxvk_set_vulkan_get_instance_proc_addr(void *fn);

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
static int g_shim_log_count;
#endif

/*
 * DXVK 1.10.3 initialises its loader pointer from the undefined
 * vkGetInstanceProcAddr symbol.  We supply that symbol instead of linking
 * libvulkan.so, so the product has no DT_NEEDED libvulkan.  The shim is a
 * last-resort forwarder; wc3_dxvk_prepare() normally replaces the DXVK
 * pointer with the platform-selected loader.  Hidden visibility keeps this
 * compat symbol from interposing on the real libvulkan.so.
 */
extern "C" __attribute__((visibility("hidden")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    const Wc3Platform *platform = wc3_platform();
    PFN_vkGetInstanceProcAddr gipa;

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
    if (g_shim_log_count++ < 32)
        DX_LOG("shim vkGetInstanceProcAddr instance=%p name=%s platform=%p",
               (void *)instance, name ? name : "(null)",
               (const void *)platform);
#endif
    if (!platform || !platform->vulkan_driver.instance_proc_addr)
        return nullptr;
    gipa = (PFN_vkGetInstanceProcAddr)
        platform->vulkan_driver.instance_proc_addr(platform->vulkan_driver.ctx);
#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
    if (g_shim_log_count++ < 32)
        DX_LOG("shim gipa=%p", (void *)gipa);
#endif
    return gipa ? gipa(instance, name) : nullptr;
}

extern "C" void wc3_dxvk_prepare(void)
{
    const Wc3Platform *platform = wc3_platform();
    void *gipa;

    DX_LOG("prepare platform=%p proc=%p", (const void *)platform,
           platform ? (const void *)platform->vulkan_driver.instance_proc_addr
                    : nullptr);
    if (!platform || !platform->vulkan_driver.instance_proc_addr)
        return;
    gipa = platform->vulkan_driver.instance_proc_addr(
        platform->vulkan_driver.ctx);
    DX_LOG("prepare selected gipa=%p", gipa);
    if (!gipa)
        return; /* no Vulkan loader at all; DXVK will report its own error */
    wc3_dxvk1_dxvk_set_vulkan_get_instance_proc_addr(gipa);
    wc3_dxvk3_dxvk_set_vulkan_get_instance_proc_addr(gipa);
    DX_LOG("prepare set dxvk1/dxvk3 gipa=%p", gipa);
}

/* The R8 package is built from Mesa 5ac41be677.  Its descriptor-prefetch
 * pass can leave a NULL insertion anchor, and a release build crashes in
 * nir_instr_insert() from ir3_nir_opt_prefetch_descriptors.  Mesa fixed the
 * pass in 44b82c7576d (2026-06-19).  The R8 package predates that fix.
 *
 * Disabling only IR3_DBG_NODESCPREFETCH lets a forced DXVK 3.1.1 run on the
 * old package.  This is a driver-optimization workaround, not a DXVK version
 * fallback: the selected DXVK archive is still 3.1.1. */
static void enable_turnip_nodescprefetch(void)
{
    const char *old = getenv(WC3_TURNIP_SHADER_DEBUG_ENV);

    if (!old || !old[0]) {
        setenv(WC3_TURNIP_SHADER_DEBUG_ENV, WC3_TURNIP_NODESCPREFETCH, 1);
        return;
    }
    if (strstr(old, WC3_TURNIP_NODESCPREFETCH))
        return;

    std::string value(old);
    value += ',';
    value += WC3_TURNIP_NODESCPREFETCH;
    setenv(WC3_TURNIP_SHADER_DEBUG_ENV, value.c_str(), 1);
}

/* The settings screen owns this value.  It is latched into the environment
 * before android_main, so the as-built thin bridge can treat it as its forced
 * selector without a second JNI path.  Match the bridge's first-character
 * parser so the wrapper suppresses the same fallback the bridge would take;
 * the settings screen writes the canonical "1"/"3", and anything else keeps
 * automatic selection. */
static int forced_dxvk_major()
{
    const char *value = getenv("WC3_DXVK_VERSION");
    if (!value) return 0;
    if (value[0] == '1') return 1;
    if (value[0] == '3') return 3;
    return 0;
}

/* The DXVK major selector calls this through --wrap=wc3_vulkan_major_minor.
 * The physical device version is authoritative; a loader-only 1.4 must not
 * send an Adreno 1.1 device to DXVK 3.1.1. */
extern "C" int wc3_vulkan_major_minor(int *major, int *minor)
{
    const Wc3Platform *platform;
    Wc3VulkanCaps caps;
    char active[WC3_VULKAN_DRIVER_ID_MAX];
    int have_caps = 0;
    uint32_t version = 0;
    int forced;
    int maj;
    int min;

    if (major) *major = 0;
    if (minor) *minor = 0;

    forced = forced_dxvk_major();
    if (forced)
        DX_LOG("DXVK %s forced by launch settings; compatibility gate "
               "bypassed", forced == 1 ? "1.10.3" : "3.1.1");

    platform = wc3_platform();
    DX_LOG("major_minor platform=%p proc=%p", (const void *)platform,
           platform ? (const void *)platform->vulkan_driver.instance_version
                    : nullptr);
    if (!platform || !platform->vulkan_driver.instance_version) {
        DX_LOG("major_minor no platform probe");
        return 0;
    }
    if (platform->vulkan_driver.instance_version(
            platform->vulkan_driver.ctx, &version) != 0 ||
        version == 0) {
        DX_LOG("major_minor probe failed version=0x%08x", version);
        return 0;
    }

    /* Vulkan packs major:minor:patch as 22:12:12. */
    maj = (int)((version >> 22) & 0x7fu);
    min = (int)((version >> 12) & 0x3ffu);

    if (platform->vulkan_driver.device_caps &&
        platform->vulkan_driver.device_caps(
            platform->vulkan_driver.ctx, 0, &caps) == 0) {
        have_caps = 1;
        DX_LOG("driver version 0x%08x", caps.driver_version);
    }
    active[0] = '\0';
    if (platform->vulkan_driver.active_id)
        platform->vulkan_driver.active_id(
            platform->vulkan_driver.ctx, active, sizeof active);
    if (wc3_dxvk3_package_needs_nodescprefetch(
            active, have_caps, have_caps ? caps.driver_version : 0)) {
        if (forced == 3) {
            enable_turnip_nodescprefetch();
            DX_LOG("driver package '%s' has the ir3 descriptor-prefetch bug; "
                   "IR3_SHADER_DEBUG=nodescprefetch enabled for forced "
                   "DXVK 3.1.1", active);
        } else {
            DX_LOG("driver package '%s' is older than Turnip 26.0.0; "
                   "selecting DXVK 1.10.3", active);
        }
        maj = 1;
        min = 2;
    }

    if (major) *major = maj;
    if (minor) *minor = min;
    if (forced) {
        DX_LOG("Vulkan report %d.%d (DXVK %s forced; automatic would choose %s)",
               maj, min, forced == 1 ? "1.10.3" : "3.1.1",
               (maj == 1 && min < 3) ? "1.10.3" : "3.1.1");
    } else {
        DX_LOG("Vulkan report %d.%d (DXVK %s)", maj, min,
               (maj == 1 && min < 3) ? "1.10.3" : "3.1.1");
    }
    return 1;
}

/*
 * The as-built thin bridge selects a DXVK major before it calls one of these
 * entry points.  Log both calls and swallow the version's own exception so the
 * fallback path is visible instead of being mistaken for "3.1.1 was never
 * selected".  CMake applies --wrap to the archive symbols.
 */
extern "C" IDirect3D9 *__real_wc3_dxvk1_Direct3DCreate9(unsigned int sdk);
extern "C" IDirect3D9 *__real_wc3_dxvk3_Direct3DCreate9(unsigned int sdk);

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
/*
 * DXVK reports setup failures by throwing dxvk::DxvkError.  That type does not
 * derive from std::exception; it is an ordinary object whose first and only
 * member is a std::string.  The isolated archive prefixes the symbol that
 * defines the type, and the NDK libc++ compares typeinfo by pointer, so a
 * normal catch (const dxvk::DxvkError&) cannot match.  The archive still
 * exports the accessor under its prefixed mangled name; call it on the current
 * exception object instead.
 */
extern "C" const void *
wc3_dxvk3__ZNK4dxvk9DxvkError7messageEv(const void *self);

static void log_current_exception(const char *where)
{
    std::type_info *type = __cxxabiv1::__cxa_current_exception_type();
    const char *name = type ? type->name() : "(unknown)";
    int status = 0;
    char *demangled = type
        ? __cxxabiv1::__cxa_demangle(name, nullptr, nullptr, &status)
        : nullptr;
    void *primary = __cxxabiv1::__cxa_current_primary_exception();

    DX_LOG("%s type=%s primary=%p", where, demangled ? demangled : name,
           primary);
    if (primary && strstr(demangled ? demangled : name, "DxvkError")) {
        const std::string *message = static_cast<const std::string *>(
            wc3_dxvk3__ZNK4dxvk9DxvkError7messageEv(primary));
        DX_LOG("%s message=%s", where,
               message ? message->c_str() : "(null)");
    }
    if (primary)
        __cxxabiv1::__cxa_decrement_exception_refcount(primary);
    std::free(demangled);
}
#else
static void log_current_exception(const char *) {}
#endif

extern "C" IDirect3D9 *__wrap_wc3_dxvk1_Direct3DCreate9(unsigned int sdk)
{
    /* A forced 3.1.1 attempt must not silently become a 1.10.3 run when the
     * selected backend throws or returns NULL.  The as-built bridge calls this
     * entry for its 3.1.1 -> 1.10.3 fallback; suppressing the wrapped result
     * keeps the user's choice and turns the failure into an explicit startup
     * failure instead of a different DXVK version. */
    if (forced_dxvk_major() == 3) {
        DX_LOG("DXVK 1.10.3 entry suppressed: DXVK 3.1.1 is forced");
        return nullptr;
    }
    DX_LOG("DXVK 1.10.3 entry sdk=%u", sdk);
    IDirect3D9 *d3d = __real_wc3_dxvk1_Direct3DCreate9(sdk);
    DX_LOG("DXVK 1.10.3 result=%p", (void *)d3d);
    return d3d;
}

extern "C" IDirect3D9 *__wrap_wc3_dxvk3_Direct3DCreate9(unsigned int sdk)
{
    DX_LOG("DXVK 3.1.1 entry sdk=%u", sdk);
    try {
        IDirect3D9 *d3d = __real_wc3_dxvk3_Direct3DCreate9(sdk);
        DX_LOG("DXVK 3.1.1 result=%p", (void *)d3d);
        return d3d;
    } catch (const std::exception &e) {
        DX_LOG("DXVK 3.1.1 threw std::exception: %s", e.what());
        return nullptr;
    } catch (...) {
        log_current_exception("DXVK 3.1.1 threw");
        return nullptr;
    }
}
