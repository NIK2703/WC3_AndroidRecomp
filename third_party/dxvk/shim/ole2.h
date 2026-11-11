/* ole2.h - the Windows build pulls it in through objbase.h.  The Android
 * build has no OLE; only the include needs to resolve. */
#pragma once
#include "windows.h"
#include "unknwn.h"
