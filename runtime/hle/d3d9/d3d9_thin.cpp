/*
 * d3d9_thin.cpp - thin D3D9 COM bridge.
 *
 * Ported from the as-built `runtime/host/hle_d3d9_thin.cpp` of the previous
 * tree.  In this tree it is the product D3D9 backend, not a -ThinD3D9
 * experiment: hle_android.c selects it through the weak hle_register_d3d9
 * hook.  The generated dispatcher is still the pinned hle_d3d9_thin_gen.o
 * (thin_gen_call/thin_method_hw); moving its generator is a separate step.
 *
 * Shape of the bridge
 * -------------------
 *   guest 32-bit vtable slot
 *     -> thin_entry()            recover (iface, index) from the thunk
 *     -> thin_gen_call()         generated marshalling (216 methods)
 *        or thin_hw_call()       handwritten policy/state machine (92)
 *     -> native IDirect3D9 / IDirect3DDevice9 / resource from DXVK
 *
 * The guest object is 16 bytes; the native 64-bit pointer, the refcount and
 * the concrete interface live in a host-side table.  A single guest object
 * represents a single native object, so GetTexture/GetRenderTarget/
 * GetSurfaceLevel return the wrapper the engine already holds instead of a
 * second one.
 *
 * The source port adds the missing guest-handle reclaim: when the last guest
 * reference to an ordinary wrapper goes away, thin_obj_release() now returns
 * its 16-byte ThinGuestObj to the HLE heap.  The as-built object only cleared
 * the record, so a session that streamed surfaces exhausted the fixed heap
 * after about a million GetSurfaceLevel calls even though every native object
 * had been destroyed.
 *
 * Caps are deliberately synthetic: the backend advertises a fixed-function
 * device (VS 1.1, PS 0) because WC3 picks a shader path that draws nothing
 * when a shader model is reported.  Forwarding DXVK's real Adreno caps would
 * change that decision, so the same policy is cloned here and the real values
 * are only logged.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "d3d9_thin.h"
#include "d3d9_thin_iids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef D3D_OK
#define D3D_OK 0
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL 0x8876086Cu
#endif
#ifndef D3DERR_NOTAVAILABLE
#define D3DERR_NOTAVAILABLE 0x8876086Au
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE 0x80004002u
#endif

/* The two statically embedded DXVK entry points (dxvk_entry.cpp). */
extern "C" IDirect3D9 *wc3_dxvk1_Direct3DCreate9(UINT sdk);
extern "C" IDirect3D9 *wc3_dxvk3_Direct3DCreate9(UINT sdk);
extern "C" void wc3_dxvk_prepare(void);
extern "C" int wc3_vulkan_major_minor(int *major, int *minor);
/* dxvk_win32.cpp: the ANativeWindow the presenter turns into a VkSurfaceKHR.
 * Declared as void* because this file has no <android/native_window.h>; the
 * pointer representation is identical. */
extern "C" void *wc3_android_window(void);
/* hle_android.c: the stable non-null HWND stand-in the guest tests for null. */
extern "C" void *hle_host_hwnd(void);

/* ------------------------------------------------------------------ */
/* diagnostics                                                        */
/* ------------------------------------------------------------------ */

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
static unsigned g_stat[THIN_STAT_MAX];

void thin_count(unsigned which)
{
    if (which < THIN_STAT_MAX) g_stat[which]++;
}

void thin_note(const char *what)
{
    THIN_LOG("%s", what ? what : "");
}

static void thin_dump_stats(void)
{
    THIN_LOG("stats d3d9=%u device=%u reset=%u present=%u tex=%u buf=%u "
             "lock=%u unlock=%u draw=%u caps=%u",
             g_stat[THIN_STAT_D3D9], g_stat[THIN_STAT_CREATE_DEVICE],
             g_stat[THIN_STAT_RESET], g_stat[THIN_STAT_PRESENT],
             g_stat[THIN_STAT_CREATE_TEXTURE], g_stat[THIN_STAT_CREATE_BUFFER],
             g_stat[THIN_STAT_LOCK], g_stat[THIN_STAT_UNLOCK],
             g_stat[THIN_STAT_DRAW], g_stat[THIN_STAT_CAPS]);
}

/* A/B needs to tell "the method was never called" from "it was called and did
 * nothing".  Counting by vtable key is one compare per call and needs no
 * per-method code in the generated file. */
static void thin_account(uint16_t key)
{
    switch (key) {
    case 0x0117: case 0x0118: case 0x0119:
        thin_count(THIN_STAT_CREATE_TEXTURE);
        break;
    case 0x011a: case 0x011b: case 0x011c:
    case 0x011d: case 0x011e:
        thin_count(THIN_STAT_CREATE_BUFFER);
        break;
    case 0x0151: case 0x0152: case 0x0153: case 0x0154:
        thin_count(THIN_STAT_DRAW);
        break;
    default:
        break;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* render-state trace (diagnostic, WC3_DEBUG_GRAPHICS builds only)    */
/*                                                                    */
/* The lift's depth/blend state stream is the only place where the    */
/* renderer can differ from the original while still drawing the      */
/* right geometry.  This records state *changes* with a frame and     */
/* draw index, plus one summary line per frame, so a screenshot can   */
/* be matched to the exact state that produced it.                    */
/* ------------------------------------------------------------------ */

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
struct ThinGfxTrace {
    int      on;
    unsigned frame;
    unsigned draws, zw1, zw0, zf_less, zf_always, nodepth;
    unsigned rs_zw, rs_zf, rs_zenable, rs_alpha, rs_cull;
    int      depth_bound;
    unsigned clear_flags;
    unsigned rs_changes;
};

/* Field order above; defaults are the D3D9 device defaults (ZENABLE on,
 * ZWRITE on, ZFUNC LESSEQUAL, ALPHABLEND off, CULLMODE CCW) because the
 * engine relies on them before it ever touches the state. */
static ThinGfxTrace g_gfx = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 1, 0, 3, 1, 0, 0
};

static unsigned g_gfx_detail;

static int thin_gfx_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("WC3_GFX_TRACE");
        v = (!e || *e != '0') ? 1 : 0;
    }
    return v;
}

static void thin_gfx_event(CPU *c, uint16_t key)
{
    ThinGfxTrace *t = &g_gfx;
    if (!thin_gfx_on()) return;
    switch (key) {
    case 0x0139: { /* IDirect3DDevice9::SetRenderState */
        unsigned st = thin_arg(c, 1), val = thin_arg(c, 2);
        switch (st) {
        case 7:  if (val == t->rs_zenable) return; t->rs_zenable = val; break;
        case 14: if (val == t->rs_zw)      return; t->rs_zw = val; break;
        case 23: if (val == t->rs_zf)      return; t->rs_zf = val; break;
        case 27: if (val == t->rs_alpha)   return; t->rs_alpha = val; break;
        case 22: if (val == t->rs_cull)    return; t->rs_cull = val; break;
        default: return;
        }
        t->rs_changes++;
        if (g_gfx_detail < 4000u) {
            g_gfx_detail++;
            THIN_LOG("GFX rs f=%u d=%u st=%u v=%u", t->frame, t->draws, st, val);
        }
        return;
    }
    case 0x0127: { /* SetDepthStencilSurface */
        unsigned p = thin_arg(c, 1);
        if ((p != 0) == (t->depth_bound != 0)) { t->depth_bound = p ? 1 : 0; return; }
        t->depth_bound = p ? 1 : 0;
        THIN_LOG("GFX depth-surface f=%u p=%08X", t->frame, p);
        return;
    }
    case 0x012b: /* Clear(Count, pRects, Flags, Color, Z, Stencil) */
        t->clear_flags = thin_arg(c, 3);
        THIN_LOG("GFX clear f=%u flags=%04X", t->frame, t->clear_flags);
        return;
    case 0x0151: case 0x0152: case 0x0153: case 0x0154:
        t->draws++;
        if (t->rs_zw == 1) t->zw1++; else t->zw0++;
        if (t->rs_zf == 4) t->zf_less++;
        else if (t->rs_zf == 8) t->zf_always++;
        if (!t->depth_bound) t->nodepth++;
        return;
    case 0x0111: /* Present: close the frame just drawn */
        if (t->frame < 1500u || (t->frame % 60u) == 0u) {
            THIN_LOG("GFX f=%u draws=%u zw1=%u zw0=%u zless=%u zalways=%u "
                     "nodepth=%u rs=%u zen=%u clear=%04X surf=%d",
                     t->frame, t->draws, t->zw1, t->zw0, t->zf_less,
                     t->zf_always, t->nodepth, t->rs_changes, t->rs_zenable,
                     t->clear_flags, t->depth_bound);
        }
        t->frame++;
        t->draws = t->zw1 = t->zw0 = t->zf_less = t->zf_always = 0;
        t->nodepth = 0;
        t->rs_changes = 0;
        return;
    default:
        return;
    }
}
#else
#define thin_gfx_event(c, key) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* registry                                                           */
/* ------------------------------------------------------------------ */

static ThinObj g_rec[THIN_MAX_REC];
static uint32_t g_vt[D3D9_IFACE_COUNT];
static int g_vt_ready;

/* thin_wrap() is called for every out-parameter resource and the original
 * registry is a linear scan over 16 K records.  The profile shows it at
 * 2.04% of the render thread.  This cache is only a lookup accelerator: the
 * record table remains authoritative, and a miss falls back to the exact
 * linear search.  A slot stores index + 1, so zero means empty. */
#define THIN_WRAP_CACHE 32768u
static_assert((THIN_WRAP_CACHE & (THIN_WRAP_CACHE - 1u)) == 0u,
              "wrap_hash masks with THIN_WRAP_CACHE - 1");
static_assert(THIN_MAX_REC < 65535u,
              "wrap cache stores record index + 1 in uint16_t");
static uint16_t g_wrap_cache[THIN_WRAP_CACHE];

/* Free records are linked through ThinObj::guest while flags == 0.  Allocation
 * no longer scans the table, and a released record is immediately reusable. */
static uint32_t g_free_head;
static int g_free_ready;

static uint32_t wrap_hash(const void *host)
{
    uintptr_t v = (uintptr_t)host;
    v ^= v >> 17;
    v ^= v >> 9;
    return (uint32_t)v & (THIN_WRAP_CACHE - 1u);
}

static void wrap_cache_reset(void)
{
    memset(g_wrap_cache, 0, sizeof g_wrap_cache);
}

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static uint32_t g_d3d_obj;
static uint32_t g_device_obj;
static PP32 g_pp;
static int g_device_retired;
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
static unsigned g_presents;
#endif

static void rec_pool_init(void)
{
    for (uint32_t i = 0; i < THIN_MAX_REC; i++)
        g_rec[i].guest = i + 1u;
    g_rec[THIN_MAX_REC - 1u].guest = UINT32_MAX;
    g_free_head = 0;
    g_free_ready = 1;
}

static ThinObj *rec_alloc(void)
{
    if (!g_free_ready) rec_pool_init();
    if (g_free_head == UINT32_MAX) {
        THIN_LOG("registry full (%d objects)", THIN_MAX_REC);
        return NULL;
    }
    uint32_t i = g_free_head;
    ThinObj *r = &g_rec[i];
    g_free_head = r->guest;
    memset(r, 0, sizeof *r);
    r->flags = THIN_F_USED;
    return r;
}

static int rec_index(const ThinObj *r)
{
    return (int)(r - g_rec);
}

static void wrap_cache_clear(const ThinObj *r)
{
    if (!r->host) return;
    uint32_t slot = wrap_hash(r->host);
    if (g_wrap_cache[slot] == (uint16_t)(rec_index(r) + 1))
        g_wrap_cache[slot] = 0;
}

static void rec_free(ThinObj *r)
{
    uint32_t i = (uint32_t)rec_index(r);
    r->host = NULL;
    r->guest = g_free_head;
    r->flags = 0;
    g_free_head = i;
}

static void guest_set_iface(ThinObj *r, int iface)
{
    if (iface < 0 || iface >= D3D9_IFACE_COUNT) return;
    r->iface = (uint16_t)iface;
    ThinGuestObj *o = (ThinGuestObj *)(uintptr_t)r->guest;
    o->iface = (uint32_t)iface;
    o->vtbl = g_vt[iface];
}

/* D3DRESOURCETYPE -> concrete interface index.  `GetType` is part of the
 * Resource9 prefix, so it is valid on every concrete resource vtable. */
static int iface_from_type(void *host)
{
    if (!host) return -1;
    D3DRESOURCETYPE t = ((IDirect3DResource9 *)host)->GetType();
    switch (t) {
    case D3DRTYPE_SURFACE:       return D3D9_IFACE_IDirect3DSurface9;
    case D3DRTYPE_VERTEXBUFFER:  return D3D9_IFACE_IDirect3DVertexBuffer9;
    case D3DRTYPE_INDEXBUFFER:   return D3D9_IFACE_IDirect3DIndexBuffer9;
    case D3DRTYPE_TEXTURE:       return D3D9_IFACE_IDirect3DTexture9;
    case D3DRTYPE_VOLUMETEXTURE: return D3D9_IFACE_IDirect3DVolumeTexture9;
    case D3DRTYPE_CUBETEXTURE:   return D3D9_IFACE_IDirect3DCubeTexture9;
    case D3DRTYPE_VOLUME:        return D3D9_IFACE_IDirect3DVolume9;
    default:                     return -1;
    }
}

static uint32_t wrap_hit(ThinObj *r, int iface, int take_ref)
{
    if (!take_ref) ((IUnknown *)r->host)->AddRef();
    r->refs++;
    /* BaseTexture/Resource9 requests keep the concrete vtable; a concrete
     * request switches it.  All concrete resource tables share the Resource9
     * prefix, so this does not invalidate the old handle. */
    if (iface >= 0 && (int)r->iface != iface) guest_set_iface(r, iface);
    return r->guest;
}

uint32_t thin_wrap(void *host, int iface, int take_ref)
{
    if (!host) return 0;
    IUnknown *unk = (IUnknown *)host;
    uint32_t slot = wrap_hash(host);

    uint32_t cached = g_wrap_cache[slot];
    if (cached) {
        ThinObj *r = &g_rec[cached - 1u];
        if ((r->flags & THIN_F_USED) && r->host == host)
            return wrap_hit(r, iface, take_ref);
    }

    for (int i = 0; i < THIN_MAX_REC; i++) {
        ThinObj *r = &g_rec[i];
        if (!(r->flags & THIN_F_USED) || r->host != host) continue;
        g_wrap_cache[slot] = (uint16_t)(i + 1);
        return wrap_hit(r, iface, take_ref);
    }

    int kind = iface;
    if (kind < 0) kind = iface_from_type(host);
    if (kind < 0) {
        if (take_ref) unk->Release();
        THIN_LOG("cannot resolve concrete interface for %p", host);
        return 0;
    }

    ThinObj *r = rec_alloc();
    if (!r) { if (take_ref) unk->Release(); return 0; }
    uint32_t g = g_alloc0(sizeof(ThinGuestObj));
    if (!g) { rec_free(r); if (take_ref) unk->Release(); return 0; }

    ThinGuestObj *o = (ThinGuestObj *)(uintptr_t)g;
    o->vtbl = g_vt[kind];
    o->id = (uint32_t)rec_index(r);
    o->iface = (uint32_t)kind;
    o->magic = THIN_MAGIC;

    r->host = host;
    r->guest = g;
    r->refs = 1;             /* the caller's reference (take_ref) or the one
                              * added just above */
    r->iface = (uint16_t)kind;
    r->flags = THIN_F_USED;
    if (!take_ref) unk->AddRef();
    g_wrap_cache[slot] = (uint16_t)(rec_index(r) + 1);
    return g;
}

void *thin_unwrap(uint32_t g)
{
    if (!g) return NULL;
    /* The handle comes from a guest argument, so it can be anything.  Reading
     * the magic from an unmapped address would fault the host inside a COM
     * call, which is indistinguishable from a renderer bug. */
    if (!hle_android_guest_ok(g, sizeof(ThinGuestObj))) {
        THIN_LOG("unmapped guest COM object %08X", g);
        return NULL;
    }
    ThinGuestObj *o = (ThinGuestObj *)(uintptr_t)g;
    if (o->magic != THIN_MAGIC || o->id >= THIN_MAX_REC) {
        THIN_LOG("bad guest COM object %08X (magic=%08X)", g, o->magic);
        return NULL;
    }
    ThinObj *r = &g_rec[o->id];
    if (!(r->flags & THIN_F_USED) || r->guest != g) {
        THIN_LOG("stale guest COM object %08X", g);
        return NULL;
    }
    return r->host;
}

static uint32_t thin_obj_addref(uint32_t g)
{
    if (!g || !hle_android_guest_ok(g, sizeof(ThinGuestObj))) return 0;
    ThinGuestObj *o = (ThinGuestObj *)(uintptr_t)g;
    if (o->magic != THIN_MAGIC || o->id >= THIN_MAX_REC) return 0;
    ThinObj *r = &g_rec[o->id];
    if (!(r->flags & THIN_F_USED) || r->guest != g) return 0;
    r->refs++;
    if (r->host && !(r->flags & THIN_F_DETACHED))
        ((IUnknown *)r->host)->AddRef();
    return r->refs;
}

static uint32_t thin_obj_release(uint32_t g)
{
    if (!g || !hle_android_guest_ok(g, sizeof(ThinGuestObj))) return 0;
    ThinGuestObj *o = (ThinGuestObj *)(uintptr_t)g;
    if (o->magic != THIN_MAGIC || o->id >= THIN_MAX_REC) return 0;
    ThinObj *r = &g_rec[o->id];
    if (!(r->flags & THIN_F_USED) || r->guest != g) return 0;

    /* The device and the IDirect3D9 object are kept alive across a guest
     * release: Android cannot create a second VkSurfaceKHR for the same
     * ANativeWindow, so the next CreateDevice has to reuse this device and
     * Reset it.  This mirrors g_device_retired in hle_d3d9.cpp. */
    if (r->iface == D3D9_IFACE_IDirect3DDevice9) {
        if (r->guest == g_device_obj) {
            g_device_retired = 1;
            g_device_obj = 0;
            THIN_LOG("device released by guest; keeping %p for reset", r->host);
        }
        if (r->refs) r->refs--;
        return r->refs;
    }
    if (r->iface == D3D9_IFACE_IDirect3D9) {
        /* The IDirect3D9 object is kept alive for the process lifetime, like
         * the device: Direct3DCreate9 may be called again after a device
         * change and the same wrapper is handed back. */
        if (r->refs > 1) r->refs--;
        return r->refs;
    }

    if (r->refs) r->refs--;
    if (r->refs == 0) thin_lock_forget(r->host);
    if (r->host && !(r->flags & THIN_F_DETACHED))
        ((IUnknown *)r->host)->Release();
    if (r->refs == 0) {
        /* The wrapper owns the last reference to the 16-byte guest object.
         * Return it to the HLE heap: GetSurfaceLevel alone is called millions
         * of times in a long session, and the as-built bridge leaked one
         * block per newly wrapped resource until the fixed heap was gone.
         * Clear the magic before returning the block: a Release that races
         * with reuse fails validation above.  Once the allocator hands the
         * block to a new wrapper, the guest pointer denotes that new COM
         * object, exactly as COM refcounting requires. */
        o->magic = 0;
        wrap_cache_clear(r);
        rec_free(r);
        hle_guest_free(g);
    }
    return r->refs;
}

void thin_drop_out(void *host)
{
    if (host) ((IUnknown *)host)->Release();
}

void thin_after_out(CPU *c, unsigned n, void *host, int iface)
{
    uint32_t out = thin_arg(c, n);
    if (!out || !hle_android_guest_ok(out, 4)) {
        /* The generated thunk already stored the native HRESULT in eax.  A
         * missing out pointer is a guest bug, but reporting D3D_OK with no
         * write hides it. */
        thin_drop_out(host);
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    uint32_t g = thin_wrap(host, iface, 1);
    if (!g) {
        /* thin_wrap() consumed the caller's reference on failure.  Returning
         * D3D_OK with a null out pointer makes the caller dereference null
         * (GetSurfaceLevel did exactly that in the heap-exhaustion crash), so
         * propagate the allocation failure as the method result. */
        c->eax = E_OUTOFMEMORY;
        wr32g(out, 0);
        return;
    }
    wr32g(out, g);
}

void thin_out_u32(CPU *c, unsigned n, uint32_t value)
{
    uint32_t out = thin_arg(c, n);
    if (out && hle_android_guest_ok(out, 4)) wr32g(out, value);
}

void thin_forget_host(void *host)
{
    if (!host) return;
    for (int i = 0; i < THIN_MAX_REC; i++) {
        ThinObj *r = &g_rec[i];
        if (!(r->flags & THIN_F_USED) || r->host != host) continue;
        wrap_cache_clear(r);
        r->host = NULL;
        r->flags |= THIN_F_DETACHED;
    }
}

/* Drop every native reference a guest wrapper still holds.  Native D3D9
 * requires all default-pool resources to be released before Reset; the guest
 * normally does that itself, but a wrapper that outlived the guest's own
 * Release would keep DXVK's resource counter above zero and make Reset fail.
 * The device and the IDirect3D9 object are deliberately kept. */
void thin_detach_all(void)
{
    for (int i = 0; i < THIN_MAX_REC; i++) {
        ThinObj *r = &g_rec[i];
        if (!(r->flags & THIN_F_USED) || !r->host) continue;
        if (r->iface == D3D9_IFACE_IDirect3DDevice9 ||
            r->iface == D3D9_IFACE_IDirect3D9) continue;
        if (r->flags & THIN_F_DETACHED) continue;
        for (uint32_t k = 0; k < r->refs; k++)
            ((IUnknown *)r->host)->Release();
        r->host = NULL;
        r->flags |= THIN_F_DETACHED;
    }
    wrap_cache_reset();
    /* The shadows described native objects that no longer exist. */
    thin_lock_reset();
}

/* ------------------------------------------------------------------ */
/* guest pointer validation                                           */
/* ------------------------------------------------------------------ */

uintptr_t thin_gptr(CPU *c, unsigned n, size_t size)
{
    uint32_t a = thin_arg(c, n);
    if (!a) return 0;
    uint32_t want = (uint32_t)(size ? size : 1);
    if (!hle_android_guest_ok(a, want)) {
        THIN_LOG("bad guest pointer %08X size %u (arg %u)", a, want, n);
        return 0;
    }
    return (uintptr_t)a;
}

/* ------------------------------------------------------------------ */
/* adapter, display modes, caps policy                                */
/* ------------------------------------------------------------------ */

static void thin_desktop_mode(int *w, int *h, int *refresh)
{
    uint32_t aw = 0, ah = 0;
    if (!hle_android_current_display_mode(&aw, &ah))
        hle_android_default_display_mode(&aw, &ah);
    *w = (int)aw;
    *h = (int)ah;
    *refresh = (int)hle_android_display_refresh_rate();
}

#define THIN_MODE_MAX 512
static D3DDISPLAYMODE g_modes[THIN_MODE_MAX];
static int g_mode_count = -1;
static uint32_t g_mode_gen;

static void thin_load_modes(void)
{
    uint32_t gen = hle_android_display_generation();
    if (g_mode_count >= 0 && g_mode_gen == gen) return;
    g_mode_gen = gen;
    g_mode_count = 0;
    uint32_t refresh = hle_android_display_refresh_rate();
    static const D3DFORMAT fmts[4] = {
        D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A2R10G10B10
    };
    for (int f = 0; f < 4; f++) {
        for (uint32_t i = 0; i < hle_android_display_mode_count(); i++) {
            uint32_t w = 0, h = 0;
            if (!hle_android_display_mode(i, &w, &h)) continue;
            if (g_mode_count >= THIN_MODE_MAX) break;
            D3DDISPLAYMODE *m = &g_modes[g_mode_count++];
            memset(m, 0, sizeof *m);
            m->Width = (UINT)w;
            m->Height = (UINT)h;
            m->RefreshRate = (UINT)refresh;
            m->Format = fmts[f];
        }
    }
    THIN_LOG("adapter modes: %d entries", g_mode_count);
}

static int thin_mode_count_for(uint32_t format)
{
    thin_load_modes();
    int n = 0;
    for (int i = 0; i < g_mode_count; i++)
        if ((uint32_t)g_modes[i].Format == format) n++;
    return n;
}

static const D3DDISPLAYMODE *thin_mode_for(uint32_t format, int index)
{
    thin_load_modes();
    int n = 0;
    for (int i = 0; i < g_mode_count; i++) {
        if ((uint32_t)g_modes[i].Format != format) continue;
        if (n == index) return &g_modes[i];
        n++;
    }
    return NULL;
}

static void thin_fill_display(D3DDISPLAYMODE *m)
{
    int w = 0, h = 0, r = 0;
    thin_desktop_mode(&w, &h, &r);
    memset(m, 0, sizeof *m);
    m->Width = (UINT)w;
    m->Height = (UINT)h;
    m->RefreshRate = (UINT)r;
    m->Format = D3DFMT_X8R8G8B8;
}

/* Advertise exactly what the fixed-function renderer implements.  Reporting
 * a shader model it does not have makes the engine take a path that draws
 * nothing, so the vertex shader version is 1.1 and the pixel shader version
 * is 0.  This is the same policy as fill_caps() in hle_d3d9.cpp. */
static void thin_fill_caps(Caps9 *c, UINT adapter, D3DDEVTYPE type)
{
    memset(c, 0, sizeof *c);
    c->DeviceType = type;
    c->AdapterOrdinal = adapter;
    c->Caps = D3DCAPS_READ_SCANLINE;
    c->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES |
               D3DCAPS2_CANAUTOGENMIPMAP;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD |
               D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION |
               D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_DEFAULT |
                               D3DPRESENT_INTERVAL_ONE |
                               D3DPRESENT_INTERVAL_TWO |
                               D3DPRESENT_INTERVAL_THREE |
                               D3DPRESENT_INTERVAL_FOUR |
                               D3DPRESENT_INTERVAL_IMMEDIATE;
    c->CursorCaps = D3DCURSORCAPS_COLOR;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                 D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                 D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                 D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                 D3DDEVCAPS_DRAWPRIMTLVERTEX |
                 D3DDEVCAPS_CANRENDERAFTERFLIP |
                 D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                 D3DDEVCAPS_DRAWPRIMITIVES2 |
                 D3DDEVCAPS_DRAWPRIMITIVES2EX |
                 D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                 D3DDEVCAPS_CANBLTSYSTONONLOCAL |
                 D3DDEVCAPS_HWRASTERIZATION |
                 D3DDEVCAPS_PUREDEVICE;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE |
                           D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                           D3DPMISCCAPS_COLORWRITEENABLE |
                           D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                           D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_TSSARGTEMP |
                           D3DPMISCCAPS_BLENDOP |
                           D3DPMISCCAPS_INDEPENDENTWRITEMASKS |
                           D3DPMISCCAPS_PERSTAGECONSTANT |
                           D3DPMISCCAPS_FOGANDSPECULARALPHA |
                           D3DPMISCCAPS_SEPARATEALPHABLEND |
                           D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                           D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
                           D3DPMISCCAPS_FOGVERTEXCLAMPED |
                           D3DPMISCCAPS_POSTBLENDSRGBCONVERT;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST |
                    D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE |
                    D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG |
                    D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_COLORPERSPECTIVE |
                    D3DPRASTERCAPS_SCISSORTEST |
                    D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS |
                    D3DPRASTERCAPS_DEPTHBIAS |
                    D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    c->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                  D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
                  D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL |
                  D3DPCMPCAPS_ALWAYS;
    c->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
                      D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                      D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA |
                      D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
                      D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                      D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA |
                      D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR;
    c->DestBlendCaps = c->SrcBlendCaps;
    c->AlphaCmpCaps = c->ZCmpCaps;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB |
                   D3DPSHADECAPS_SPECULARGOURAUDRGB |
                   D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                   D3DPSHADECAPS_FOGGOURAUD;
    c->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA |
                     D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE |
                     D3DPTEXTURECAPS_ALPHAPALETTE |
                     D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP |
                     D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP |
                     D3DPTEXTURECAPS_MIPVOLUMEMAP |
                     D3DPTEXTURECAPS_MIPCUBEMAP;
    c->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
                           D3DPTFILTERCAPS_MINFLINEAR |
                           D3DPTFILTERCAPS_MINFANISOTROPIC |
                           D3DPTFILTERCAPS_MIPFPOINT |
                           D3DPTFILTERCAPS_MIPFLINEAR |
                           D3DPTFILTERCAPS_MAGFPOINT |
                           D3DPTFILTERCAPS_MAGFLINEAR |
                           D3DPTFILTERCAPS_MAGFANISOTROPIC;
    c->CubeTextureFilterCaps = c->TextureFilterCaps;
    c->VolumeTextureFilterCaps = c->TextureFilterCaps;
    c->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR |
                            D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
                            D3DPTADDRESSCAPS_INDEPENDENTUV |
                            D3DPTADDRESSCAPS_MIRRORONCE;
    c->VolumeTextureAddressCaps = c->TextureAddressCaps;
    c->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST |
                  D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP |
                  D3DLINECAPS_FOG | D3DLINECAPS_ANTIALIAS;
    c->MaxTextureWidth = 16384;
    c->MaxTextureHeight = 16384;
    c->MaxVolumeExtent = 8192;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 8192;
    c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = -32768.0f;
    c->GuardBandTop = -32768.0f;
    c->GuardBandRight = 32768.0f;
    c->GuardBandBottom = 32768.0f;
    c->ExtentsAdjust = 0.0f;
    c->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
                     D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                     D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
                     D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                     D3DSTENCILCAPS_TWOSIDED;
    c->FVFCaps = (8 & D3DFVFCAPS_TEXCOORDCOUNTMASK) | D3DFVFCAPS_PSIZE;
    c->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
                       D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                       D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X |
                       D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
                       D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT |
                       D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                       D3DTEXOPCAPS_BLENDTEXTUREALPHA |
                       D3DTEXOPCAPS_BLENDFACTORALPHA |
                       D3DTEXOPCAPS_BLENDTEXTUREALPHAPM |
                       D3DTEXOPCAPS_BLENDCURRENTALPHA |
                       D3DTEXOPCAPS_PREMODULATE |
                       D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
                       D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
                       D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
                       D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA |
                       D3DTEXOPCAPS_BUMPENVMAP |
                       D3DTEXOPCAPS_BUMPENVMAPLUMINANCE |
                       D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD |
                       D3DTEXOPCAPS_LERP;
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 |
                              D3DVTXPCAPS_DIRECTIONALLIGHTS |
                              D3DVTXPCAPS_POSITIONALLIGHTS |
                              D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TWEENING |
                              D3DVTXPCAPS_TEXGEN_SPHEREMAP;
    c->MaxActiveLights = 8;
    c->MaxUserClipPlanes = 6;
    c->MaxVertexBlendMatrices = 4;
    c->MaxVertexBlendMatrixIndex = 0;
    c->MaxPointSize = 64.0f;
    c->MaxPrimitiveCount = 0xFFFFF;
    c->MaxVertexIndex = 0xFFFFF;
    c->MaxStreams = 16;
    c->MaxStreamStride = 256;
    c->VertexShaderVersion = D3DVS_VERSION(1, 1);
    c->MaxVertexShaderConst = 96;
    c->PixelShaderVersion = 0;
    c->MaxPixelShaderValue = 1.0f;
}

/* The policy above is what the guest sees.  The real Adreno/DXVK values are
 * logged once so an A/B can answer "did the guest hide a capability, or does
 * the device not have it".  Debug-only; no guest-visible effect. */
static void thin_log_real_caps(void)
{
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    if (!g_dev || !g_d3d) return;
    D3DCAPS9 caps;
    memset(&caps, 0, sizeof caps);
    if (SUCCEEDED(g_dev->GetDeviceCaps(&caps))) {
        THIN_LOG("caps-native VS=%08X PS=%08X maxtex=%ux%u intervals=%08X",
                 (unsigned)caps.VertexShaderVersion,
                 (unsigned)caps.PixelShaderVersion,
                 (unsigned)caps.MaxTextureWidth,
                 (unsigned)caps.MaxTextureHeight,
                 (unsigned)caps.PresentationIntervals);
    }
    const D3DFORMAT probe[] = { D3DFMT_DXT1, D3DFMT_DXT5, D3DFMT_A8R8G8B8 };
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        HRESULT hr = g_d3d->CheckDeviceFormat(
            0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, probe[i]);
        THIN_LOG("caps-check fmt=%u -> %08lX", (unsigned)probe[i],
                 (unsigned long)hr);
    }
#endif
}

/* ------------------------------------------------------------------ */
/* presentation parameters                                            */
/* ------------------------------------------------------------------ */

void thin_pp_from_guest(uint32_t g, D3DPRESENT_PARAMETERS *pp)
{
    const PP32 *p = (const PP32 *)(uintptr_t)g;
    memset(pp, 0, sizeof *pp);
    if (!p) return;
    pp->BackBufferWidth = p->BackBufferWidth;
    pp->BackBufferHeight = p->BackBufferHeight;
    pp->BackBufferFormat = (D3DFORMAT)p->BackBufferFormat;
    pp->BackBufferCount = p->BackBufferCount;
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)p->MultiSampleType;
    pp->MultiSampleQuality = p->MultiSampleQuality;
    pp->SwapEffect = (D3DSWAPEFFECT)p->SwapEffect;
    pp->hDeviceWindow = (HWND)hle_host_hwnd();
    pp->Windowed = p->Windowed ? TRUE : FALSE;
    pp->EnableAutoDepthStencil = p->EnableAutoDepthStencil ? TRUE : FALSE;
    pp->AutoDepthStencilFormat = (D3DFORMAT)p->AutoDepthStencilFormat;
    pp->Flags = p->Flags;
    pp->FullScreen_RefreshRateInHz = p->FullScreen_RefreshRateInHz;
    pp->PresentationInterval = p->FullScreen_PresentationInterval;
}

void thin_pp_to_guest(uint32_t g, const D3DPRESENT_PARAMETERS *pp)
{
    PP32 *p = (PP32 *)(uintptr_t)g;
    if (!p || !pp) return;
    p->BackBufferWidth = pp->BackBufferWidth;
    p->BackBufferHeight = pp->BackBufferHeight;
    p->BackBufferFormat = (uint32_t)pp->BackBufferFormat;
    p->BackBufferCount = pp->BackBufferCount;
    p->MultiSampleType = (uint32_t)pp->MultiSampleType;
    p->MultiSampleQuality = pp->MultiSampleQuality;
    p->SwapEffect = (uint32_t)pp->SwapEffect;
    p->hDeviceWindow = (uint32_t)(uintptr_t)hle_host_hwnd();
    p->Windowed = pp->Windowed ? 1u : 0u;
    p->EnableAutoDepthStencil = pp->EnableAutoDepthStencil ? 1u : 0u;
    p->AutoDepthStencilFormat = (uint32_t)pp->AutoDepthStencilFormat;
    p->Flags = pp->Flags;
    p->FullScreen_RefreshRateInHz = pp->FullScreen_RefreshRateInHz;
    p->FullScreen_PresentationInterval = pp->PresentationInterval;
}

static void thin_build_pp(D3DPRESENT_PARAMETERS *pp, int w, int h, int windowed,
                          int vsync, uint32_t bb_fmt, uint32_t depth_fmt)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = (UINT)w;
    pp->BackBufferHeight = (UINT)h;
    pp->BackBufferFormat = (D3DFORMAT)bb_fmt;
    pp->BackBufferCount = 1;
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->MultiSampleQuality = 0;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = (HWND)hle_host_hwnd();
    pp->Windowed = windowed ? TRUE : FALSE;
    pp->EnableAutoDepthStencil = depth_fmt != D3DFMT_UNKNOWN;
    pp->AutoDepthStencilFormat = (D3DFORMAT)depth_fmt;
    pp->Flags = 0;
    pp->FullScreen_RefreshRateInHz = windowed ? 0 : D3DPRESENT_RATE_DEFAULT;
    pp->PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE
                                     : D3DPRESENT_INTERVAL_IMMEDIATE;
}

/* ------------------------------------------------------------------ */
/* DXVK creation and device lifetime                                  */
/* ------------------------------------------------------------------ */

static IDirect3D9 *thin_create_d3d9(void)
{
    int forced = 0;
    const char *e = getenv("WC3_DXVK_VERSION");
    if (e && *e) {
        if (*e == '1') forced = 1;
        else if (*e == '3') forced = 3;
    }
    wc3_dxvk_prepare();
    int major = 0, minor = 0;
    int have_vk = wc3_vulkan_major_minor(&major, &minor);
    if (!forced) {
        if (!have_vk || major < 1 || (major == 1 && minor < 3)) forced = 1;
        else forced = 3;
    }
    THIN_LOG("Vulkan %d.%d%s, trying DXVK %s", major, minor,
             have_vk ? "" : " (unknown)", forced == 1 ? "1.10.3" : "3.1.1");
    THIN_LOG("android window at Direct3DCreate9: %p (hwnd=%p)",
             wc3_android_window(), hle_host_hwnd());

    IDirect3D9 *d3d = NULL;
    /* DXVK reports setup failures as dxvk::DxvkError, which does not derive
     * from std::exception.  A catch-all is what lets a failed 3.1.1 attempt
     * fall back to 1.10.3 instead of terminating the process. */
    try {
        d3d = (forced == 1) ? wc3_dxvk1_Direct3DCreate9(D3D_SDK_VERSION)
                            : wc3_dxvk3_Direct3DCreate9(D3D_SDK_VERSION);
    } catch (...) {
        THIN_LOG("Direct3DCreate9 v%d threw", forced);
        d3d = NULL;
    }
    if (!d3d && forced == 3) {
        THIN_LOG("3.1.1 could not create a device; falling back to 1.10.3");
        try {
            d3d = wc3_dxvk1_Direct3DCreate9(D3D_SDK_VERSION);
        } catch (...) {
            d3d = NULL;
        }
    }
    if (!d3d) THIN_LOG("no DXVK version could create IDirect3D9");
    return d3d;
}

static uint32_t thin_device_wrapper(void)
{
    if (g_device_obj) return g_device_obj;
    if (!g_dev) return 0;
    /* Reuse the existing device record if the guest released its handle: the
     * device itself is still alive, only the 32-bit wrapper was retired. */
    for (int i = 0; i < THIN_MAX_REC; i++) {
        ThinObj *r = &g_rec[i];
        if (!(r->flags & THIN_F_USED) || r->host != g_dev) continue;
        if (r->iface != D3D9_IFACE_IDirect3DDevice9) continue;
        r->refs = 1;
        r->flags &= (uint16_t)~THIN_F_DETACHED;
        g_device_obj = r->guest;
        return r->guest;
    }
    g_device_obj = thin_wrap(g_dev, D3D9_IFACE_IDirect3DDevice9, 1);
    if (!g_device_obj) {
        /* thin_wrap(take_ref=1) released the CreateDevice reference on
         * failure.  Leaving g_dev pointing at it would hand a dangling
         * device to the next Reset/CreateDevice. */
        g_dev = NULL;
    }
    return g_device_obj;
}

/* ------------------------------------------------------------------ */
/* synthetic input hook (WC3_CLICK_AT / WC3_KEY_AT)                   */
/* ------------------------------------------------------------------ */

static void thin_after_present(void)
{
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    static int parsed;
    static long click_at[8];
    static int click_x[8], click_y[8];
    static int nclicks;
    static long key_at[8];
    static int key_vk[8];
    static int nkeys;
    static int release_click;
    static int release_key = -1;

    if (!parsed) {
        parsed = 1;
        const char *s = getenv("WC3_CLICK_AT");
        while (s && *s && nclicks < 8) {
            long f = -1;
            int x = 0, y = 0;
            if (sscanf(s, "%ld:%d,%d", &f, &x, &y) == 3 && f >= 0) {
                click_at[nclicks] = f;
                click_x[nclicks] = x;
                click_y[nclicks] = y;
                nclicks++;
            }
            s = strchr(s, '|');
            if (!s) break;
            s++;
        }
        s = getenv("WC3_KEY_AT");
        while (s && *s && nkeys < 8) {
            long f = -1;
            int vk = 0;
            if (sscanf(s, "%ld:%d", &f, &vk) == 2 && f >= 0) {
                key_at[nkeys] = f;
                key_vk[nkeys] = vk;
                nkeys++;
            }
            s = strchr(s, '|');
            if (!s) break;
            s++;
        }
    }

    if (release_click) { hle_android_mouse_button(0); release_click = 0; }
    if (release_key >= 0) { hle_android_key_event(release_key, 0); release_key = -1; }

    for (int i = 0; i < nclicks; i++) {
        if (click_at[i] != (long)g_presents) continue;
        int w = (int)(g_pp.BackBufferWidth ? g_pp.BackBufferWidth : 800);
        int h = (int)(g_pp.BackBufferHeight ? g_pp.BackBufferHeight : 600);
        if (w <= 0) w = 800;
        if (h <= 0) h = 600;
        hle_android_mouse_move((float)click_x[i] / (float)w,
                               (float)click_y[i] / (float)h);
        hle_android_mouse_button(1);
        release_click = 1;
    }
    for (int i = 0; i < nkeys; i++) {
        if (key_at[i] != (long)g_presents) continue;
        hle_android_key_event(key_vk[i], 1);
        release_key = key_vk[i];
    }
#endif
}

/* ------------------------------------------------------------------ */
/* handwritten dispatch                                               */
/* ------------------------------------------------------------------ */

static void thin_query_interface(CPU *c, ThinObj *o)
{
    uint32_t iidp = thin_arg(c, 1);
    uint32_t outp = thin_arg(c, 2);
    if (!iidp || !hle_android_guest_ok(iidp, 16) ||
        !outp || !hle_android_guest_ok(outp, 4)) {
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    const uint8_t *want = (const uint8_t *)(uintptr_t)iidp;
    const ThinIid *hit = NULL;
    for (unsigned i = 0; i < k_thin_iid_count; i++) {
        if (memcmp(want, k_thin_iids[i].bytes, 16) == 0) {
            hit = &k_thin_iids[i];
            break;
        }
    }
    if (!hit) {
        wr32g(outp, 0);
        c->eax = E_NOINTERFACE;
        return;
    }

    if (hit->iface == -2) {
        /* IUnknown / Resource9: same object, concrete vtable. */
        wr32g(outp, o->guest);
        c->eax = (uint32_t)thin_obj_addref(o->guest);
        return;
    }
    if (hit->iface == -3) {
        int kind = iface_from_type(o->host);
        if (kind < 0) {
            wr32g(outp, 0);
            c->eax = E_NOINTERFACE;
            return;
        }
        guest_set_iface(o, kind);
        wr32g(outp, o->guest);
        c->eax = (uint32_t)thin_obj_addref(o->guest);
        return;
    }
    if (hit->iface == (int)o->iface) {
        wr32g(outp, o->guest);
        c->eax = (uint32_t)thin_obj_addref(o->guest);
        return;
    }

    /* Ask DXVK; it is the only authority on whether the object implements the
     * interface.  The returned reference is owned by the new wrapper. */
    void *p = NULL;
    HRESULT hr = ((IUnknown *)o->host)->QueryInterface(
        *reinterpret_cast<const IID *>(want), &p);
    if (FAILED(hr) || !p) {
        wr32g(outp, 0);
        c->eax = E_NOINTERFACE;
        return;
    }
    uint32_t g = thin_wrap(p, hit->iface, 1);
    if (!g) {
        /* thin_wrap(take_ref=1) already released the QueryInterface
         * reference on failure; releasing it here would drop DXVK's
         * reference twice and free the object under the device. */
        wr32g(outp, 0);
        c->eax = E_OUTOFMEMORY;
        return;
    }
    wr32g(outp, g);
    c->eax = D3D_OK;
}

static void thin_get_container(CPU *c, ThinObj *o)
{
    uint32_t iidp = thin_arg(c, 1);
    uint32_t outp = thin_arg(c, 2);
    if (!iidp || !hle_android_guest_ok(iidp, 16) ||
        !outp || !hle_android_guest_ok(outp, 4)) {
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    const uint8_t *want = (const uint8_t *)(uintptr_t)iidp;
    int kind = -1;
    for (unsigned i = 0; i < k_thin_iid_count; i++) {
        if (memcmp(want, k_thin_iids[i].bytes, 16) == 0) {
            kind = k_thin_iids[i].iface;
            break;
        }
    }
    /* GetContainer is not QueryInterface: it returns the *parent* (the
     * texture that owns a surface, the device that owns a texture), so it has
     * to be forwarded to the native method rather than answered from the
     * registry. */
    void *p = NULL;
    HRESULT hr;
    if (o->iface == D3D9_IFACE_IDirect3DVolume9)
        hr = ((IDirect3DVolume9 *)o->host)->GetContainer(
            *reinterpret_cast<const IID *>(want), &p);
    else
        hr = ((IDirect3DSurface9 *)o->host)->GetContainer(
            *reinterpret_cast<const IID *>(want), &p);
    if (FAILED(hr) || !p) {
        wr32g(outp, 0);
        c->eax = E_NOINTERFACE;
        return;
    }
    uint32_t g = thin_wrap(p, kind, 1);
    if (!g) {
        /* As in QueryInterface: take_ref=1 means thin_wrap owns and has
         * already released the reference on failure.  The parent was already
         * returned by the native method, so a wrapping failure is exhaustion,
         * not an absent interface. */
        wr32g(outp, 0);
        c->eax = E_OUTOFMEMORY;
        return;
    }
    wr32g(outp, g);
    c->eax = D3D_OK;
}

void thin_hw_call(CPU *c, const ThinSlot *s, ThinObj *o)
{
    switch (s->key) {

    /* ---- IUnknown, shared by every interface ----------------------- */
    case 0x0000: case 0x0100: case 0x0200: case 0x0300: case 0x0400:
    case 0x0500: case 0x0600: case 0x0700: case 0x0800: case 0x0900:
    case 0x0a00: case 0x0b00: case 0x0c00: case 0x0d00: case 0x0e00:
    case 0x0f00:
        thin_query_interface(c, o);
        return;
    case 0x0001: case 0x0101: case 0x0201: case 0x0301: case 0x0401:
    case 0x0501: case 0x0601: case 0x0701: case 0x0801: case 0x0901:
    case 0x0a01: case 0x0b01: case 0x0c01: case 0x0d01: case 0x0e01:
    case 0x0f01:
        c->eax = thin_obj_addref(o->guest);
        return;
    case 0x0002: case 0x0102: case 0x0202: case 0x0302: case 0x0402:
    case 0x0502: case 0x0602: case 0x0702: case 0x0802: case 0x0902:
    case 0x0a02: case 0x0b02: case 0x0c02: case 0x0d02: case 0x0e02:
    case 0x0f02:
        c->eax = thin_obj_release(o->guest);
        return;

    /* ---- IDirect3D9 ------------------------------------------------ */
    case 0x0004: /* GetAdapterCount */
        c->eax = 1;
        return;
    case 0x0005: { /* GetAdapterIdentifier */
        uint32_t out = thin_arg(c, 3);
        if (!out || !hle_android_guest_ok(out, sizeof(AdapterId32))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        AdapterId32 *id = (AdapterId32 *)(uintptr_t)out;
        memset(id, 0, sizeof *id);
        strncpy(id->Driver, "wc3recomp-thin", sizeof id->Driver - 1);
        strncpy(id->Description, "Warcraft III static recompiler (thin D3D9)",
                sizeof id->Description - 1);
        strncpy(id->DeviceName, "\\\\.\\DISPLAY1", sizeof id->DeviceName - 1);
        id->VendorId = 0x10DE;
        id->DriverVersion.QuadPart = 0x0009000000000000ll;
        c->eax = D3D_OK;
        return;
    }
    case 0x0006: /* GetAdapterModeCount */
        c->eax = (uint32_t)thin_mode_count_for(thin_arg(c, 2));
        return;
    case 0x0007: { /* EnumAdapterModes */
        uint32_t fmt = thin_arg(c, 2);
        int idx = (int)thin_arg(c, 3);
        uint32_t out = thin_arg(c, 4);
        const D3DDISPLAYMODE *m = thin_mode_for(fmt, idx);
        if (!m || !out || !hle_android_guest_ok(out, sizeof(D3DDISPLAYMODE))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        *(D3DDISPLAYMODE *)(uintptr_t)out = *m;
        c->eax = D3D_OK;
        return;
    }
    case 0x0008: { /* GetAdapterDisplayMode */
        uint32_t out = thin_arg(c, 2);
        if (!out || !hle_android_guest_ok(out, sizeof(D3DDISPLAYMODE))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        thin_fill_display((D3DDISPLAYMODE *)(uintptr_t)out);
        c->eax = D3D_OK;
        return;
    }
    case 0x0009: case 0x000a: case 0x000b: case 0x000c: case 0x000d:
        /* The renderer accepts every format the engine asks for; a real
         * CheckDevice* would let DXVK reject a format WC3 then cannot fall
         * back from.  This is the default backend's policy. */
        c->eax = D3D_OK;
        return;
    case 0x000e: { /* GetDeviceCaps */
        uint32_t out = thin_arg(c, 3);
        if (!out || !hle_android_guest_ok(out, sizeof(Caps9))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        thin_fill_caps((Caps9 *)(uintptr_t)out, thin_arg(c, 1),
                       (D3DDEVTYPE)thin_arg(c, 2));
        thin_count(THIN_STAT_CAPS);
        c->eax = D3D_OK;
        return;
    }
    case 0x000f: /* GetAdapterMonitor */
        c->eax = 1;
        return;
    case 0x0010: { /* CreateDevice */
        uint32_t ppg = thin_arg(c, 5);
        uint32_t out = thin_arg(c, 6);
        if (!ppg || !hle_android_guest_ok(ppg, sizeof(PP32)) ||
            !out || !hle_android_guest_ok(out, 4)) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        const PP32 *p = (const PP32 *)(uintptr_t)ppg;
        g_pp = *p;
        int w = (int)p->BackBufferWidth;
        int h = (int)p->BackBufferHeight;
        if (!w || !h) thin_desktop_mode(&w, &h, NULL);
        int windowed = p->Windowed ? 1 : 0;
        if (getenv("WC3_HIDDEN")) windowed = 1;
        int vsync = p->FullScreen_PresentationInterval !=
                    D3DPRESENT_INTERVAL_IMMEDIATE;
        uint32_t bb_fmt = p->BackBufferFormat ? p->BackBufferFormat
                                              : (uint32_t)D3DFMT_X8R8G8B8;
        uint32_t depth_fmt = p->EnableAutoDepthStencil
                                 ? p->AutoDepthStencilFormat
                                 : (uint32_t)D3DFMT_UNKNOWN;

        hle_android_select_display_mode((uint32_t)w, (uint32_t)h);
        thin_count(THIN_STAT_CREATE_DEVICE);
        THIN_LOG("CreateDevice %dx%d windowed=%d fmt=%u depth=%u reuse=%d "
                 "retired=%d", w, h, windowed, bb_fmt, depth_fmt,
                 g_dev != NULL, g_device_retired);

        if (g_dev) {
            D3DPRESENT_PARAMETERS pp;
            thin_build_pp(&pp, w, h, windowed, vsync, bb_fmt, depth_fmt);
            thin_detach_all();
            HRESULT hr;
            try {
                hr = g_dev->Reset(&pp);
            } catch (...) {
                hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                g_device_retired = 0;
                uint32_t g = thin_device_wrapper();
                if (!g) {
                    c->eax = E_OUTOFMEMORY;
                    return;
                }
                wr32g(out, g);
                thin_log_real_caps();
                hle_android_after_d3d9(c);
                c->eax = D3D_OK;
                return;
            }
            THIN_LOG("CreateDevice reset failed hr=%08lX; recreating",
                     (unsigned long)hr);
            g_dev->Release();
            g_dev = NULL;
            g_device_retired = 0;
            g_device_obj = 0;
        }

        if (!g_d3d) g_d3d = thin_create_d3d9();
        if (!g_d3d) {
            c->eax = D3DERR_NOTAVAILABLE;
            return;
        }
        D3DPRESENT_PARAMETERS pp;
        thin_build_pp(&pp, w, h, windowed, vsync, bb_fmt, depth_fmt);
        THIN_LOG("CreateDevice: window=%p hwnd=%p", wc3_android_window(),
                 (void *)pp.hDeviceWindow);
        IDirect3DDevice9 *dev = NULL;
        HRESULT hr;
        try {
            hr = g_d3d->CreateDevice(0, D3DDEVTYPE_HAL,
                                     (HWND)hle_host_hwnd(),
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                         D3DCREATE_FPU_PRESERVE,
                                     &pp, &dev);
        } catch (...) {
            hr = E_FAIL;
            THIN_LOG("CreateDevice threw");
        }
        if (FAILED(hr) || !dev) {
            THIN_LOG("CreateDevice failed hr=%08lX window now=%p",
                     (unsigned long)hr, wc3_android_window());
            c->eax = D3DERR_NOTAVAILABLE;
            return;
        }
        g_dev = dev;
        g_device_retired = 0;
        uint32_t g = thin_device_wrapper();
        if (!g) {
            c->eax = E_OUTOFMEMORY;
            return;
        }
        wr32g(out, g);
        THIN_LOG("CreateDevice -> guest=%08X", g);
        thin_log_real_caps();
        hle_android_after_d3d9(c);
        c->eax = D3D_OK;
        return;
    }

    /* ---- IDirect3DDevice9 ----------------------------------------- */
    case 0x0104: /* GetAvailableTextureMem */
        c->eax = 256u * 1024u * 1024u;
        return;
    case 0x0107: { /* GetDeviceCaps */
        uint32_t out = thin_arg(c, 1);
        if (!out || !hle_android_guest_ok(out, sizeof(Caps9))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        thin_fill_caps((Caps9 *)(uintptr_t)out, 0, D3DDEVTYPE_HAL);
        thin_count(THIN_STAT_CAPS);
        c->eax = D3D_OK;
        return;
    }
    case 0x0108: { /* GetDisplayMode */
        uint32_t out = thin_arg(c, 2);
        if (!out || !hle_android_guest_ok(out, sizeof(D3DDISPLAYMODE))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        thin_fill_display((D3DDISPLAYMODE *)(uintptr_t)out);
        c->eax = D3D_OK;
        return;
    }
    case 0x0109: { /* GetCreationParameters */
        uint32_t out = thin_arg(c, 1);
        if (!out || !hle_android_guest_ok(out, sizeof(Creation32))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        Creation32 *p = (Creation32 *)(uintptr_t)out;
        p->AdapterOrdinal = 0;
        p->DeviceType = D3DDEVTYPE_HAL;
        p->hFocusWindow = (uint32_t)(uintptr_t)hle_host_hwnd();
        p->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
        c->eax = D3D_OK;
        return;
    }
    case 0x010d: { /* CreateAdditionalSwapChain */
        uint32_t ppg = thin_arg(c, 1);
        if (!ppg || !hle_android_guest_ok(ppg, sizeof(PP32))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        D3DPRESENT_PARAMETERS pp;
        thin_pp_from_guest(ppg, &pp);
        IDirect3DSwapChain9 *sc = NULL;
        HRESULT hr;
        try {
            hr = g_dev ? g_dev->CreateAdditionalSwapChain(&pp, &sc)
                       : D3DERR_INVALIDCALL;
        } catch (...) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) thin_after_out(c, 2, sc, D3D9_IFACE_IDirect3DSwapChain9);
        c->eax = (uint32_t)hr;
        return;
    }
    case 0x010f: /* GetNumberOfSwapChains */
        c->eax = 1;
        return;
    case 0x0110: { /* Reset */
        uint32_t ppg = thin_arg(c, 1);
        if (!ppg || !hle_android_guest_ok(ppg, sizeof(PP32))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        const PP32 *p = (const PP32 *)(uintptr_t)ppg;
        g_pp = *p;
        int w = (int)p->BackBufferWidth;
        int h = (int)p->BackBufferHeight;
        if (!w || !h) thin_desktop_mode(&w, &h, NULL);
        int windowed = p->Windowed ? 1 : 0;
        if (getenv("WC3_HIDDEN")) windowed = 1;
        int vsync = p->FullScreen_PresentationInterval !=
                    D3DPRESENT_INTERVAL_IMMEDIATE;
        uint32_t bfmt = p->BackBufferFormat ? p->BackBufferFormat
                                            : (uint32_t)D3DFMT_X8R8G8B8;
        uint32_t dfmt = p->EnableAutoDepthStencil ? p->AutoDepthStencilFormat
                                                  : (uint32_t)D3DFMT_UNKNOWN;
        hle_android_select_display_mode((uint32_t)w, (uint32_t)h);
        D3DPRESENT_PARAMETERS pp;
        thin_build_pp(&pp, w, h, windowed, vsync, bfmt, dfmt);
        thin_detach_all();
        thin_count(THIN_STAT_RESET);
        HRESULT hr;
        try {
            hr = g_dev ? g_dev->Reset(&pp) : D3DERR_INVALIDCALL;
        } catch (...) {
            hr = E_FAIL;
        }
        THIN_LOG("Reset %dx%d -> %08lX", w, h, (unsigned long)hr);
        c->eax = (uint32_t)hr;
        return;
    }
    case 0x0111: { /* Present */
        HRESULT hr;
        try {
            hr = g_dev ? g_dev->Present(NULL, NULL, NULL, NULL)
                       : D3DERR_INVALIDCALL;
        } catch (...) {
            hr = E_FAIL;
        }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
        g_presents++;
        if ((g_presents & 511u) == 0) thin_dump_stats();
#endif
        thin_count(THIN_STAT_PRESENT);
        thin_after_present();
        c->eax = (uint32_t)hr;
        return;
    }
    case 0x0113: { /* GetRasterStatus */
        uint32_t out = thin_arg(c, 2);
        if (!out || !hle_android_guest_ok(out, sizeof(D3DRASTER_STATUS))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        D3DRASTER_STATUS *r = (D3DRASTER_STATUS *)(uintptr_t)out;
        r->InVBlank = TRUE;
        r->ScanLine = 0;
        c->eax = D3D_OK;
        return;
    }

    /* ---- IDirect3DSwapChain9 -------------------------------------- */
    case 0x0303: { /* Present */
        HRESULT hr;
        try {
            hr = ((IDirect3DSwapChain9 *)o->host)->Present(
                (const RECT *)(uintptr_t)thin_arg(c, 1),
                (const RECT *)(uintptr_t)thin_arg(c, 2), NULL,
                (const RGNDATA *)(uintptr_t)thin_arg(c, 4),
                (DWORD)thin_arg(c, 5));
        } catch (...) {
            hr = E_FAIL;
        }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
        g_presents++;
        if ((g_presents & 511u) == 0) thin_dump_stats();
#endif
        thin_count(THIN_STAT_PRESENT);
        thin_after_present();
        c->eax = (uint32_t)hr;
        return;
    }
    case 0x0306: { /* GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) */
        uint32_t out = thin_arg(c, 1);
        if (!out || !hle_android_guest_ok(out, sizeof(D3DRASTER_STATUS))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        D3DRASTER_STATUS *r = (D3DRASTER_STATUS *)(uintptr_t)out;
        r->InVBlank = TRUE;
        r->ScanLine = 0;
        c->eax = D3D_OK;
        return;
    }
    case 0x0307: { /* GetDisplayMode */
        uint32_t out = thin_arg(c, 1);
        if (!out || !hle_android_guest_ok(out, sizeof(D3DDISPLAYMODE))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        thin_fill_display((D3DDISPLAYMODE *)(uintptr_t)out);
        c->eax = D3D_OK;
        return;
    }
    case 0x0309: { /* GetPresentParameters */
        uint32_t out = thin_arg(c, 1);
        if (!out || !hle_android_guest_ok(out, sizeof(PP32))) {
            c->eax = D3DERR_INVALIDCALL;
            return;
        }
        D3DPRESENT_PARAMETERS pp;
        memset(&pp, 0, sizeof pp);
        pp.BackBufferWidth = g_pp.BackBufferWidth;
        pp.BackBufferHeight = g_pp.BackBufferHeight;
        pp.BackBufferFormat = (D3DFORMAT)g_pp.BackBufferFormat;
        pp.BackBufferCount = g_pp.BackBufferCount;
        pp.MultiSampleType = (D3DMULTISAMPLE_TYPE)g_pp.MultiSampleType;
        pp.MultiSampleQuality = g_pp.MultiSampleQuality;
        pp.SwapEffect = (D3DSWAPEFFECT)g_pp.SwapEffect;
        pp.hDeviceWindow = (HWND)hle_host_hwnd();
        pp.Windowed = g_pp.Windowed ? TRUE : FALSE;
        pp.EnableAutoDepthStencil = g_pp.EnableAutoDepthStencil ? TRUE : FALSE;
        pp.AutoDepthStencilFormat = (D3DFORMAT)g_pp.AutoDepthStencilFormat;
        pp.Flags = g_pp.Flags;
        pp.FullScreen_RefreshRateInHz = g_pp.FullScreen_RefreshRateInHz;
        pp.PresentationInterval = g_pp.FullScreen_PresentationInterval;
        thin_pp_to_guest(out, &pp);
        c->eax = D3D_OK;
        return;
    }

    /* ---- GetContainer --------------------------------------------- */
    case 0x070b: case 0x0b07:
        thin_get_container(c, o);
        return;

    /* ---- locks ----------------------------------------------------- */
    case 0x050b: /* IDirect3DVertexBuffer9::Lock */
    case 0x060b: /* IDirect3DIndexBuffer9::Lock */
        c->eax = thin_lock_buffer(c, o, thin_arg(c, 1), thin_arg(c, 2),
                                  thin_arg(c, 4), thin_arg(c, 3));
        return;
    case 0x050c: case 0x060c: /* Unlock */
        c->eax = thin_unlock_buffer(c, o);
        return;
    case 0x070d: /* IDirect3DSurface9::LockRect(pLocked, pRect, flags) */
        c->eax = thin_lock_rect(c, o, THIN_LK_SURFACE, 0, thin_arg(c, 3),
                                thin_arg(c, 2), thin_arg(c, 1), 0);
        return;
    case 0x070e: /* UnlockRect */
        c->eax = thin_unlock_rect(c, o, THIN_LK_SURFACE, 0);
        return;
    case 0x0800 + 0x13: /* IDirect3DTexture9::LockRect */
        c->eax = thin_lock_rect(c, o, THIN_LK_TEXTURE, thin_arg(c, 1),
                                thin_arg(c, 4), thin_arg(c, 3),
                                thin_arg(c, 2), 0);
        return;
    case 0x0800 + 0x14: /* UnlockRect */
        c->eax = thin_unlock_rect(c, o, THIN_LK_TEXTURE, thin_arg(c, 1));
        return;
    case 0x0900 + 0x13: /* VolumeTexture::LockBox */
        c->eax = thin_lock_rect(c, o, THIN_LK_VOLTEX, thin_arg(c, 1),
                                thin_arg(c, 4), thin_arg(c, 3),
                                thin_arg(c, 2), 1);
        return;
    case 0x0900 + 0x14: /* UnlockBox */
        c->eax = thin_unlock_rect(c, o, THIN_LK_VOLTEX, thin_arg(c, 1));
        return;
    case 0x0a00 + 0x13: { /* CubeTexture::LockRect(face, level, ...) */
        uint32_t face = thin_arg(c, 1), level = thin_arg(c, 2);
        c->eax = thin_lock_rect(c, o, THIN_LK_CUBE, (face << 16) | (level & 0xFFFF),
                                thin_arg(c, 5), thin_arg(c, 4),
                                thin_arg(c, 3), 0);
        return;
    }
    case 0x0a00 + 0x14: { /* UnlockRect(face, level) */
        uint32_t face = thin_arg(c, 1), level = thin_arg(c, 2);
        c->eax = thin_unlock_rect(c, o, THIN_LK_CUBE,
                                  (face << 16) | (level & 0xFFFF));
        return;
    }
    case 0x0b09: /* IDirect3DVolume9::LockBox(pLocked, pBox, flags) */
        c->eax = thin_lock_rect(c, o, THIN_LK_VOLUME, 0, thin_arg(c, 3),
                                thin_arg(c, 2), thin_arg(c, 1), 1);
        return;
    case 0x0b0a: /* UnlockBox */
        c->eax = thin_unlock_rect(c, o, THIN_LK_VOLUME, 0);
        return;

    /* ---- GetDC / ReleaseDC: not representable in 32-bit guest memory ---- */
    case 0x070f: case 0x0710:
        c->eax = D3DERR_INVALIDCALL;
        return;

    default:
        THIN_LOG("handwritten dispatch missing key %04X", s->key);
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* thunk entry and vtable construction                                */
/* ------------------------------------------------------------------ */

static void thin_entry(CPU *c)
{
    HleEnt *e = hle_entry(c->eip);
    const ThinSlot *s = e ? (const ThinSlot *)e->fwd : NULL;
    if (!s) {
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    thin_account(s->key);
#endif
    thin_gfx_event(c, s->key);

    uint32_t self = thin_arg(c, 0);
    if (!self || !hle_android_guest_ok(self, sizeof(ThinGuestObj))) {
        THIN_LOG("null/unmapped this (%s)", hle_describe(c->eip));
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    ThinGuestObj *go = (ThinGuestObj *)(uintptr_t)self;
    if (go->magic != THIN_MAGIC || go->id >= THIN_MAX_REC) {
        THIN_LOG("bad guest COM object %08X (%s)", self, hle_describe(c->eip));
        c->eax = D3DERR_INVALIDCALL;
        return;
    }
    ThinObj *o = &g_rec[go->id];
    if (!(o->flags & THIN_F_USED) || o->guest != self) {
        THIN_LOG("stale guest COM object %08X (%s)", self, hle_describe(c->eip));
        c->eax = D3DERR_INVALIDCALL;
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    static int trace = -1;
    if (trace < 0) {
        const char *t = getenv("WC3_D3D9_TRACE");
        trace = (t && *t) ? atoi(t) : 0;
    }
    if (trace >= 2) {
        THIN_LOG("call %-44s this=%08X ret=%08X tid=%lu",
                 hle_describe(c->eip), self, rd32g(c->esp),
                 (unsigned long)GetCurrentThreadId());
    }
#endif
    if (s->hw) thin_hw_call(c, s, o);
    else       thin_gen_call(c, s, o);
}

static void thin_build_vtables(void)
{
    if (g_vt_ready) return;
    g_vt_ready = 1;
    for (unsigned f = 0; f < D3D9_IFACE_COUNT; f++) {
        const D3D9Iface *iface = &g_d3d9_ifaces[f];
        uint32_t vtable = g_alloc0(iface->count * 4u);
        g_vt[f] = vtable;
        for (unsigned i = 0; i < iface->count; i++) {
            const D3D9Method *m = &iface->methods[i];
            char name[96];
            snprintf(name, sizeof name, "%s::%s", iface->name, m->name);
            HleEnt *e = reg("d3d9.dll", name, 0, thin_entry, m->argc + 1, 0);
            ThinSlot *s = (ThinSlot *)calloc(1, sizeof *s);
            s->iface = (uint8_t)f;
            s->index = (uint8_t)i;
            s->argc = m->argc;
            s->key = (uint16_t)(f * 256u + i);
            s->hw = (uint16_t)thin_method_hw(s->key);
            e->fwd = s;
            wr32g(vtable + i * 4u, GUEST_HLE_BASE +
                  (uint32_t)(e - g_hle) * HLE_STRIDE);
        }
    }
    THIN_LOG("guest vtable built (%u interfaces, 308 methods)",
             D3D9_IFACE_COUNT);
}

static void thin_direct3d_create9(CPU *c)
{
    thin_build_vtables();
    thin_count(THIN_STAT_D3D9);
    if (!g_d3d) g_d3d = thin_create_d3d9();
    if (!g_d3d) {
        c->eax = 0;
        return;
    }
    if (!g_d3d_obj)
        g_d3d_obj = thin_wrap(g_d3d, D3D9_IFACE_IDirect3D9, 0);
    THIN_LOG("Direct3DCreate9(%u) -> guest=%08X (thin bridge)",
             thin_arg(c, 0), g_d3d_obj);
    c->eax = g_d3d_obj;
}

extern "C" void hle_d3d9_thin_register(void)
{
    THIN_MARK("backend: thin");
    /* The 16 guest vtables are built on the first Direct3DCreate9.
     * Registration runs during HLE startup, and doing 308 reg() calls there
     * would move work into the boot milestone for no benefit. */
    reg("d3d9.dll", "Direct3DCreate9", 0, thin_direct3d_create9, 1, 0);
}

/* hle_android.c declares this symbol weak and calls it when the D3D9
 * backend is selected.  In this tree the thin bridge is the only definition,
 * so no duplicate registration is possible. */
extern "C" void hle_register_d3d9(void)
{
    hle_d3d9_thin_register();
}
