#include "../dxvk_platform_exts.h"

namespace dxvk {

  DxvkPlatformExts DxvkPlatformExts::s_instance;

  std::string_view DxvkPlatformExts::getName() {
#ifdef __ANDROID__
    return "Android WSI";
#else
    return "Win32 WSI";
#endif
  }


  DxvkNameSet DxvkPlatformExts::getInstanceExtensions() {
    DxvkNameSet names;
#ifdef __ANDROID__
    /* Same role as VK_KHR_win32_surface on the Windows host: the presenter
     * turns the process's window into a VkSurfaceKHR through it. */
    names.add(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
    names.add(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
    return names;
  }


  DxvkNameSet DxvkPlatformExts::getDeviceExtensions(
          uint32_t      adapterId) {
    return DxvkNameSet();
  }


  void DxvkPlatformExts::initInstanceExtensions() {

  }


  void DxvkPlatformExts::initDeviceExtensions(
    const DxvkInstance* instance) {

  }

}