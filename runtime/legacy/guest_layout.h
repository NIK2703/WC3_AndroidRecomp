/* guest_layout.h - the guest address space, per platform.
 *
 * Split out of host.h so that a C++ translation unit can see the layout
 * without pulling in cpu.h and the host prototypes: hle_d3d9_dxvk.cpp needs
 * the same ranges to keep its staging arena out of the guest's way, and a
 * hand copy of a layout is a copy that goes stale silently.
 *
 * The Windows values are the reference host's and must not change.  The
 * Android values exist because ART owns 0x10980000-0x30980000 in an app
 * process; see docs/ANDROID_WSI.md §10 for the measurement and for why the
 * images (and therefore the corpus) have to be rebased.
 */
#ifndef WC3_GUEST_LAYOUT_H
#define WC3_GUEST_LAYOUT_H

#if defined(__ANDROID__)
/* An app process is not a shell process.  ART maps boot.oat at
 * 0x05440000-0x0c8e9000 and reserves a 512 MB region space at
 * 0x10980000-0x30980000 before the activity starts, so every region that used
 * to live there has to move (docs/ANDROID_WSI.md §10, measured on the device).
 *
 * The anonymous regions are free to move: nothing depends on their addresses.
 * The two *images* are not - Storm and Mss32 are compiled into the lift with
 * their addresses in the code, so moving them means re-lifting from a rebased
 * copy of the DLL (tools/rebase_pe.py) and linking that corpus instead
 * (build27/chunks/storm_a35, reports/lift27/storm_27_*_a35.c).  The Windows
 * layout below stays the reference, and the Windows corpus stays untouched.
 *
 * Free range measured in the app process: 0x30980000-0x91D80000 (1556 MB). */
#define GUEST_IMAGE_WAR3   0x00400000u
#define GUEST_IMAGE_GAME   0x6F000000u
#define GUEST_IMAGE_STORM  0x35000000u
#define GUEST_IMAGE_MSS    0x35100000u
#define GUEST_IMAGE_MSSFAST 0x35200000u
#define GUEST_IMAGE_MP3DEC 0x35300000u
#define GUEST_IMAGE_IJL    0x60000000u
#define GUEST_STACK_BASE   0x31000000u
#define GUEST_STACK_SIZE   0x00800000u
#define GUEST_HEAP_BASE    0x36000000u
#define GUEST_HEAP_SIZE    0x04000000u
#define GUEST_HLE_HEAP_BASE 0x3A000000u
#define GUEST_HLE_HEAP_SIZE 0x04000000u
#define GUEST_VA_BASE      0x61000000u
#define GUEST_VA_SIZE      0x04000000u
#define GUEST_VA2_BASE     0x40000000u
#define GUEST_VA2_SIZE     0x20000000u
/* 32-bit Windows does not require every VirtualAlloc to live below 2 GB when
 * the process is LARGEADDRESSAWARE, and the app process has a measured free
 * range up to 0x91D80000.  Warcraft III asks for one 256 MB reserve after the
 * first two arenas are full; without a third arena that reserve fails and the
 * game reports a critical error before audio starts. */
#define GUEST_VA3_BASE     0x80000000u
#define GUEST_VA3_SIZE     0x10000000u
/* The 0x70000000..0x80000000 block is the only free 256 MB range below
 * 0x80000000 once the game image ends.  TEBs move down into the gap between
 * the HLE heap and the second VA arena so this block stays one mapping. */
#define GUEST_VA4_BASE     0x70000000u
#define GUEST_VA4_SIZE     0x10000000u
/* Above ART's large-object space and below the JIT code caches, the app's
 * 64-bit process has another 512 MB free in the low 4 GB.  The guest mirrors
 * a 32-bit noLAA address space but it is our address space, so a reserve above
 * 2 GB is legal here; the game already asks for such addresses itself. */
#define GUEST_VA5_BASE     0xA1000000u
#define GUEST_VA5_SIZE     0x30000000u
/* One more 256 MB window between the JIT caches and ART's sentinel page. */
#define GUEST_VA6_BASE     0xD9800000u
#define GUEST_VA6_SIZE     0x10000000u
/* The game's CDataRecycler genuinely keeps growing: it doubles its bucket
 * array (32, 64, 128, 256 MB) as the loading map frees more blocks.  The
 * measured free window above ART's sentinel page runs to 0xFFFFFFFF; start
 * clear of it and stop short of the 4 GB wrap that va_live_overlaps does
 * arithmetic on.  320 MB covers the next 256 MB reserve with room to spare. */
#define GUEST_VA7_BASE     0xEBAD7000u
#define GUEST_VA7_SIZE     0x14000000u
#define GUEST_TEB_BASE     0x3F000000u
#define GUEST_HLE_BASE     0x7F000000u
#define GUEST_HLE_STRIDE   0x00000010u
#define GUEST_THREAD_MAX        24
#define GUEST_THREAD_STACK_BASE 0x31800000u
#define GUEST_THREAD_STACK_SIZE 0x00200000u
#define GUEST_THREAD_TEB_BASE   0x3F100000u
#else
#define GUEST_IMAGE_WAR3   0x00400000u
#define GUEST_IMAGE_GAME   0x6F000000u
#define GUEST_IMAGE_STORM  0x15000000u
#define GUEST_IMAGE_MSS    0x21100000u
#define GUEST_IMAGE_MSSFAST 0x22600000u
#define GUEST_IMAGE_MP3DEC 0x26F00000u
#define GUEST_IMAGE_IJL    0x60000000u
#define GUEST_STACK_BASE   0x30000000u
#define GUEST_STACK_SIZE   0x00800000u
#define GUEST_HEAP_BASE    0x08000000u
#define GUEST_HEAP_SIZE    0x04000000u
/* The game owns GUEST_HEAP_BASE: Storm's SMemAlloc reserves fixed blocks at
 * 0x08000000/0x08001000.  HLE allocations must live outside that arena. */
#define GUEST_HLE_HEAP_BASE 0x0C000000u
#define GUEST_HLE_HEAP_SIZE 0x04000000u
/* VirtualAlloc(NULL) arena.  Windows rounds every fresh reservation up to the
 * 64 KiB allocation granularity; Storm's SMem block magic stores only
 * heap_base>>16, and SMemReAlloc reconstructs the heap pointer as
 * (magic & 0xFFFF) << 16.  Returning a 16-byte-aligned HLE payload makes the
 * reconstructed heap point at unrelated zeroed memory, so the block magic is
 * rewritten as 0 and the next grow silently loses the element array.
 *
 * War3 creates one Storm heap per named class (over a thousand), and each one
 * costs a 64 KiB slot, so a single 64 MiB range is not enough.  Each range is
 * bump-allocated in 64 KiB slots and mapped on demand; a second range covers
 * the free 0x40000000..0x60000000 block above the guest stacks. */
#define GUEST_VA_BASE      0x10000000u
#define GUEST_VA_SIZE      0x04000000u
#define GUEST_VA2_BASE     0x40000000u
#define GUEST_VA2_SIZE     0x20000000u
#define GUEST_TEB_BASE     0x7FFD0000u
#define GUEST_HLE_BASE     0x7F000000u
#define GUEST_HLE_STRIDE   0x00000010u

/* Guest worker threads.  Each worker gets its own mapped stack (the lifted
 * code uses ESP as a real flat address) and its own simulated TIB page.  The
 * main thread keeps GUEST_TEB_BASE; worker TIBs live in a separate arena. */
#define GUEST_THREAD_MAX        24
#define GUEST_THREAD_STACK_BASE 0x32000000u
#define GUEST_THREAD_STACK_SIZE 0x00200000u
#define GUEST_THREAD_TEB_BASE   0x7FDA0000u
#endif /* __ANDROID__ layout */

#endif /* WC3_GUEST_LAYOUT_H */
