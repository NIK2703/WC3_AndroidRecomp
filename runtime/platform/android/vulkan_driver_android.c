/*
 * vulkan_driver_android.c - opt-in ADPkg ICD loading through libadrenotools.
 *
 * System Vulkan is the default.  A package is opened only when the user
 * selected one, and only at the first DXVK instance.  Any package/load/probe
 * failure is one ICD-level fallback to the system loader: the selected id
 * stays visible, active_id() reports the driver that actually runs, and
 * last_error() carries a product-visible 4xxx code.
 */
#define _GNU_SOURCE 1

#include "vulkan_driver_android.h"
#include "vulkan_driver_package.h"
#include "wc3_debug.h"

#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

/* libadrenotools API; the header is not part of the public NDK. */
extern void *adrenotools_open_libvulkan(
    int dlopenMode, int featureFlags, const char *tmpLibDir,
    const char *hookLibDir, const char *customDriverDir,
    const char *customDriverName, const char *fileRedirectDir,
    void **userMappingHandle);

enum { ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0 };

#define DRV_LOG_TAG "GPU_DRIVERS"
#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
#define DRV_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, DRV_LOG_TAG, __VA_ARGS__)
#else
#define DRV_LOG(...) ((void)0)
#endif

typedef struct DriverState {
    pthread_mutex_t lock;
    char root[WC3_DRIVER_DIR_MAX];
    char hook_dir[WC3_DRIVER_DIR_MAX];
    char selected[WC3_VULKAN_DRIVER_ID_MAX];
    char active[WC3_VULKAN_DRIVER_ID_MAX];
    int attempted;
    int error;
    void *handle;
    PFN_vkGetInstanceProcAddr gipa;
} DriverState;

static DriverState g_driver;
static pthread_once_t g_driver_once = PTHREAD_ONCE_INIT;

static int wc3_android_vulkan_driver_select(void *ctx, int driver);
static int wc3_android_vulkan_driver_instance_version(void *ctx,
                                                      uint32_t *version);
static int wc3_android_vulkan_driver_device_caps(void *ctx,
                                                  uint32_t device_index,
                                                  Wc3VulkanCaps *caps);
static int wc3_android_vulkan_driver_select_id(void *ctx, const char *id);
static void *wc3_android_vulkan_driver_proc(void *ctx);
static int wc3_android_vulkan_driver_active(void *ctx, char *dst,
                                             size_t cap);
static int wc3_android_vulkan_driver_error(void *ctx);

static void driver_state_init(void)
{
    pthread_mutex_init(&g_driver.lock, NULL);
}

static int copy_string(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0u) return 0;
    n = src ? strlen(src) : 0u;
    if (n + 1u > cap) return 0;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return 1;
}

static PFN_vkGetInstanceProcAddr system_gipa(void);
static void system_loader_init(void);

static pthread_once_t g_system_once = PTHREAD_ONCE_INIT;
static void *g_system_handle;
static PFN_vkGetInstanceProcAddr g_system_gipa;

static void system_loader_init(void)
{
    g_system_handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_system_handle)
        g_system_handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_system_handle)
        g_system_gipa = (PFN_vkGetInstanceProcAddr)dlsym(
            g_system_handle, "vkGetInstanceProcAddr");
    DRV_LOG("system loader handle=%p gipa=%p", g_system_handle,
            (void *)g_system_gipa);
}

static PFN_vkGetInstanceProcAddr system_gipa(void)
{
    pthread_once(&g_system_once, system_loader_init);
    return g_system_gipa;
}

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
#define CHECK_REQUIRED(cond, name)                                            \
    do {                                                                      \
        if (!(cond)) {                                                        \
            DRV_LOG("missing required feature: %s", name);                    \
            missing++;                                                        \
        }                                                                     \
    } while (0)

/* DXVK 3.1.1's device filter rejects the adapter if any of these features is
 * false.  Keep this list aligned with dxvk_device_info.cpp; it exists only to
 * turn a silent "No adapters found" into a named diagnostic. */
static void log_missing_required_features(
    VkPhysicalDevice device, PFN_vkGetPhysicalDeviceFeatures2 get_features2)
{
    VkPhysicalDeviceFeatures2 f2 = { 0 };
    VkPhysicalDeviceVulkan11Features v11 = { 0 };
    VkPhysicalDeviceVulkan12Features v12 = { 0 };
    VkPhysicalDeviceVulkan13Features v13 = { 0 };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip = { 0 };
    VkPhysicalDeviceRobustness2FeaturesEXT robust2 = { 0 };
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = { 0 };
    VkPhysicalDeviceMaintenance6FeaturesKHR m6 = { 0 };
    uint32_t missing = 0;

    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    depth_clip.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
    robust2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    m5.sType = (VkStructureType)1000470000;
    m6.sType = (VkStructureType)1000545000;

    f2.pNext = &v11;
    v11.pNext = &v12;
    v12.pNext = &v13;
    v13.pNext = &depth_clip;
    depth_clip.pNext = &robust2;
    robust2.pNext = &m5;
    m5.pNext = &m6;
    get_features2(device, &f2);

    CHECK_REQUIRED(f2.features.depthBiasClamp, "depthBiasClamp");
    CHECK_REQUIRED(f2.features.depthClamp, "depthClamp");
    CHECK_REQUIRED(f2.features.dualSrcBlend, "dualSrcBlend");
    CHECK_REQUIRED(f2.features.fillModeNonSolid, "fillModeNonSolid");
    CHECK_REQUIRED(f2.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics");
    CHECK_REQUIRED(f2.features.fullDrawIndexUint32, "fullDrawIndexUint32");
    CHECK_REQUIRED(f2.features.geometryShader, "geometryShader");
    CHECK_REQUIRED(f2.features.imageCubeArray, "imageCubeArray");
    CHECK_REQUIRED(f2.features.independentBlend, "independentBlend");
    CHECK_REQUIRED(f2.features.multiDrawIndirect, "multiDrawIndirect");
    CHECK_REQUIRED(f2.features.multiViewport, "multiViewport");
    CHECK_REQUIRED(f2.features.occlusionQueryPrecise, "occlusionQueryPrecise");
    CHECK_REQUIRED(f2.features.robustBufferAccess, "robustBufferAccess");
    CHECK_REQUIRED(f2.features.sampleRateShading, "sampleRateShading");
    CHECK_REQUIRED(f2.features.samplerAnisotropy, "samplerAnisotropy");
    CHECK_REQUIRED(f2.features.shaderClipDistance, "shaderClipDistance");
    CHECK_REQUIRED(f2.features.shaderCullDistance, "shaderCullDistance");
    CHECK_REQUIRED(f2.features.shaderImageGatherExtended, "shaderImageGatherExtended");
    CHECK_REQUIRED(f2.features.shaderInt16, "shaderInt16");
    CHECK_REQUIRED(f2.features.shaderInt64, "shaderInt64");
    CHECK_REQUIRED(f2.features.shaderSampledImageArrayDynamicIndexing,
                   "shaderSampledImageArrayDynamicIndexing");
    CHECK_REQUIRED(f2.features.textureCompressionBC, "textureCompressionBC");

    CHECK_REQUIRED(v11.shaderDrawParameters, "shaderDrawParameters");
    CHECK_REQUIRED(v11.storageBuffer16BitAccess, "storageBuffer16BitAccess");

    CHECK_REQUIRED(v12.bufferDeviceAddress, "bufferDeviceAddress");
    CHECK_REQUIRED(v12.descriptorIndexing, "descriptorIndexing");
    CHECK_REQUIRED(v12.storageBuffer8BitAccess, "storageBuffer8BitAccess");
    CHECK_REQUIRED(v12.descriptorBindingSampledImageUpdateAfterBind,
                   "descriptorBindingSampledImageUpdateAfterBind");
    CHECK_REQUIRED(v12.descriptorBindingUpdateUnusedWhilePending,
                   "descriptorBindingUpdateUnusedWhilePending");
    CHECK_REQUIRED(v12.descriptorBindingPartiallyBound,
                   "descriptorBindingPartiallyBound");
    CHECK_REQUIRED(v12.hostQueryReset, "hostQueryReset");
    CHECK_REQUIRED(v12.runtimeDescriptorArray, "runtimeDescriptorArray");
    CHECK_REQUIRED(v12.samplerMirrorClampToEdge, "samplerMirrorClampToEdge");
    CHECK_REQUIRED(v12.scalarBlockLayout, "scalarBlockLayout");
    CHECK_REQUIRED(v12.shaderInt8, "shaderInt8");
    CHECK_REQUIRED(v12.timelineSemaphore, "timelineSemaphore");
    CHECK_REQUIRED(v12.uniformBufferStandardLayout, "uniformBufferStandardLayout");
    CHECK_REQUIRED(v12.vulkanMemoryModel, "vulkanMemoryModel");

    CHECK_REQUIRED(v13.inlineUniformBlock, "inlineUniformBlock");
    CHECK_REQUIRED(v13.computeFullSubgroups, "computeFullSubgroups");
    CHECK_REQUIRED(v13.dynamicRendering, "dynamicRendering");
    CHECK_REQUIRED(v13.maintenance4, "maintenance4");
    CHECK_REQUIRED(v13.shaderDemoteToHelperInvocation,
                   "shaderDemoteToHelperInvocation");
    CHECK_REQUIRED(v13.shaderZeroInitializeWorkgroupMemory,
                   "shaderZeroInitializeWorkgroupMemory");
    CHECK_REQUIRED(v13.subgroupSizeControl, "subgroupSizeControl");
    CHECK_REQUIRED(v13.synchronization2, "synchronization2");

    CHECK_REQUIRED(depth_clip.depthClipEnable, "depthClipEnable");
    CHECK_REQUIRED(robust2.robustBufferAccess2, "robustBufferAccess2");
    CHECK_REQUIRED(robust2.nullDescriptor, "nullDescriptor");
    CHECK_REQUIRED(m5.maintenance5, "maintenance5");
    CHECK_REQUIRED(m6.maintenance6, "maintenance6");

    {
        VkPhysicalDevice8BitStorageFeatures storage8 = { 0 };
        VkPhysicalDeviceFeatures2 f8 = { 0 };
        storage8.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
        f8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f8.pNext = &storage8;
        get_features2(device, &f8);
        DRV_LOG("8bit storage: buffer=%u uniform=%u push=%u",
                storage8.storageBuffer8BitAccess,
                storage8.uniformAndStorageBuffer8BitAccess,
                storage8.storagePushConstant8);
    }
    DRV_LOG("required feature check missing=%u", missing);
}
#else
static void log_missing_required_features(
    VkPhysicalDevice device, PFN_vkGetPhysicalDeviceFeatures2 get_features2)
{
    (void)device;
    (void)get_features2;
}
#endif

/* A throw-away instance is the only trustworthy source of the physical
 * device version.  The platform loader can report 1.4 while the Adreno
 * device is 1.1, which would send DXVK 3.1.1 down a path it cannot finish. */
static int probe_with_gipa(PFN_vkGetInstanceProcAddr gipa,
                           uint32_t device_index, Wc3VulkanCaps *caps,
                           uint32_t *api_version)
{
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkEnumeratePhysicalDevices enumerate_phys;
    PFN_vkGetPhysicalDeviceProperties get_props;
    PFN_vkEnumerateDeviceExtensionProperties enum_ext;
    PFN_vkGetPhysicalDeviceFeatures2 get_features2;
    VkApplicationInfo app;
    VkInstanceCreateInfo create_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice devices[8];
    VkPhysicalDeviceProperties props;
    uint32_t count = 0;
    uint32_t ext_count = 0;
    VkResult vr;
    int result = -1;

    if (!gipa) return -1;

    DRV_LOG("probe gipa=%p device_index=%u", (void *)gipa, device_index);
    create_instance = (PFN_vkCreateInstance)gipa(
        VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) return -1;

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wc3-recomp";
    app.applicationVersion = 1;
    app.pEngineName = "wc3";
    app.engineVersion = 1;
    /* Match DXVK 3.1.1's instance API so the Vulkan 1.2/1.3 feature chains
     * are queried under the same conditions.  Older devices fall back to the
     * 1.1 instance that DXVK 1.10.3 requires. */
    app.apiVersion = VK_API_VERSION_1_3;

    memset(&create_info, 0, sizeof create_info);
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app;

    vr = create_instance(&create_info, NULL, &instance);
    if (vr == VK_ERROR_INCOMPATIBLE_DRIVER) {
        DRV_LOG("probe 1.3 instance rejected; retry 1.1");
        app.apiVersion = VK_API_VERSION_1_1;
        vr = create_instance(&create_info, NULL, &instance);
    }
    if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        DRV_LOG("probe createInstance failed result=%d instance=%p",
                (int)vr, (void *)instance);
        return -1;
    }
    DRV_LOG("probe instance=%p", (void *)instance);

    destroy_instance = (PFN_vkDestroyInstance)gipa(
        instance, "vkDestroyInstance");
    enumerate_phys = (PFN_vkEnumeratePhysicalDevices)gipa(
        instance, "vkEnumeratePhysicalDevices");
    get_props = (PFN_vkGetPhysicalDeviceProperties)gipa(
        instance, "vkGetPhysicalDeviceProperties");
    enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)gipa(
        instance, "vkEnumerateDeviceExtensionProperties");
    get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(
        instance, "vkGetPhysicalDeviceFeatures2");
    DRV_LOG("probe features2=%p", (void *)get_features2);

    if (!destroy_instance || !enumerate_phys || !get_props)
        goto cleanup;

    vr = enumerate_phys(instance, &count, NULL);
    if ((vr != VK_SUCCESS && vr != VK_INCOMPLETE) || count == 0u)
        goto cleanup;
    DRV_LOG("probe physical devices=%u", count);
    if (device_index >= count)
        goto cleanup;
    if (count > (uint32_t)(sizeof devices / sizeof devices[0]))
        count = (uint32_t)(sizeof devices / sizeof devices[0]);
    vr = enumerate_phys(instance, &count, devices);
    if ((vr != VK_SUCCESS && vr != VK_INCOMPLETE) || device_index >= count)
        goto cleanup;

    memset(&props, 0, sizeof props);
    get_props(devices[device_index], &props);

#if WC3_DEBUG && WC3_DEBUG_GPU_DRIVERS
    if (get_features2 && props.apiVersion >= VK_API_VERSION_1_3)
        log_missing_required_features(devices[device_index], get_features2);
#endif

    if (api_version) *api_version = props.apiVersion;
    if (caps) {
        memset(caps, 0, sizeof *caps);
        caps->api_version = props.apiVersion;
        caps->driver_version = props.driverVersion;
        caps->vendor_id = props.vendorID;
        caps->device_id = props.deviceID;
        if (enum_ext &&
            enum_ext(devices[device_index], NULL, &ext_count, NULL) ==
                VK_SUCCESS)
            caps->extension_count = ext_count;
    }
    DRV_LOG("physical device %u.%u (%s) driver=0x%08x vendor=0x%04x device=0x%04x",
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), props.deviceName,
            props.driverVersion, props.vendorID, props.deviceID);
    result = 0;

cleanup:
    if (destroy_instance)
        destroy_instance(instance, NULL);
    return result;
}

/* One ICD-level fallback: on any package/load/probe failure both DXVK copies
 * receive the system loader's vkGetInstanceProcAddr.  DXVK 3.1.1 starts with a
 * null internal pointer on Android, so returning NULL here would leave an
 * API 1.4 system device unable to use the newer major. */
static PFN_vkGetInstanceProcAddr fallback_gipa_locked(void)
{
    g_driver.gipa = system_gipa();
    return g_driver.gipa;
}

/* Called with g_driver.lock held. */
static PFN_vkGetInstanceProcAddr open_selected_locked(void)
{
    Wc3DriverPackage pkg;
    char lib_path[WC3_DRIVER_DIR_MAX];
    char driver_dir[WC3_DRIVER_DIR_MAX];
    char hook_main[WC3_DRIVER_DIR_MAX];
    char hook_impl[WC3_DRIVER_DIR_MAX];
    int rc;

    if (g_driver.attempted) return g_driver.gipa;
    g_driver.attempted = 1;

    DRV_LOG("open selected='%s' root='%s' hooks='%s'", g_driver.selected,
            g_driver.root, g_driver.hook_dir);

    if (g_driver.selected[0] == '\0') {
        g_driver.active[0] = '\0';
        g_driver.error = WC3_DRIVER_OK;
        DRV_LOG("no package selected; system loader");
        return fallback_gipa_locked();
    }
    if (android_get_device_api_level() < 28) {
        g_driver.error = WC3_DRIVER_ERR_API;
        DRV_LOG("libadrenotools requires API 28 (device=%d)",
                android_get_device_api_level());
        return fallback_gipa_locked();
    }

    rc = wc3_driver_registry_get(g_driver.root, g_driver.selected, &pkg,
                                 lib_path, sizeof lib_path);
    if (rc != WC3_DRIVER_OK) {
        g_driver.error = rc;
        DRV_LOG("package %s rejected: %d", g_driver.selected, rc);
        return fallback_gipa_locked();
    }
    DRV_LOG("package %s library='%s' min_api=%u size=%u page16k=%d",
            g_driver.selected, pkg.library, pkg.min_api, pkg.size,
            pkg.page16k);
    if (pkg.min_api > (uint32_t)android_get_device_api_level()) {
        g_driver.error = WC3_DRIVER_ERR_API;
        DRV_LOG("package %s needs API %u", g_driver.selected, pkg.min_api);
        return fallback_gipa_locked();
    }
    if (g_driver.hook_dir[0] == '\0') {
        g_driver.error = WC3_DRIVER_ERR_LOAD;
        DRV_LOG("hook directory is empty");
        return fallback_gipa_locked();
    }
    if (snprintf(hook_main, sizeof hook_main, "%s/libmain_hook.so",
                 g_driver.hook_dir) <= 0 ||
        snprintf(hook_impl, sizeof hook_impl, "%s/libhook_impl.so",
                 g_driver.hook_dir) <= 0 ||
        access(hook_main, R_OK) != 0 || access(hook_impl, R_OK) != 0) {
        g_driver.error = WC3_DRIVER_ERR_LOAD;
        DRV_LOG("adrenotools hooks missing in %s (main=%d impl=%d)",
                g_driver.hook_dir, access(hook_main, R_OK),
                access(hook_impl, R_OK));
        return fallback_gipa_locked();
    }
    DRV_LOG("hooks main='%s' impl='%s'", hook_main, hook_impl);
    /* libadrenotools concatenates customDriverDir + customDriverName. */
    rc = snprintf(driver_dir, sizeof driver_dir, "%s/%s/", g_driver.root,
                  g_driver.selected);
    if (rc <= 0 || (size_t)rc >= sizeof driver_dir) {
        g_driver.error = WC3_DRIVER_ERR_PATH;
        return fallback_gipa_locked();
    }

    DRV_LOG("opening %s%s via %s", driver_dir, pkg.library,
            g_driver.hook_dir);
    g_driver.handle = adrenotools_open_libvulkan(
        RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, g_driver.hook_dir,
        driver_dir, pkg.library, NULL, NULL);
    DRV_LOG("adrenotools handle=%p", g_driver.handle);
    if (!g_driver.handle) {
        const char *why = dlerror();
        (void)why;
        g_driver.error = WC3_DRIVER_ERR_LOAD;
        DRV_LOG("adrenotools failed: %s", why ? why : "unknown");
        return fallback_gipa_locked();
    }

    g_driver.gipa = (PFN_vkGetInstanceProcAddr)dlsym(
        g_driver.handle, "vkGetInstanceProcAddr");
    DRV_LOG("custom gipa=%p", (void *)g_driver.gipa);
    if (!g_driver.gipa) {
        g_driver.error = WC3_DRIVER_ERR_LOAD;
        return fallback_gipa_locked();
    }
    if (probe_with_gipa(g_driver.gipa, 0, NULL, NULL) != 0) {
        g_driver.gipa = NULL;
        g_driver.error = WC3_DRIVER_ERR_PROBE;
        DRV_LOG("custom driver probe failed");
        return fallback_gipa_locked();
    }

    copy_string(g_driver.active, sizeof g_driver.active, g_driver.selected);
    g_driver.error = WC3_DRIVER_OK;
    DRV_LOG("active driver %s", g_driver.active);
    return g_driver.gipa;
}

void wc3_android_vulkan_driver_install(Wc3VulkanDriverApi *api)
{
    if (!api) return;
    DRV_LOG("install VulkanDriverApi=%p", (void *)api);
    memset(api, 0, sizeof *api);
    api->ctx = NULL;
    api->select = wc3_android_vulkan_driver_select;
    api->instance_version = wc3_android_vulkan_driver_instance_version;
    api->device_caps = wc3_android_vulkan_driver_device_caps;
    api->select_id = wc3_android_vulkan_driver_select_id;
    api->instance_proc_addr = wc3_android_vulkan_driver_proc;
    api->active_id = wc3_android_vulkan_driver_active;
    api->last_error = wc3_android_vulkan_driver_error;
}

int wc3_android_vulkan_driver_configure(const char *root, const char *id,
                                        const char *hook_dir)
{
    int rc = 0;
    DRV_LOG("configure root='%s' id='%s' hooks='%s'",
            root ? root : "(null)", id ? id : "(null)",
            hook_dir ? hook_dir : "(null)");
    pthread_once(&g_driver_once, driver_state_init);
    pthread_mutex_lock(&g_driver.lock);
    if (g_driver.attempted) {
        rc = -2;
        DRV_LOG("configure rejected: loader already attempted");
    } else if (!copy_string(g_driver.root, sizeof g_driver.root, root) ||
               !copy_string(g_driver.hook_dir, sizeof g_driver.hook_dir,
                            hook_dir)) {
        rc = -1;
        DRV_LOG("configure rejected: root/hook path too long");
    } else if (id && id[0] && !wc3_driver_id_valid(id)) {
        g_driver.error = WC3_DRIVER_ERR_ID;
        rc = -1;
        DRV_LOG("configure rejected: invalid id '%s'", id);
    } else {
        copy_string(g_driver.selected, sizeof g_driver.selected,
                    id ? id : "");
        g_driver.active[0] = '\0';
        g_driver.error = WC3_DRIVER_OK;
        DRV_LOG("configure selected='%s'", g_driver.selected);
    }
    pthread_mutex_unlock(&g_driver.lock);
    return rc;
}

static int wc3_android_vulkan_driver_select(void *ctx, int driver)
{
    (void)ctx;
    if (driver == WC3_VULKAN_DRIVER_SYSTEM)
        return wc3_android_vulkan_driver_select_id(NULL, "");
    /* The enum cannot address an installed package; only the legacy system
     * choice is representable here. */
    return -1;
}

static int wc3_android_vulkan_driver_select_id(void *ctx, const char *id)
{
    int rc = 0;
    (void)ctx;
    pthread_once(&g_driver_once, driver_state_init);
    pthread_mutex_lock(&g_driver.lock);
    if (g_driver.attempted) {
        rc = -1;
    } else if (!id || !id[0]) {
        g_driver.selected[0] = '\0';
        g_driver.active[0] = '\0';
    } else if (!wc3_driver_id_valid(id)) {
        g_driver.error = WC3_DRIVER_ERR_ID;
        rc = -1;
    } else if (!copy_string(g_driver.selected, sizeof g_driver.selected, id)) {
        g_driver.error = WC3_DRIVER_ERR_ID;
        rc = -1;
    }
    pthread_mutex_unlock(&g_driver.lock);
    return rc;
}

static void *wc3_android_vulkan_driver_proc(void *ctx)
{
    void *result;
    (void)ctx;
    pthread_once(&g_driver_once, driver_state_init);
    pthread_mutex_lock(&g_driver.lock);
    DRV_LOG("instance_proc_addr requested selected='%s' attempted=%d",
            g_driver.selected, g_driver.attempted);
    result = open_selected_locked();
    DRV_LOG("instance_proc_addr result=%p active='%s' error=%d", result,
            g_driver.active, g_driver.error);
    pthread_mutex_unlock(&g_driver.lock);
    return result;
}

static int wc3_android_vulkan_driver_instance_version(void *ctx,
                                                      uint32_t *version)
{
    PFN_vkGetInstanceProcAddr gipa;
    uint32_t api_version = 0;
    (void)ctx;
    if (!version) return -1;
    *version = 0;
    gipa = (PFN_vkGetInstanceProcAddr)wc3_android_vulkan_driver_proc(NULL);
    if (!gipa) gipa = system_gipa();
    if (!gipa) return -1;
    if (probe_with_gipa(gipa, 0, NULL, &api_version) != 0) return -1;
    *version = api_version;
    return 0;
}

static int wc3_android_vulkan_driver_device_caps(void *ctx,
                                                  uint32_t device_index,
                                                  Wc3VulkanCaps *caps)
{
    PFN_vkGetInstanceProcAddr gipa;
    (void)ctx;
    if (!caps) return -1;
    gipa = (PFN_vkGetInstanceProcAddr)wc3_android_vulkan_driver_proc(NULL);
    if (!gipa) gipa = system_gipa();
    if (!gipa) return -1;
    return probe_with_gipa(gipa, device_index, caps, NULL);
}

static int wc3_android_vulkan_driver_active(void *ctx, char *dst, size_t cap)
{
    (void)ctx;
    if (!dst || cap == 0u) return -1;
    pthread_once(&g_driver_once, driver_state_init);
    pthread_mutex_lock(&g_driver.lock);
    if (!copy_string(dst, cap, g_driver.active)) {
        pthread_mutex_unlock(&g_driver.lock);
        return -1;
    }
    pthread_mutex_unlock(&g_driver.lock);
    return 0;
}

static int wc3_android_vulkan_driver_error(void *ctx)
{
    int error;
    (void)ctx;
    pthread_once(&g_driver_once, driver_state_init);
    pthread_mutex_lock(&g_driver.lock);
    error = g_driver.error;
    pthread_mutex_unlock(&g_driver.lock);
    return error;
}
