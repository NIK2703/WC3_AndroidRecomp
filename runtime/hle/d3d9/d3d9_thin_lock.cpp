/*
 * d3d9_thin_lock.cpp - guest-visible memory for the thin bridge.
 *
 * Ported from the as-built `runtime/host/hle_d3d9_thin_lock.cpp`.  The guest
 * visible semantics are unchanged; the copy path and its instrumentation are
 * the target-owned part (docs/THIN_D3D9_COPY_PLAN.md).
 *
 * The guest is 32-bit; DXVK's LockRect/LockBox/Lock return native 64-bit
 * pointers.  A lock therefore has two possible backing stores:
 *
 *   1. Direct: when DXVK's pointer is already below 4 GB and the range can be
 *      registered with the guest validator, the guest gets the native pointer
 *      itself.  No copy-in, no copy-back; DXVK's mapping is the buffer.  Phase
 *      3 of the plan makes this reachable for the hot dynamic buffers by
 *      placing DXVK's host-visible memory low (`VK_EXT_map_memory_placed`);
 *      without that extension it fires only for memory a driver happens to
 *      place low.
 *   2. Shadow: otherwise a copy lives in a low, committed arena that fits in
 *      32 bits.  Unlock copies the written range back through DXVK's pointer.
 *
 * The shadow arena has to call guest_commit(): the validator behind cr_memcpy
 * (guest_ok in hle_android.c) accepts the static layout table plus ranges
 * committed after startup.  A staging arena that is merely VirtualAlloc'd is
 * invisible to it, and every guest memcpy into a locked buffer is then
 * silently dropped - the defect documented in PROGRESS.md.  The thin backend
 * must not reproduce it.
 *
 * Lock semantics implemented here:
 *   - nested locks: a small stack per (object, subresource); only the
 *     outermost frame talks to DXVK, inner frames return the same shadow;
 *   - D3DLOCK_DISCARD: no copy-in (the old contents are undefined);
 *   - D3DLOCK_READONLY: no copy-back;
 *   - D3DLOCK_NOOVERWRITE: forwarded for buffers, retried without it for
 *     textures if DXVK refuses;
 *   - rect/box locks: the guest pointer is offset into the shadow, and
 *     copy-in/copy-back honour pitch and the rect origin;
 *   - D3DPOOL_SYSTEMMEM: the shadow is kept for the object's lifetime, so a
 *     pointer the guest holds after Unlock stays mapped.
 *
 * WC3_DEBUG_PERF counters measure which path each lock took.  They compile to
 * nothing in release (BV-5/BV-6).
 */
#include "d3d9_thin.h"
#include "wc3_lowmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* debug-only lock accounting                                         */
/* ------------------------------------------------------------------ */

/*
 * Counters answer the questions the copy plan needs answered and nothing
 * else: which lock kind, which flags, how many bytes moved in each direction,
 * whether the lock used the native pointer, and how often the descriptor
 * caches hit.  They are read periodically and at thin_lock_reset().
 */
#if WC3_DEBUG && WC3_DEBUG_PERF
struct ThinLockStats {
    unsigned long long buffer_locks, buffer_unlocks;
    unsigned long long rect_locks, rect_unlocks;
    unsigned long long copy_in_bytes, copy_out_bytes;
    unsigned long long direct_locks;
    unsigned long long discard_locks;
    unsigned long long wo_locks;
    unsigned long long wo_copyin_skipped;
    unsigned long long getdesc_calls, getdesc_hits;
    unsigned long long dims_calls, dims_hits;
    unsigned long long staging_grows;
    unsigned long long low_register_fail;
};
static ThinLockStats g_lk;

#define LK_INC(f) (++g_lk.f)
#define LK_ADD(f, n) (g_lk.f += (unsigned long long)(n))

static void thin_lock_stats_dump(const char *why)
{
    fprintf(stderr,
            "[thin-lock] %s buf=%llu/%llu rect=%llu/%llu copyin=%llu "
            "copyout=%llu direct=%llu discard=%llu wo=%llu wo_skip=%llu "
            "getdesc=%llu/%llu dims=%llu/%llu staging_grow=%llu low_fail=%llu\n",
            why,
            g_lk.buffer_locks, g_lk.buffer_unlocks,
            g_lk.rect_locks, g_lk.rect_unlocks,
            g_lk.copy_in_bytes, g_lk.copy_out_bytes,
            g_lk.direct_locks, g_lk.discard_locks,
            g_lk.wo_locks, g_lk.wo_copyin_skipped,
            g_lk.getdesc_calls, g_lk.getdesc_hits,
            g_lk.dims_calls, g_lk.dims_hits,
            g_lk.staging_grows, g_lk.low_register_fail);
}
#else
#define LK_INC(f) ((void)0)
#define LK_ADD(f, n) ((void)0)
#define thin_lock_stats_dump(why) ((void)0)
#endif

/* A/B switch for "a D3DUSAGE_WRITEONLY buffer does not need the copy-in".
 * D3D9 defines WRITEONLY as "the application will not read from the buffer",
 * so preserving bytes the guest does not touch is not observable to it; the
 * GPU still reads whatever the guest writes.  It stays OFF until the menu /
 * map / gameplay capture shows identical output (docs/THIN_D3D9_COPY_PLAN.md
 * Phase 2).  A debug build can override it for a no-rebuild A/B. */
#ifndef WC3_THIN_SKIP_WRITEONLY_COPYIN
#define WC3_THIN_SKIP_WRITEONLY_COPYIN 0
#endif

static int thin_wo_copyin_skip(void)
{
#if WC3_DEBUG && WC3_DEBUG_PERF
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("WC3_THIN_WO_SKIP");
        mode = e ? (e[0] == '1') : WC3_THIN_SKIP_WRITEONLY_COPYIN;
    }
    return mode;
#else
    return WC3_THIN_SKIP_WRITEONLY_COPYIN;
#endif
}

/* Palette diagnostics are injected into the generated game chunks by the
 * debug corpus.  The production HLE does not implement them; these no-op
 * definitions keep the debug chunks linkable.  The target/gameplay queries
 * return zero so the injected probes stay dormant. */
extern "C" void wc3_paldiag_watch(uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
}
extern "C" void wc3_paldiag_set_target(uint32_t addr)
{
    (void)addr;
}
extern "C" uint32_t wc3_paldiag_get_target(void)
{
    return 0;
}
extern "C" int wc3_paldiag_gameplay(void)
{
    return 0;
}

/* ------------------------------------------------------------------ */
/* low, guest-visible arena                                           */
/* ------------------------------------------------------------------ */

#define THIN_PAGE       0x1000u
#define THIN_ARENA_MAX  0x10000000u      /* 256 MB, same ceiling as d9vk */
#define THIN_ARENA_MIN  0x00800000u
#define THIN_PAGES      (THIN_ARENA_MAX / THIN_PAGE)
#define THIN_WORDS      (THIN_PAGES / 64)

/* Ranges the arena must keep clear.  Kept in step with guest_layout.h; the
 * Android layout is the one this backend runs on.  The low-memory window is
 * reserved for DXVK's placed host-visible mappings: if the staging arena took
 * it first, the direct lock path would never see a low pointer and the copy
 * would stay. */
static const struct { uint32_t lo, hi; } g_keep[] = {
    { 0x00000000u, 0x01000000u },
    { WC3_LOWMEM_BEGIN, WC3_LOWMEM_END },
    { GUEST_STACK_BASE, GUEST_STACK_BASE + GUEST_STACK_SIZE },
    { GUEST_THREAD_STACK_BASE, GUEST_THREAD_STACK_BASE +
      GUEST_THREAD_MAX * GUEST_THREAD_STACK_SIZE },
    { GUEST_IMAGE_STORM, GUEST_IMAGE_MSSFAST + 0x00020000u },
    { GUEST_IMAGE_MP3DEC, GUEST_IMAGE_MP3DEC + 0x00100000u },
    { GUEST_IMAGE_VER, GUEST_IMAGE_VER + 0x00100000u },
    { GUEST_HEAP_BASE, GUEST_HEAP_BASE + GUEST_HEAP_SIZE },
    { GUEST_HLE_HEAP_BASE, GUEST_HLE_HEAP_BASE + GUEST_HLE_HEAP_SIZE },
    { GUEST_VA2_BASE, GUEST_VA2_BASE + GUEST_VA2_SIZE },
    { GUEST_IMAGE_IJL, GUEST_IMAGE_IJL + 0x00100000u },
    { GUEST_VA_BASE, GUEST_VA_BASE + GUEST_VA_SIZE },
    /* The five high VA arenas exist only in the Android layout; on the
     * reference layout these macros are absent. */
#ifdef GUEST_VA3_BASE
    { GUEST_VA3_BASE, GUEST_VA3_BASE + GUEST_VA3_SIZE },
    { GUEST_VA4_BASE, GUEST_VA4_BASE + GUEST_VA4_SIZE },
    { GUEST_VA5_BASE, GUEST_VA5_BASE + GUEST_VA5_SIZE },
    { GUEST_VA6_BASE, GUEST_VA6_BASE + GUEST_VA6_SIZE },
    { GUEST_VA7_BASE, GUEST_VA7_BASE + GUEST_VA7_SIZE },
#endif
    { GUEST_TEB_BASE, GUEST_TEB_BASE + 0x00010000u },
    { GUEST_THREAD_TEB_BASE, GUEST_THREAD_TEB_BASE +
      GUEST_THREAD_MAX * 0x00001000u },
    { GUEST_IMAGE_GAME, GUEST_IMAGE_GAME + 0x01000000u },
    { GUEST_HLE_BASE, GUEST_HLE_BASE + 0x00200000u },
    { 0x7FD00000u, 0x80000000u },
};

static uint64_t g_map[THIN_WORDS];
static uint32_t g_base;
static uint32_t g_size;
static uint32_t g_npages;
static uint32_t g_hint;
static int g_state;
static uint64_t g_used;

static int low_ready(void)
{
    if (g_state) return g_state > 0;
    g_state = -1;
    uintptr_t best_lo = 0, best_size = 0, big_lo = 0, big_size = 0;
    uintptr_t a = 0x00100000;
    const uintptr_t limit = 0x7F000000;
    while (a < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= THIN_ARENA_MIN) {
            uintptr_t lo = (uintptr_t)mbi.BaseAddress;
            uintptr_t hi = lo + mbi.RegionSize;
            uintptr_t cur = lo;
            while (cur < hi) {
                uintptr_t jump = 0, cut = hi;
                for (size_t i = 0; i < sizeof g_keep / sizeof g_keep[0]; i++) {
                    uintptr_t klo = (uintptr_t)g_keep[i].lo;
                    uintptr_t khi = (uintptr_t)g_keep[i].hi;
                    if (cur >= klo && cur < khi) {
                        if (khi > jump) jump = khi;
                    } else if (klo > cur && klo < cut) {
                        cut = klo;
                    }
                }
                if (jump > cur) { cur = jump; continue; }
                size_t seg = cut - cur;
                if (seg >= THIN_ARENA_MIN) {
                    if (!best_lo || seg > best_size) { best_lo = cur; best_size = seg; }
                    if (seg >= THIN_ARENA_MAX && !big_lo) { big_lo = cur; big_size = seg; }
                }
                cur = cut;
            }
        }
        a = next;
    }
    uintptr_t lo = big_lo ? big_lo : best_lo;
    size_t want = big_lo ? big_size : best_size;
    if (lo) {
        if (want > THIN_ARENA_MAX) want = THIN_ARENA_MAX;
        want &= ~(size_t)(THIN_PAGE - 1);
        void *p = VirtualAlloc((void *)lo, want, MEM_RESERVE, PAGE_NOACCESS);
        if (p) {
            g_base = (uint32_t)(uintptr_t)p;
            g_size = (uint32_t)want;
            g_npages = g_size / THIN_PAGE;
            g_state = 1;
            THIN_LOG("staging arena %08X+%X (%u pages)",
                     g_base, g_size, g_npages);
            return 1;
        }
    }
    THIN_LOG("staging arena unavailable");
    return 0;
}

static int page_used(uint32_t i) { return (int)((g_map[i >> 6] >> (i & 63)) & 1u); }

static void page_set(uint32_t i, int used)
{
    uint64_t bit = (uint64_t)1 << (i & 63);
    uint64_t *w = &g_map[i >> 6];
    if (used) {
        if (!(*w & bit)) { *w |= bit; g_used += THIN_PAGE; }
    } else if (*w & bit) {
        *w &= ~bit;
        g_used -= THIN_PAGE;
    }
}

static void *low_alloc(size_t n)
{
    if (!n) n = 1;
    if (!low_ready()) return NULL;
    uint32_t pages = (uint32_t)((n + THIN_PAGE - 1) / THIN_PAGE);
    if (pages > g_npages) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t i = pass ? 0 : g_hint;
        uint32_t lim = pass ? g_hint : g_npages;
        while (i + pages <= lim) {
            uint32_t run = 0;
            while (run < pages && !page_used(i + run)) run++;
            if (run == pages) {
                for (uint32_t k = 0; k < pages; k++) page_set(i + k, 1);
                g_hint = i + pages;
                void *p = (void *)(uintptr_t)(g_base + i * THIN_PAGE);
                size_t span = (size_t)pages * THIN_PAGE;
                if (!guest_commit((uint32_t)(uintptr_t)p, (uint32_t)span)) {
                    for (uint32_t k = 0; k < pages; k++) page_set(i + k, 0);
                    THIN_LOG("staging commit %08X+%X failed",
                             (uint32_t)(uintptr_t)p, (uint32_t)span);
                    return NULL;
                }
                memset(p, 0, span);
                return p;
            }
            i += run + 1;
        }
    }
    THIN_LOG("staging exhausted (%llu bytes, %llu used of %u)",
             (unsigned long long)n, (unsigned long long)g_used, g_size);
    return NULL;
}

static void low_free(void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    if (!p) return;
    if (a < g_base || a >= (uintptr_t)g_base + g_size) return;
    uint32_t first = (uint32_t)((a - g_base) / THIN_PAGE);
    uint32_t pages = (uint32_t)((n + THIN_PAGE - 1) / THIN_PAGE);
    if (!pages) pages = 1;
    for (uint32_t k = 0; k < pages && first + k < g_npages; k++)
        page_set(first + k, 0);
    if (first < g_hint) g_hint = first;
}

/* ------------------------------------------------------------------ */
/* direct low-pointer path                                            */
/* ------------------------------------------------------------------ */

/*
 * DXVK's mapped buffer memory is ordinary host memory.  When the driver
 * placed it below 4 GB the guest can use that pointer directly: the guest
 * writes become DXVK's writes, and Unlock has nothing to copy.
 *
 * guest_range_register() only teaches the validator about the range; it does
 * not mprotect, because the pages belong to the Vulkan driver's mapping.
 */
static int low_buffer_usable(void *ptr, uint32_t off, uint32_t size,
                             uint32_t total)
{
    uintptr_t a = (uintptr_t)ptr;
    if (!a || a >= 0x100000000ull) return 0;
    if ((uint64_t)a + size > 0x100000000ull) return 0;
    if (hle_android_guest_ok((uint32_t)a, size)) return 1;
    if (a < off || !total) return 0;
    uintptr_t base = a - off;
    if ((uint64_t)base + total > 0x100000000ull) return 0;
    if (!guest_range_register((uint32_t)base, total)) {
        LK_INC(low_register_fail);
        return 0;
    }
    return hle_android_guest_ok((uint32_t)a, size);
}

/* ------------------------------------------------------------------ */
/* shadow records                                                     */
/* ------------------------------------------------------------------ */

#define THIN_SHADOW_MAX 4096
#define THIN_LOCK_STACK 4

struct ThinFrame {
    uint32_t off;      /* byte offset of the guest pointer inside staging */
    uint32_t bytes;    /* locked byte count */
    uint32_t flags;
    int      used;
    int      direct;   /* pBits is native memory; no copy-in/copy-back */
};

struct ThinShadow {
    void    *host;
    int      kind;     /* THIN_LK_* */
    uint32_t sub;      /* level, or (face << 16) | level */
    uint8_t *staging;
    uint32_t bytes;
    uint32_t pitch;       /* row pitch of the whole surface */
    uint32_t slice_pitch; /* volume only */
    uint32_t height;
    uint32_t depth;
    uint32_t bpp;
    uint32_t lock_off;    /* offset of the outermost native lock */
    uint32_t lock_bytes;  /* outermost locked range */
    uint32_t rx, ry, rz;  /* outermost rect/box origin */
    uint32_t rw, rh, rd;  /* outermost rect/box extent */
    void    *native;      /* outermost native pointer */
    int      nframes;
    ThinFrame frame[THIN_LOCK_STACK];
    /* D3D9 resource descriptors do not change while the object exists, so
     * the descriptor is fetched once per shadow instead of once per lock.
     * The shadow is rebuilt when the host object is released. */
    uint32_t desc_size;
    uint32_t desc_usage;
    uint32_t desc_pool;
    int      desc_valid;
    uint32_t dim_w, dim_h, dim_d;
    D3DFORMAT dim_fmt;
    int      dims_valid;
    int      used;
};

static ThinShadow g_shadow[THIN_SHADOW_MAX];
static uint32_t g_shadow_hint;

static ThinShadow *shadow_find(void *host, int kind, uint32_t sub, int create)
{
    for (int i = 0; i < THIN_SHADOW_MAX; i++) {
        ThinShadow *s = &g_shadow[i];
        if (!s->used) continue;
        if (s->host == host && s->kind == kind && s->sub == sub) return s;
    }
    if (!create) return NULL;
    for (int n = 0; n < THIN_SHADOW_MAX; n++) {
        uint32_t i = (g_shadow_hint + (uint32_t)n) % THIN_SHADOW_MAX;
        if (!g_shadow[i].used) {
            g_shadow_hint = (i + 1u) % THIN_SHADOW_MAX;
            memset(&g_shadow[i], 0, sizeof g_shadow[i]);
            g_shadow[i].used = 1;
            g_shadow[i].host = host;
            g_shadow[i].kind = kind;
            g_shadow[i].sub = sub;
            return &g_shadow[i];
        }
    }
    THIN_LOG("shadow table full");
    return NULL;
}

static uint32_t format_bpp(D3DFORMAT fmt)
{
    switch ((uint32_t)fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
        return 4;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_R16F:
    case D3DFMT_D16:
    case D3DFMT_D15S1:
    case D3DFMT_L16:
        return 2;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
    case D3DFMT_A4L4:
        return 1;
    case D3DFMT_D32:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_R32F:
        return 4;
    default:
        return 0;   /* compressed or unknown: no rect offset arithmetic */
    }
}

static int surface_dims_query(ThinShadow *s, uint32_t *w, uint32_t *h,
                               uint32_t *d, D3DFORMAT *fmt)
{
    *w = *h = 1; *d = 1; *fmt = D3DFMT_UNKNOWN;
    switch (s->kind) {
    case THIN_LK_TEXTURE: {
        D3DSURFACE_DESC desc;
        memset(&desc, 0, sizeof desc);
        HRESULT hr = ((IDirect3DTexture9 *)s->host)->GetLevelDesc(
            s->sub & 0xFFFF, &desc);
        if (FAILED(hr)) return 0;
        *w = desc.Width; *h = desc.Height; *fmt = desc.Format;
        return 1;
    }
    case THIN_LK_CUBE: {
        D3DSURFACE_DESC desc;
        memset(&desc, 0, sizeof desc);
        HRESULT hr = ((IDirect3DCubeTexture9 *)s->host)->GetLevelDesc(
            s->sub & 0xFFFF, &desc);
        if (FAILED(hr)) return 0;
        *w = desc.Width; *h = desc.Height; *fmt = desc.Format;
        return 1;
    }
    case THIN_LK_SURFACE: {
        D3DSURFACE_DESC desc;
        memset(&desc, 0, sizeof desc);
        HRESULT hr = ((IDirect3DSurface9 *)s->host)->GetDesc(&desc);
        if (FAILED(hr)) return 0;
        *w = desc.Width; *h = desc.Height; *fmt = desc.Format;
        return 1;
    }
    case THIN_LK_VOLTEX: {
        D3DVOLUME_DESC desc;
        memset(&desc, 0, sizeof desc);
        HRESULT hr = ((IDirect3DVolumeTexture9 *)s->host)->GetLevelDesc(
            s->sub & 0xFFFF, &desc);
        if (FAILED(hr)) return 0;
        *w = desc.Width; *h = desc.Height; *d = desc.Depth; *fmt = desc.Format;
        return 1;
    }
    case THIN_LK_VOLUME: {
        D3DVOLUME_DESC desc;
        memset(&desc, 0, sizeof desc);
        HRESULT hr = ((IDirect3DVolume9 *)s->host)->GetDesc(&desc);
        if (FAILED(hr)) return 0;
        *w = desc.Width; *h = desc.Height; *d = desc.Depth; *fmt = desc.Format;
        return 1;
    }
    default:
        return 0;
    }
}

/* Descriptors are immutable per (object, subresource); the old layer asked
 * DXVK for them on every lock, which costs a COM call and a lock in DXVK. */
static int surface_dims(ThinShadow *s, uint32_t *w, uint32_t *h, uint32_t *d,
                        D3DFORMAT *fmt)
{
    if (s->dims_valid) {
        LK_INC(dims_hits);
        *w = s->dim_w;
        *h = s->dim_h;
        *d = s->dim_d;
        *fmt = s->dim_fmt;
        return 1;
    }
    LK_INC(dims_calls);
    if (!surface_dims_query(s, w, h, d, fmt)) return 0;
    s->dim_w = *w;
    s->dim_h = *h;
    s->dim_d = *d;
    s->dim_fmt = *fmt;
    s->dims_valid = 1;
    return 1;
}

static int rect_span(const RECT *r, uint32_t w, uint32_t h,
                     uint32_t *x, uint32_t *y, uint32_t *rw, uint32_t *rh)
{
    *x = *y = 0; *rw = w; *rh = h;
    if (!r) return 1;
    if (r->left < 0 || r->top < 0 || r->right <= r->left || r->bottom <= r->top)
        return 0;
    if ((uint32_t)r->right > w || (uint32_t)r->bottom > h) return 0;
    *x = (uint32_t)r->left;
    *y = (uint32_t)r->top;
    *rw = (uint32_t)(r->right - r->left);
    *rh = (uint32_t)(r->bottom - r->top);
    return 1;
}

static int box_span(const D3DBOX *b, uint32_t w, uint32_t h, uint32_t d,
                    uint32_t *x, uint32_t *y, uint32_t *z,
                    uint32_t *bw, uint32_t *bh, uint32_t *bd)
{
    *x = *y = *z = 0; *bw = w; *bh = h; *bd = d;
    if (!b) return 1;
    if (b->Left < 0 || b->Top < 0 || b->Front < 0 ||
        b->Right <= b->Left || b->Bottom <= b->Top || b->Back <= b->Front)
        return 0;
    if ((uint32_t)b->Right > w || (uint32_t)b->Bottom > h ||
        (uint32_t)b->Back > d)
        return 0;
    *x = b->Left; *y = b->Top; *z = b->Front;
    *bw = b->Right - b->Left;
    *bh = b->Bottom - b->Top;
    *bd = b->Back - b->Front;
    return 1;
}

/* ------------------------------------------------------------------ */
/* lock / unlock                                                      */
/* ------------------------------------------------------------------ */

uint32_t thin_lock_rect(CPU *c, ThinObj *o, int kind, uint32_t sub,
                        uint32_t flags, uint32_t guest_rect,
                        uint32_t guest_out, int is_box)
{
    if (!o->host) return D3DERR_INVALIDCALL;
    if (guest_out && !hle_android_guest_ok(guest_out,
                                           is_box ? sizeof(LockedBox32)
                                                  : sizeof(LockedRect32)))
        return D3DERR_INVALIDCALL;
    if (guest_rect && !hle_android_guest_ok(guest_rect,
                                            is_box ? sizeof(D3DBOX)
                                                   : sizeof(RECT)))
        return D3DERR_INVALIDCALL;

    ThinShadow *s = shadow_find(o->host, kind, sub, 1);
    if (!s) return D3DERR_INVALIDCALL;

    LK_INC(rect_locks);

    uint32_t w = 0, h = 0, d = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    if (!surface_dims(s, &w, &h, &d, &fmt)) return D3DERR_INVALIDCALL;
    uint32_t bpp = format_bpp(fmt);
    s->bpp = bpp;
    s->height = h;
    s->depth = d;

    uint32_t rx = 0, ry = 0, rz = 0, rw = w, rh = h, rd = d;
    if (is_box) {
        if (!box_span((const D3DBOX *)(uintptr_t)guest_rect, w, h, d,
                      &rx, &ry, &rz, &rw, &rh, &rd))
            return D3DERR_INVALIDCALL;
    } else {
        if (!rect_span((const RECT *)(uintptr_t)guest_rect, w, h,
                       &rx, &ry, &rw, &rh))
            return D3DERR_INVALIDCALL;
    }

    /* Nested lock: return the same shadow, no second DXVK lock.  Real D3D9
     * permits this; DXVK's LockRect does not. */
    if (s->nframes > 0) {
        if (s->nframes >= THIN_LOCK_STACK) return D3DERR_INVALIDCALL;
        ThinFrame *f = &s->frame[s->nframes++];
        f->used = 1;
        f->flags = flags;
        f->off = is_box
            ? rz * s->slice_pitch + ry * s->pitch + rx * bpp
            : ry * s->pitch + rx * bpp;
        f->bytes = is_box ? rd * s->slice_pitch : rh * s->pitch;
        uint32_t p = (uint32_t)(uintptr_t)(s->staging + f->off);
        if (is_box) {
            LockedBox32 lb;
            lb.RowPitch = s->pitch;
            lb.SlicePitch = s->slice_pitch;
            lb.pBits = p;
            if (guest_out) *(LockedBox32 *)(uintptr_t)guest_out = lb;
        } else {
            LockedRect32 lr;
            lr.Pitch = s->pitch;
            lr.pBits = p;
            if (guest_out) *(LockedRect32 *)(uintptr_t)guest_out = lr;
        }
        return D3D_OK;
    }

    DWORD dflags = 0;
    if (flags & D3DLOCK_DISCARD) dflags |= D3DLOCK_DISCARD;
    if (flags & D3DLOCK_NOOVERWRITE) dflags |= D3DLOCK_NOOVERWRITE;
    if (flags & D3DLOCK_NOSYSLOCK) dflags |= D3DLOCK_NOSYSLOCK;
    if (flags & D3DLOCK_DONOTWAIT) dflags |= D3DLOCK_DONOTWAIT;
    if (flags & D3DLOCK_READONLY) dflags |= D3DLOCK_READONLY;

    void *native = NULL;
    uint32_t pitch = 0, slice = 0;
    HRESULT hr = E_FAIL;

    if (is_box) {
        D3DLOCKED_BOX lb;
        memset(&lb, 0, sizeof lb);
        const D3DBOX *box = (const D3DBOX *)(uintptr_t)guest_rect;
        if (kind == THIN_LK_VOLTEX)
            hr = ((IDirect3DVolumeTexture9 *)o->host)->LockBox(
                sub & 0xFFFF, &lb, box, dflags);
        else
            hr = ((IDirect3DVolume9 *)o->host)->LockBox(&lb, box, dflags);
        if (FAILED(hr) || !lb.pBits) {
            THIN_LOG("LockBox hr=%08lX kind=%d",
                     (unsigned long)hr, kind);
            return (uint32_t)hr;
        }
        native = lb.pBits;
        pitch = (uint32_t)lb.RowPitch;
        slice = (uint32_t)lb.SlicePitch;
    } else {
        D3DLOCKED_RECT lr;
        memset(&lr, 0, sizeof lr);
        const RECT *rect = (const RECT *)(uintptr_t)guest_rect;
        if (kind == THIN_LK_TEXTURE)
            hr = ((IDirect3DTexture9 *)o->host)->LockRect(
                sub & 0xFFFF, &lr, rect, dflags);
        else if (kind == THIN_LK_CUBE)
            hr = ((IDirect3DCubeTexture9 *)o->host)->LockRect(
                (D3DCUBEMAP_FACES)((sub >> 16) & 0xFFFF), sub & 0xFFFF,
                &lr, rect, dflags);
        else
            hr = ((IDirect3DSurface9 *)o->host)->LockRect(&lr, rect, dflags);
        if (FAILED(hr) || !lr.pBits) {
            THIN_LOG("LockRect hr=%08lX kind=%d flags=%08X",
                     (unsigned long)hr, kind, flags);
            return (uint32_t)hr;
        }
        native = lr.pBits;
        pitch = (uint32_t)lr.Pitch;
    }

    s->pitch = pitch;
    s->slice_pitch = slice ? slice : pitch * h;
    uint32_t need = is_box ? s->slice_pitch * d : pitch * h;
    if (need < pitch) need = pitch;
    if (!s->staging || s->bytes < need) {
        if (s->staging) low_free(s->staging, s->bytes);
        s->staging = (uint8_t *)low_alloc(need);
        s->bytes = s->staging ? need : 0;
    }
    if (!s->staging) return D3DERR_INVALIDCALL;

    s->native = native;
    s->rx = rx; s->ry = ry; s->rz = rz;
    s->rw = rw; s->rh = rh; s->rd = rd;
    s->lock_off = is_box ? rz * s->slice_pitch + ry * pitch + rx * bpp
                         : ry * pitch + rx * bpp;
    s->lock_bytes = is_box ? rd * s->slice_pitch : rh * pitch;

    /* Copy-in.  DISCARD means the previous contents are undefined. */
    if (!(flags & D3DLOCK_DISCARD)) {
        if (bpp) {
            for (uint32_t z = 0; z < rd; z++) {
                for (uint32_t y = 0; y < rh; y++) {
                    memcpy(s->staging + (rz + z) * s->slice_pitch +
                               (ry + y) * pitch + rx * bpp,
                           (uint8_t *)native + z * s->slice_pitch + y * pitch,
                           rw * bpp);
                }
            }
            LK_ADD(copy_in_bytes, (unsigned long long)rw * bpp * rh * rd);
        } else {
            memcpy(s->staging + s->lock_off, native, s->lock_bytes);
            LK_ADD(copy_in_bytes, s->lock_bytes);
        }
    }

    ThinFrame *f = &s->frame[0];
    f->used = 1;
    f->flags = flags;
    f->off = s->lock_off;
    f->bytes = s->lock_bytes;
    f->direct = 0;
    s->nframes = 1;

    uint32_t p = (uint32_t)(uintptr_t)(s->staging + s->lock_off);
    if (is_box) {
        LockedBox32 lb;
        lb.RowPitch = pitch;
        lb.SlicePitch = s->slice_pitch;
        lb.pBits = p;
        if (guest_out) *(LockedBox32 *)(uintptr_t)guest_out = lb;
    } else {
        LockedRect32 lr;
        lr.Pitch = pitch;
        lr.pBits = p;
        if (guest_out) *(LockedRect32 *)(uintptr_t)guest_out = lr;
    }

    thin_count(THIN_STAT_LOCK);
    THIN_LOG("lock kind=%d sub=%u flags=%08X pitch=%u bpp=%u ptr=%08X",
             kind, sub, flags, pitch, bpp, p);
    return D3D_OK;
}

static void tex_alpha_log(ThinShadow *s)
{
#if WC3_DEBUG && WC3_DEBUG_GRAPHICS
    static int on = -1;
    static unsigned seen;
    if (on < 0) on = getenv("WC3_THIN_TEX_ALPHA") != NULL;
    if (!on || seen >= 64) return;
    if (s->kind != THIN_LK_TEXTURE && s->kind != THIN_LK_CUBE) return;

    uint32_t w = 0, h = 0, d = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    if (!surface_dims(s, &w, &h, &d, &fmt)) return;
    uint32_t bpp = format_bpp(fmt);
    if (!bpp || !s->staging || !s->rw || !s->rh) return;
    if (w < 64 || h < 64) return;

    unsigned a0 = 0, a255 = 0, acima = 0, nz = 0, amin = 255, amax = 0;
    unsigned long long sum = 0;
    unsigned hist[16] = {0};
    for (uint32_t y = 0; y < s->rh; y++) {
        const uint8_t *row = s->staging + (s->ry + y) * s->pitch +
                             s->rx * bpp;
        for (uint32_t x = 0; x < s->rw; x++) {
            const uint8_t *p = row + x * bpp;
            unsigned a;
            switch ((uint32_t)fmt) {
            case D3DFMT_A8R8G8B8:
                a = p[3];
                break;
            case D3DFMT_A8B8G8R8:
                a = p[3];
                break;
            case D3DFMT_A4R4G4B4:
            case D3DFMT_X4R4G4B4:
                a = (unsigned)((p[1] >> 4) & 0xFu) * 17u;
                break;
            case D3DFMT_A1R5G5B5:
            case D3DFMT_X1R5G5B5:
                a = (p[1] & 0x80u) ? 255u : 0u;
                break;
            case D3DFMT_A8:
            case D3DFMT_A4L4:
                a = p[0];
                break;
            default:
                return;
            }
            if (a == 0) a0++;
            if (a == 255) a255++;
            if (a >= 240) acima++;
            if (a) nz++;
            if (a < amin) amin = a;
            if (a > amax) amax = a;
            hist[a >> 4]++;
            sum += a;
        }
    }
    unsigned n = s->rw * s->rh;
    uint32_t px0 = s->rw >= 4 ? *(const uint32_t *)s->staging : 0;
    THIN_LOG("texa kind=%d sub=%u w=%u h=%u fmt=%u bpp=%u rw=%u rh=%u "
             "a0=%u a255=%u acima=%u nz=%u amin=%u amax=%u mean=%u "
             "px0=%08X h=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
             s->kind, s->sub, w, h, (unsigned)fmt, bpp, s->rw, s->rh,
             a0, a255, acima, nz, amin, amax, (unsigned)(sum / (n ? n : 1)),
             px0, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5],
             hist[6], hist[7], hist[8], hist[9], hist[10], hist[11],
             hist[12], hist[13], hist[14], hist[15]);
    seen++;
#else
    (void)s;
#endif
}

uint32_t thin_unlock_rect(CPU *c, ThinObj *o, int kind, uint32_t sub)
{
    (void)c;
    ThinShadow *s = shadow_find(o->host, kind, sub, 0);
    if (!s || s->nframes == 0) return D3DERR_INVALIDCALL;
    if (s->nframes > 1) { s->nframes--; return D3D_OK; }

    LK_INC(rect_unlocks);
    ThinFrame *f = &s->frame[0];
    int readonly = (f->flags & D3DLOCK_READONLY) != 0;

    if (!readonly) tex_alpha_log(s);

    if (!readonly && s->native && s->staging) {
        if (s->bpp && s->pitch) {
            uint32_t copy_w = s->rw * s->bpp;
            if (copy_w > s->pitch) copy_w = s->pitch;
            for (uint32_t z = 0; z < s->rd; z++) {
                for (uint32_t y = 0; y < s->rh; y++) {
                    memcpy((uint8_t *)s->native + z * s->slice_pitch +
                               y * s->pitch,
                           s->staging + (s->rz + z) * s->slice_pitch +
                               (s->ry + y) * s->pitch + s->rx * s->bpp,
                           copy_w);
                }
            }
            LK_ADD(copy_out_bytes,
                   (unsigned long long)copy_w * s->rh * s->rd);
        } else {
            memcpy(s->native, s->staging + s->lock_off, s->lock_bytes);
            LK_ADD(copy_out_bytes, s->lock_bytes);
        }
    }

    HRESULT hr;
    if (kind == THIN_LK_VOLTEX)
        hr = ((IDirect3DVolumeTexture9 *)o->host)->UnlockBox(sub & 0xFFFF);
    else if (kind == THIN_LK_VOLUME)
        hr = ((IDirect3DVolume9 *)o->host)->UnlockBox();
    else if (kind == THIN_LK_TEXTURE)
        hr = ((IDirect3DTexture9 *)o->host)->UnlockRect(sub & 0xFFFF);
    else if (kind == THIN_LK_CUBE)
        hr = ((IDirect3DCubeTexture9 *)o->host)->UnlockRect(
            (D3DCUBEMAP_FACES)((sub >> 16) & 0xFFFF), sub & 0xFFFF);
    else
        hr = ((IDirect3DSurface9 *)o->host)->UnlockRect();

    s->nframes = 0;
    s->native = NULL;
    thin_count(THIN_STAT_UNLOCK);
    return (uint32_t)hr;
}

uint32_t thin_lock_buffer(CPU *c, ThinObj *o, uint32_t off, uint32_t size,
                          uint32_t flags, uint32_t guest_out)
{
    (void)c;
    if (!o->host) return D3DERR_INVALIDCALL;
    if (guest_out && !hle_android_guest_ok(guest_out, 4))
        return D3DERR_INVALIDCALL;

    ThinShadow *s = shadow_find(o->host, THIN_LK_BUFFER, 0, 1);
    if (!s) return D3DERR_INVALIDCALL;

    /* The descriptor of a buffer never changes, so fetch it once per
     * shadow.  The old path paid a COM call and a DXVK lock on every Lock. */
    if (!s->desc_valid) {
        uint32_t total = 0, usage = 0, pool = 0;
        if (o->iface == D3D9_IFACE_IDirect3DVertexBuffer9) {
            D3DVERTEXBUFFER_DESC desc;
            memset(&desc, 0, sizeof desc);
            if (FAILED(((IDirect3DVertexBuffer9 *)o->host)->GetDesc(&desc)))
                return D3DERR_INVALIDCALL;
            total = desc.Size;
            usage = desc.Usage;
            pool = desc.Pool;
        } else {
            D3DINDEXBUFFER_DESC desc;
            memset(&desc, 0, sizeof desc);
            if (FAILED(((IDirect3DIndexBuffer9 *)o->host)->GetDesc(&desc)))
                return D3DERR_INVALIDCALL;
            total = desc.Size;
            usage = desc.Usage;
            pool = desc.Pool;
        }
        s->desc_size = total;
        s->desc_usage = usage;
        s->desc_pool = pool;
        s->desc_valid = 1;
        LK_INC(getdesc_calls);
    } else {
        LK_INC(getdesc_hits);
    }
    uint32_t total = s->desc_size;
    if (off >= total) return D3DERR_INVALIDCALL;
    uint32_t n = size ? size : total - off;
    if (n > total - off) return D3DERR_INVALIDCALL;

    LK_INC(buffer_locks);
#if WC3_DEBUG && WC3_DEBUG_PERF
    if ((g_lk.buffer_locks & 8191ull) == 0)
        thin_lock_stats_dump("periodic");
#endif

    if (s->nframes > 0) {
        if (s->nframes >= THIN_LOCK_STACK) return D3DERR_INVALIDCALL;
        ThinFrame *f = &s->frame[s->nframes++];
        f->used = 1;
        f->flags = flags;
        f->off = off;
        f->bytes = n;
        f->direct = s->frame[0].direct;
        if (guest_out) {
            uint32_t p = f->direct
                ? (uint32_t)(uintptr_t)((uint8_t *)s->native +
                                        (ptrdiff_t)off -
                                        (ptrdiff_t)s->lock_off)
                : (uint32_t)(uintptr_t)(s->staging + off);
            wr32g(guest_out, p);
        }
        return D3D_OK;
    }

    void *ptr = NULL;
    DWORD dflags = 0;
    if (flags & D3DLOCK_DISCARD) dflags |= D3DLOCK_DISCARD;
    if (flags & D3DLOCK_NOOVERWRITE) dflags |= D3DLOCK_NOOVERWRITE;
    if (flags & D3DLOCK_NOSYSLOCK) dflags |= D3DLOCK_NOSYSLOCK;
    if (flags & D3DLOCK_DONOTWAIT) dflags |= D3DLOCK_DONOTWAIT;
    if (flags & D3DLOCK_READONLY) dflags |= D3DLOCK_READONLY;

    HRESULT hr;
    if (o->iface == D3D9_IFACE_IDirect3DVertexBuffer9)
        hr = ((IDirect3DVertexBuffer9 *)o->host)->Lock(off, n, &ptr, dflags);
    else
        hr = ((IDirect3DIndexBuffer9 *)o->host)->Lock(off, n, &ptr, dflags);
    if (FAILED(hr) || !ptr) {
        THIN_LOG("Buffer::Lock(%u,%u,%08X) hr=%08lX",
                 off, n, flags, (unsigned long)hr);
        return (uint32_t)hr;
    }

    /* Direct path: DXVK's pointer is already guest addressable.  The guest
     * writes straight into the buffer DXVK will read; Unlock has no copy. */
    int direct = low_buffer_usable(ptr, off, n, total);
    if (direct) LK_INC(direct_locks);

    if (!direct) {
        if (!s->staging || s->bytes < total) {
            if (s->staging) low_free(s->staging, s->bytes);
            s->staging = (uint8_t *)low_alloc(total);
            s->bytes = s->staging ? total : 0;
            LK_INC(staging_grows);
        }
        if (!s->staging) {
            /* Keep DXVK's lock accounting balanced: the native lock already
             * succeeded, so it must be released before we fail the call. */
            if (o->iface == D3D9_IFACE_IDirect3DVertexBuffer9)
                ((IDirect3DVertexBuffer9 *)o->host)->Unlock();
            else
                ((IDirect3DIndexBuffer9 *)o->host)->Unlock();
            return D3DERR_INVALIDCALL;
        }

        if (!(flags & D3DLOCK_DISCARD)) {
            int wo = (s->desc_usage & D3DUSAGE_WRITEONLY) != 0;
            if (wo) LK_INC(wo_locks);
            int skip = wo && thin_wo_copyin_skip();
            if (skip) LK_INC(wo_copyin_skipped);
            if (!skip) {
                memcpy(s->staging + off, ptr, n);
                LK_ADD(copy_in_bytes, n);
            }
        }
    }
    if (flags & D3DLOCK_DISCARD) LK_INC(discard_locks);

    s->native = ptr;
    s->lock_off = off;
    s->lock_bytes = n;
    ThinFrame *f = &s->frame[0];
    f->used = 1;
    f->flags = flags;
    f->off = off;
    f->bytes = n;
    f->direct = direct;
    s->nframes = 1;

    if (guest_out) {
        uint32_t p = direct
            ? (uint32_t)(uintptr_t)ptr
            : (uint32_t)(uintptr_t)(s->staging + off);
        wr32g(guest_out, p);
    }
    thin_count(THIN_STAT_LOCK);
    return D3D_OK;
}

void thin_lock_forget(void *host)
{
    if (!host) return;
    for (int i = 0; i < THIN_SHADOW_MAX; i++) {
        ThinShadow *s = &g_shadow[i];
        if (!s->used || s->host != host) continue;
        if (s->staging) low_free(s->staging, s->bytes);
        memset(s, 0, sizeof *s);
    }
}

void thin_lock_reset(void)
{
    thin_lock_stats_dump("reset");
    for (int i = 0; i < THIN_SHADOW_MAX; i++) {
        ThinShadow *s = &g_shadow[i];
        if (!s->used) continue;
        if (s->staging) low_free(s->staging, s->bytes);
        memset(s, 0, sizeof *s);
    }
    g_shadow_hint = 0;
}

uint32_t thin_unlock_buffer(CPU *c, ThinObj *o)
{
    (void)c;
    ThinShadow *s = shadow_find(o->host, THIN_LK_BUFFER, 0, 0);
    if (!s || s->nframes == 0) return D3DERR_INVALIDCALL;
    if (s->nframes > 1) { s->nframes--; return D3D_OK; }

    LK_INC(buffer_unlocks);
    ThinFrame *f = &s->frame[0];
    /* A direct lock already wrote into DXVK's memory; only a shadow lock has
     * something to copy back. */
    if (!f->direct && !(f->flags & D3DLOCK_READONLY) &&
        s->native && s->staging) {
        memcpy(s->native, s->staging + f->off, f->bytes);
        LK_ADD(copy_out_bytes, f->bytes);
    }

    HRESULT hr;
    if (o->iface == D3D9_IFACE_IDirect3DVertexBuffer9)
        hr = ((IDirect3DVertexBuffer9 *)o->host)->Unlock();
    else
        hr = ((IDirect3DIndexBuffer9 *)o->host)->Unlock();

    s->nframes = 0;
    s->native = NULL;
    thin_count(THIN_STAT_UNLOCK);
    return (uint32_t)hr;
}
