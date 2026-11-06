/*
 * guest_layout.h - guest address space for the Android arm64 target.
 *
 * The Android values are the measured ones from the 1.27 port: ART reserves
 * 0x10980000-0x30980000 in an app process, so the anonymous arenas and the
 * worker-thread stack/TEB areas are placed around it.  The two image bases
 * (game.dll and the Storm/MSS support modules) are part of the lifted corpus
 * and must not be changed without re-lifting from a rebased PE.
 *
 * No Windows layout lives here.  The Windows host is a historical reference;
 * this project builds only the Android target.
 */
#ifndef WC3_GUEST_LAYOUT_H
#define WC3_GUEST_LAYOUT_H

#define GUEST_IMAGE_WAR3    0x00400000u
#define GUEST_IMAGE_GAME    0x6F000000u
#define GUEST_IMAGE_STORM   0x35000000u
#define GUEST_IMAGE_MSS     0x35100000u
#define GUEST_IMAGE_MSSFAST 0x35200000u
#define GUEST_IMAGE_MP3DEC  0x35300000u
#define GUEST_IMAGE_IJL     0x60000000u

#define GUEST_STACK_BASE    0x31000000u
#define GUEST_STACK_SIZE    0x00800000u

#define GUEST_HEAP_BASE     0x36000000u
#define GUEST_HEAP_SIZE     0x04000000u

#define GUEST_HLE_HEAP_BASE 0x3A000000u
#define GUEST_HLE_HEAP_SIZE 0x04000000u

#define GUEST_VA_BASE       0x61000000u
#define GUEST_VA_SIZE       0x04000000u
#define GUEST_VA2_BASE      0x40000000u
#define GUEST_VA2_SIZE      0x20000000u
#define GUEST_VA3_BASE      0x80000000u
#define GUEST_VA3_SIZE      0x10000000u
#define GUEST_VA4_BASE      0x70000000u
#define GUEST_VA4_SIZE      0x10000000u
#define GUEST_VA5_BASE      0xA1000000u
#define GUEST_VA5_SIZE      0x30000000u
#define GUEST_VA6_BASE      0xD9800000u
#define GUEST_VA6_SIZE      0x10000000u
#define GUEST_VA7_BASE      0xEBAD7000u
#define GUEST_VA7_SIZE      0x14000000u

#define GUEST_TEB_BASE      0x3F000000u
#define GUEST_HLE_BASE      0x7F000000u
#define GUEST_HLE_STRIDE    0x00000010u
#define GUEST_HLE_MAX       65536u

#define GUEST_THREAD_MAX        24u
#define GUEST_THREAD_STACK_BASE 0x31800000u
#define GUEST_THREAD_STACK_SIZE 0x00200000u
#define GUEST_THREAD_TEB_BASE   0x3F100000u

_Static_assert((GUEST_HLE_STRIDE % 4u) == 0u, "HLE thunk stride must be aligned");
_Static_assert(GUEST_STACK_SIZE >= GUEST_THREAD_STACK_SIZE,
               "main stack must not be smaller than a worker stack");

#endif /* WC3_GUEST_LAYOUT_H */
