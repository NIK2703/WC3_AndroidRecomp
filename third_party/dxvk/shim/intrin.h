/* Android arm64 build: DXVK reaches <intrin.h> when _MSC_VER is defined.
 * It is not, but the header must exist for the include to resolve. */
#pragma once
#include "x86intrin.h"
