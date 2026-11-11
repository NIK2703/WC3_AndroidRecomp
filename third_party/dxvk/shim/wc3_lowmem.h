#pragma once

#include <stdint.h>

/*
 * The window itself is shared with the thin D3D9 bridge: its staging arena
 * must stay out of it so a placed mapping still finds a free gap.
 */
#define WC3_LOWMEM_BEGIN 0x01000000u
#define WC3_LOWMEM_END   0x31000000u

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Finds a free range below 4 GB for a Vulkan placed memory mapping.
 *
 * The guest is 32-bit and the thin D3D9 bridge can hand its pointer straight
 * to the guest only when DXVK's host-visible memory is mapped low.  The shim
 * owns the target-specific address-space knowledge; DXVK only asks for a
 * range and falls back to an ordinary vkMapMemory when this returns 0.
 *
 * `alignment` is the driver's minPlacedMemoryMapAlignment.  The returned
 * address is aligned to it and lies in [0x01000000, 0x31000000), the window
 * between the war3.exe image and the main guest stack that no guest arena
 * uses.  The range is unmapped on return, so the caller can place a mapping
 * there.
 */
uintptr_t wc3_lowmem_acquire(uint64_t size, uint64_t alignment);

#ifdef __cplusplus
}
#endif
