/*
 * init_array_probe.h - debug-only constructor counter (O-44).
 *
 * The two DXVK backends are vendor-owned and stay initialized eagerly until
 * GB-5/ADR-004/007.  This probe only observes the final .so's .init_array:
 * it counts entries at the first constructor and publishes the count to the
 * boot layer.  Release compiles the implementation and the call out.
 */
#ifndef WC3_BOOT_INIT_ARRAY_PROBE_H
#define WC3_BOOT_INIT_ARRAY_PROBE_H

#include <stdint.h>

#include "wc3_debug.h"

#if WC3_DEBUG && WC3_DEBUG_BOOT
uint32_t wc3_init_array_count(void);
#else
#define wc3_init_array_count() 0u
#endif

#endif /* WC3_BOOT_INIT_ARRAY_PROBE_H */
