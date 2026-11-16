#if defined(DXVK_WSI_WIN32)

#include "wsi_platform_win32.h"

namespace dxvk::wsi {

  std::vector<const char *> Win32WsiDriver::getInstanceExtensions() {
#ifdef __ANDROID__
    /* The guest's HWND is only a token; the presenter creates the surface
     * from the activity's ANativeWindow, so request the Android extension. */
    return { VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
#else
    return { VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
#endif
  }

  static bool createWin32WsiDriver(WsiDriver **driver) {
    *driver = new Win32WsiDriver();
    return true;
  }

  WsiBootstrap Win32WSI = {
    "Win32",
    createWin32WsiDriver
  };

}

#endif
