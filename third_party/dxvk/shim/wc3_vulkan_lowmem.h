#pragma once

/*
 * DXVK 1.10.3 bundles Vulkan 1.3.217 headers, which predate VK_KHR_map_memory2
 * and VK_EXT_map_memory_placed.  The Android build passes this shim directory
 * to every DXVK object, so the low-memory path can name the few structures it
 * needs without replacing the whole vendored header set.
 *
 * Include this after <vulkan/vulkan.h>.  The newer 3.1.1 tree has the real
 * definitions and never includes this file.
 */

#ifndef VK_KHR_MAP_MEMORY_2_EXTENSION_NAME

#define VK_KHR_map_memory2 1
#define VK_KHR_MAP_MEMORY_2_SPEC_VERSION 1
#define VK_KHR_MAP_MEMORY_2_EXTENSION_NAME "VK_KHR_map_memory2"

#define VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR ((VkStructureType)1000271000)

typedef struct VkMemoryMapInfoKHR {
    VkStructureType     sType;
    const void*         pNext;
    VkMemoryMapFlags    flags;
    VkDeviceMemory      memory;
    VkDeviceSize        offset;
    VkDeviceSize        size;
} VkMemoryMapInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkMapMemory2KHR)(
    VkDevice device, const VkMemoryMapInfoKHR* pMemoryMapInfo, void** ppData);

#endif /* VK_KHR_MAP_MEMORY_2_EXTENSION_NAME */


#ifndef VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME

#define VK_EXT_map_memory_placed 1
#define VK_EXT_MAP_MEMORY_PLACED_SPEC_VERSION 1
#define VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME "VK_EXT_map_memory_placed"

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT ((VkStructureType)1000272001)
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT ((VkStructureType)1000272002)

/* Placed mappings must pass this flag in VkMemoryMapInfoKHR::flags. */
#define VK_MEMORY_MAP_PLACED_BIT_EXT 0x00000001

typedef struct VkPhysicalDeviceMapMemoryPlacedFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           memoryMapPlaced;
    VkBool32           memoryMapRangePlaced;
    VkBool32           memoryUnmapReserve;
} VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;

typedef struct VkPhysicalDeviceMapMemoryPlacedPropertiesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkDeviceSize       minPlacedMemoryMapAlignment;
} VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;

typedef struct VkMemoryMapPlacedInfoEXT {
    VkStructureType    sType;
    const void*        pNext;
    void*              pPlacedAddress;
} VkMemoryMapPlacedInfoEXT;

#endif /* VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME */
