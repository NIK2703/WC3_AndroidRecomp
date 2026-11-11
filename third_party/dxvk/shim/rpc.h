/* rpc.h - DXVK includes it through objbase.h on Windows for the UUID
 * helpers.  Nothing in the Android build uses RPC, so the header exists to
 * satisfy the include graph. */
#pragma once
#include "windows.h"
