/*
 * d3d9_thin.h - thin D3D9 COM bridge, shared declarations.
 *
 * Ported from the as-built `runtime/host/hle_d3d9_thin.h` of the previous
 * tree.  The guest-visible layouts below are the frozen part that the
 * as-built bridge defined, and the source port keeps them: ThinGuestObj,
 * ThinObj, ThinSlot, the guest structs and the mangled signatures of the
 * lock layer must not change.
 *
 * What changed in the port:
 *   - CPU, wr32g/rd32g and the guest layout come from the target headers
 *     (hle_registry.h -> host.h -> cpu.h/guest_layout.h), not the old tree's
 *     hle_internal.h;
 *   - diagnostics are tagged WC3_DEBUG_GRAPHICS (BV-6), so a release build
 *     carries no `[thin-d3d9]` line from this translation unit.
 *
 * The guest is 32-bit and calls COM methods through a vtable in guest memory.
 * Each guest object is only 16 bytes ({vtbl, id, iface, magic}); the host
 * keeps a side table with the native 64-bit DXVK IUnknown pointer, the refcount
 * and the concrete interface.  Everything that crosses the boundary is
 * marshalled: HRESULT/ULONG returns go into eax, 32-bit guest handles stand in
 * for native interface pointers, and Lock/LockRect/LockBox hand the guest a
 * 32-bit pointer: DXVK's own mapping when it lies low, otherwise a copy in a
 * committed low arena.
 */
#ifndef WC3_HLE_D3D9_THIN_H
#define WC3_HLE_D3D9_THIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>
#include <d3d9caps.h>
#include <d3d9.h>

extern "C" {
#include "hle_registry.h"   /* CPU, HleEnt, wr32g/rd32g, guest_commit */
#include "hle_android.h"    /* hle_android_guest_ok and the input surface */
}
#include "d3d9_tables.h"
#include "wc3_debug.h"

/* Both markers are debug-only.  The old header kept THIN_MARK unconditional
 * so the backend could be identified in a release log; BV-6.1 forbids that,
 * and this port belongs to the target tree. */
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
#define THIN_MARK(...) do { \
    fprintf(stderr, "[thin-d3d9] " __VA_ARGS__); \
    fputc('\n', stderr); \
} while (0)
#define THIN_LOG(...) THIN_MARK(__VA_ARGS__)
#else
#define THIN_MARK(...) ((void)0)
#define THIN_LOG(...) ((void)0)
#endif

#ifndef D3D_OK
#define D3D_OK 0
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL 0x8876086Cu
#endif
#ifndef D3DERR_NOTAVAILABLE
#define D3DERR_NOTAVAILABLE 0x8876086Au
#endif
#ifndef D3DERR_DEVICELOST
#define D3DERR_DEVICELOST 0x88760868u
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE 0x80004002u
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY 0x8007000Eu
#endif
#ifndef D3DVS_VERSION
#define D3DVS_VERSION(a, b) ((DWORD)(0xFFFE0000u | ((a) << 8) | (b)))
#endif

/* ------------------------------------------------------------------ */
/* 32-bit guest structures.  The host's D3DPRESENT_PARAMETERS (64 bytes),    */
/* D3DDEVICE_CREATION_PARAMETERS (24), D3DLOCKED_RECT (16) and              */
/* D3DLOCKED_BOX (24) all carry pointer-sized fields, so they can never be  */
/* forwarded as a block.  The sizes below are part of the guest ABI and are  */
/* asserted both here and by the generator's ABI check.                     */
/* ------------------------------------------------------------------ */

#pragma pack(push, 4)
struct PP32 {
    uint32_t BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount;
    uint32_t MultiSampleType, MultiSampleQuality, SwapEffect, hDeviceWindow;
    uint32_t Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat;
    uint32_t Flags, FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
struct Creation32 {
    uint32_t AdapterOrdinal, DeviceType, hFocusWindow, BehaviorFlags;
};
struct LockedRect32 { uint32_t Pitch; uint32_t pBits; };
struct LockedBox32  { uint32_t RowPitch, SlicePitch, pBits; };
struct SurfaceDesc32 {
    uint32_t Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
    uint32_t Width, Height;
};
struct BufferDesc32 { uint32_t Format, Type, Usage, Pool, Size, FVF; };
struct IndexBufferDesc32 { uint32_t Format, Type, Usage, Pool, Size; };
/* WC3 1.27 was built against a pre-9.0c D3DADAPTER_IDENTIFIER9: 0x448 bytes,
 * with no trailing WHQLLevel.  Writing the host's 0x44C bytes would
 * overwrite the caller's stack cookie. */
struct AdapterId32 {
    char Driver[512];
    char Description[512];
    char DeviceName[32];
    LARGE_INTEGER DriverVersion;
    uint32_t VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
};
#pragma pack(pop)

static_assert(sizeof(PP32) == 56, "PP32 guest layout");
static_assert(sizeof(Creation32) == 16, "Creation32 guest layout");
static_assert(sizeof(LockedRect32) == 8, "LockedRect32 guest layout");
static_assert(sizeof(LockedBox32) == 12, "LockedBox32 guest layout");
static_assert(sizeof(AdapterId32) == 0x448, "AdapterId32 guest layout");
static_assert(sizeof(SurfaceDesc32) == 32, "SurfaceDesc32 guest layout");
static_assert(sizeof(BufferDesc32) == 24, "BufferDesc32 guest layout");
static_assert(sizeof(IndexBufferDesc32) == 20, "IndexBufferDesc32 guest layout");

/* D3DCAPS9 as the guest sees it.  Pinned here so the ABI does not depend on
 * which SDK header the host was compiled with. */
struct Caps9 {
    DWORD DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3;
    DWORD PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps;
    DWORD RasterCaps;
    DWORD ZCmpCaps;
    DWORD SrcBlendCaps;
    DWORD DestBlendCaps;
    DWORD AlphaCmpCaps;
    DWORD ShadeCaps;
    DWORD TextureCaps;
    DWORD TextureFilterCaps;
    DWORD CubeTextureFilterCaps;
    DWORD VolumeTextureFilterCaps;
    DWORD TextureAddressCaps;
    DWORD VolumeTextureAddressCaps;
    DWORD LineCaps;
    DWORD MaxTextureWidth;
    DWORD MaxTextureHeight;
    DWORD MaxVolumeExtent;
    DWORD MaxTextureRepeat;
    DWORD MaxTextureAspectRatio;
    DWORD MaxAnisotropy;
    float MaxVertexW;
    float GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom;
    float ExtentsAdjust;
    DWORD StencilCaps;
    DWORD FVFCaps;
    DWORD TextureOpCaps;
    DWORD MaxTextureBlendStages;
    DWORD MaxSimultaneousTextures;
    DWORD VertexProcessingCaps;
    DWORD MaxActiveLights;
    DWORD MaxUserClipPlanes;
    DWORD MaxVertexBlendMatrices;
    DWORD MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount;
    DWORD MaxVertexIndex;
    DWORD MaxStreams;
    DWORD MaxStreamStride;
    DWORD VertexShaderVersion;
    DWORD MaxVertexShaderConst;
    DWORD PixelShaderVersion;
    float MaxPixelShaderValue;
};
static_assert(sizeof(Caps9) == 0xD4, "D3DCAPS9 guest layout");

/* ------------------------------------------------------------------ */
/* Guest and host objects                                             */
/* ------------------------------------------------------------------ */

#define THIN_MAGIC      0x5448494Eu   /* "THIN" */
#define THIN_MAX_REC    16384
#define THIN_F_USED     0x0001
#define THIN_F_DETACHED 0x0002

struct ThinGuestObj {
    uint32_t vtbl;
    uint32_t id;
    uint32_t iface;
    uint32_t magic;
};
static_assert(sizeof(ThinGuestObj) == 16, "guest COM object layout");

struct ThinObj {
    void     *host;      /* native IUnknown* */
    uint32_t  guest;     /* 32-bit handle */
    uint32_t  refs;      /* host references owned by this wrapper */
    uint16_t  iface;     /* D3D9_IFACE_* of the guest vtable */
    uint16_t  flags;
};

struct ThinSlot {
    uint8_t  iface;
    uint8_t  index;
    uint8_t  argc;
    uint8_t  pad;
    uint16_t key;        /* iface * 256 + index */
    uint16_t hw;         /* 1 = routed to thin_hw_call */
};

/* ------------------------------------------------------------------ */
/* Guest stack helpers.  Argument 0 is `this`; real args start at 1.    */
/* ------------------------------------------------------------------ */

static inline uint32_t thin_arg(CPU *c, unsigned n)
{
    return *(volatile uint32_t *)(uintptr_t)(c->esp + 4u + n * 4u);
}
static inline float thin_argf(CPU *c, unsigned n)
{
    uint32_t b = thin_arg(c, n);
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}
static inline uint32_t thin_fbits(float f)
{
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

/* ------------------------------------------------------------------ */
/* Runtime API (d3d9_thin.cpp)                                        */
/* ------------------------------------------------------------------ */

/* Validate a guest pointer before handing it to DXVK.  Returns the host
 * address, or 0 when the range is not mapped.  A zero guest pointer is
 * preserved as a null host pointer. */
uintptr_t thin_gptr(CPU *c, unsigned n, size_t size);

/* host -> guest.  `take_ref` says the caller already owns one host
 * reference (the usual Create/Get case); otherwise AddRef is done here. */
uint32_t thin_wrap(void *host, int iface, int take_ref);
void     *thin_unwrap(uint32_t guest);

/* Out-interface handling for generated thunks. */
void thin_after_out(CPU *c, unsigned n, void *host, int iface);
void thin_drop_out(void *host);
/* Write a 32-bit out value only if the guest pointer is mapped. */
void thin_out_u32(CPU *c, unsigned n, uint32_t value);

/* Dispatch.  `thin_method_hw` is emitted by the generator from the ABI spec;
 * it is the single source of truth for which methods are handwritten. */
void thin_gen_call(CPU *c, const ThinSlot *s, ThinObj *o);
void thin_hw_call(CPU *c, const ThinSlot *s, ThinObj *o);
int  thin_method_hw(unsigned key);

/* Front door.  hle_android.c declares that symbol weak and calls it when
 * the D3D9 backend is the thin bridge, so no C-side change and no duplicate
 * registration is needed.  C linkage: a future runtime switch would call it
 * from C. */
extern "C" void hle_d3d9_thin_register(void);

/* Device lifetime: called from Reset to drop native COM references before
 * DXVK recreates the swapchain. */
void thin_detach_all(void);
void thin_forget_host(void *host);

/* Cheap A/B counters and debug log. */
enum {
    THIN_STAT_D3D9 = 0,
    THIN_STAT_CREATE_DEVICE,
    THIN_STAT_RESET,
    THIN_STAT_PRESENT,
    THIN_STAT_CREATE_TEXTURE,
    THIN_STAT_CREATE_BUFFER,
    THIN_STAT_LOCK,
    THIN_STAT_UNLOCK,
    THIN_STAT_DRAW,
    THIN_STAT_CAPS,
    THIN_STAT_MAX,
};
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
void thin_count(unsigned which);
void thin_note(const char *what);
#else
#define thin_count(which) ((void)0)
#define thin_note(what)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Lock layer (d3d9_thin_lock.cpp)                                    */
/* ------------------------------------------------------------------ */

enum {
    THIN_LK_TEXTURE = 0,
    THIN_LK_CUBE,
    THIN_LK_SURFACE,
    THIN_LK_VOLUME,
    THIN_LK_VOLTEX,
    THIN_LK_BUFFER,
};

uint32_t thin_lock_rect(CPU *c, ThinObj *o, int kind, uint32_t sub,
                        uint32_t flags, uint32_t guest_rect,
                        uint32_t guest_out, int is_box);
uint32_t thin_unlock_rect(CPU *c, ThinObj *o, int kind, uint32_t sub);
uint32_t thin_lock_buffer(CPU *c, ThinObj *o, uint32_t off, uint32_t size,
                          uint32_t flags, uint32_t guest_out);
uint32_t thin_unlock_buffer(CPU *c, ThinObj *o);
/* Drop every shadow and its staging allocation.  Called before Reset: the
 * native objects the shadows described are released at that point. */
void thin_lock_reset(void);
/* Drop only the shadows of one native object, when its last guest reference
 * goes away.  Without this a long session that streams textures would fill
 * the fixed shadow table. */
void thin_lock_forget(void *host);

/* D3DPRESENT_PARAMETERS conversion shared by Reset/CreateDevice. */
void thin_pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *pp);
void thin_pp_to_guest(uint32_t g, const D3DPRESENT_PARAMETERS *pp);

#endif /* WC3_HLE_D3D9_THIN_H */
