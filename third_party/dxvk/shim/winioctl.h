/*
 * winioctl.h - device I/O control codes, for the Android DXVK build.
 *
 * DXVK's util_shared_res.cpp talks to Wine's "\\\\.\\SharedGpuResource"
 * pseudo-device to pass a GPU resource handle between processes.  There is no
 * such device on Android and no cross-process sharing to do, so the file
 * exists only so that the CTL_CODE expressions and the CreateFile /
 * DeviceIoControl prototypes resolve.  Every call fails at runtime, which is
 * exactly the behaviour a caller expects when the feature is absent.
 */
#ifndef WC3_ANDROID_DXVK_WINIOCTL_H
#define WC3_ANDROID_DXVK_WINIOCTL_H

#include "windows.h"

/* Device types from the Windows DDK.  Only VIDEO is referenced by DXVK. */
#define FILE_DEVICE_BEEP                0x00000001u
#define FILE_DEVICE_UNKNOWN             0x00000022u
#define FILE_DEVICE_VIDEO               0x00000023u
#define FILE_DEVICE_MASS_STORAGE        0x0000002Du

/* Buffering methods. */
#define METHOD_BUFFERED                 0u
#define METHOD_IN_DIRECT                1u
#define METHOD_OUT_DIRECT               2u
#define METHOD_NEITHER                  3u

/* Access bits. */
#define FILE_ANY_ACCESS                 0u
#define FILE_READ_ACCESS                0x0001u
#define FILE_WRITE_ACCESS               0x0002u

/* The CTL_CODE macro is part of the contract, not an implementation detail:
 * the values it produces are what the driver dispatches on. */
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

/* CreateFileA dispositions and attributes.  DXVK's shared-resource code uses
 * OPEN_EXISTING and FILE_ATTRIBUTE_NORMAL; the rest are here because they are
 * the same enumeration and cost nothing. */
#ifndef CREATE_NEW
#define CREATE_NEW          1u
#define CREATE_ALWAYS       2u
#define OPEN_EXISTING       3u
#define OPEN_ALWAYS         4u
#define TRUNCATE_EXISTING   5u
#endif

#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL  0x00000080u
#endif

#ifndef GENERIC_READ
#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#endif

#endif /* WC3_ANDROID_DXVK_WINIOCTL_H */
