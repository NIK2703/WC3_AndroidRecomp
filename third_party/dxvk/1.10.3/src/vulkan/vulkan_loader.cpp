#include "vulkan_loader.h"

namespace dxvk::vk {

  /*
   * The Android build can replace the Vulkan loader entry point at runtime:
   * runtime/android/dxvk_entry.cpp opens libvulkan through libadrenotools,
   * which makes the system loader load a custom ICD (Turnip) instead of the
   * vendor driver.  Everything DXVK resolves goes through this pointer, so
   * setting it before Direct3DCreate9 is enough to move the whole device to
   * that ICD.  On the Windows host the default is the linked loader and the
   * setter is simply never called.
   */
  static PFN_vkGetInstanceProcAddr GetInstanceProcAddr = vkGetInstanceProcAddr;

  extern "C" void dxvk_set_vulkan_get_instance_proc_addr(PFN_vkGetInstanceProcAddr fn) {
    if (fn)
      GetInstanceProcAddr = fn;
  }

  PFN_vkVoidFunction LibraryLoader::sym(const char* name) const {
    return dxvk::vk::GetInstanceProcAddr(nullptr, name);
  }
  
  
  InstanceLoader::InstanceLoader(bool owned, VkInstance instance)
  : m_instance(instance), m_owned(owned) { }
  
  
  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return dxvk::vk::GetInstanceProcAddr(m_instance, name);
  }
  
  
  DeviceLoader::DeviceLoader(bool owned, VkInstance instance, VkDevice device)
  : m_getDeviceProcAddr(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      dxvk::vk::GetInstanceProcAddr(instance, "vkGetDeviceProcAddr"))),
    m_device(device), m_owned(owned) { }
  
  
  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    return m_getDeviceProcAddr(m_device, name);
  }
  
  
  LibraryFn::LibraryFn() { }
  LibraryFn::~LibraryFn() { }
  
  
  InstanceFn::InstanceFn(bool owned, VkInstance instance)
  : InstanceLoader(owned, instance) { }
  InstanceFn::~InstanceFn() {
    if (m_owned)
      this->vkDestroyInstance(m_instance, nullptr);
  }
  
  
  DeviceFn::DeviceFn(bool owned, VkInstance instance, VkDevice device)
  : DeviceLoader(owned, instance, device) { }
  DeviceFn::~DeviceFn() {
    if (m_owned)
      this->vkDestroyDevice(m_device, nullptr);
  }
  
}