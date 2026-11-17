#include "guest_host.h"

/* Identity mode by default.  runtime/platform/android/guest_window_android.c
 * sets this once during boot, before any guest mapping is installed. */
uintptr_t g_guest_host_base = 0;
