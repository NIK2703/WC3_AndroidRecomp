/*
 * hle_widescreen127.c - the two RenderEdge_Widescreen hooks, ported to the
 * lifted 1.27 corpus.
 *
 * The original mod is a 32-bit DLL injected into war3.exe.  It detours
 * Game.dll's CreateMatrixPerspectiveFov and BuildHPBars.  The recompiler never
 * executes the original x86 image, so the same two observations are applied
 * around the lifted functions instead:
 *
 *   CreateMatrixPerspectiveFov:
 *       wideMul = (client width / height) / (4 / 3)
 *       projection.m00 /= wideMul
 *
 *   BuildHPBars:
 *       after the complete original call, divide the HP-bar frame's
 *       float field at +0x58 by wideMul.
 *
 * BuildHPBars is wrapped at its only direct caller, L_6F654E20, rather than
 * at L_6F3925F0 itself.  The lifted BuildHPBars extent ends in dispatch_jmp
 * tails; a wrapper around the extent would run before those tails, while the
 * original Detours proxy runs after the whole function returns.  The caller
 * invokes BuildHPBars through dispatch(), which drains the tail chain before
 * returning, so the correction belongs there.
 *
 * The link uses --wrap so neither the generated dispatcher nor the generated
 * corpus is edited.
 */
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "host.h"

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
#include "parallel_phase.h"
#endif

#ifdef __ANDROID__
/* This file lives in runtime/host; the Android build's common flags do not
 * add runtime/android, and hle_android.c reaches its own header by directory
 * lookup only.  Keep the include relative so the shared build script needs no
 * extra include path. */
#include "hle_android.h"
#else
#include <windows.h>
#endif

/* --wrap turns references to L_x into __wrap_L_x and supplies this alias for
 * the real body.  Keep the declarations visible so the compiler type-checks
 * the CPU ABI. */
void __real_L_6F126E30(CPU *c);
void __real_L_6F654E20(CPU *c);

/* The original reads the whole window rectangle.  The recompiler has no
 * window chrome in the guest, and the guest client/back-buffer is the surface
 * whose aspect the projection is actually rendered into, so use that instead.
 * The Android display mode is the selected guest back-buffer, not the native
 * surface: DXVK scales the guest image to the surface afterwards. */
static float widescreen_factor(int *out_w, int *out_h)
{
    int w = 0, h = 0;

#ifdef __ANDROID__
    uint32_t aw = 0, ah = 0;
    if (!hle_android_current_display_mode(&aw, &ah))
        return 1.0f;
    w = (int)aw;
    h = (int)ah;
#else
    vu_host_client_size(&w, &h);
#endif

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (w <= 1 || h <= 1)
        return 1.0f;

    float factor = (float)w * (1.0f / (float)h) * 0.75f;
    if (!(factor > 0.0f))
        return 1.0f;
    return factor;
}

static float widescreen_factor_logged(void)
{
    int w = 0, h = 0;
    float factor = widescreen_factor(&w, &h);
#if WC3_DEBUG && WC3_DEBUG_RESOLUTION
    static int last_w = -1, last_h = -1;
    if (w != last_w || h != last_h) {
        fprintf(stderr, "[resolution] widescreen client %dx%d factor %.6f\n",
                w, h, factor);
        last_w = w;
        last_h = h;
    }
#endif
    return factor;
}

/* L_6F126E30: __fastcall, ECX = out 4x4 matrix; stack args are
 * (fovY, aspectRatio, nearZ, farZ).  The original computes the complete
 * matrix.  RenderEdge changes exactly one element, m00, which is the first
 * float in this column-major matrix. */
void __wrap_L_6F126E30(CPU *c)
{
    uint32_t out = c->ecx;

    __real_L_6F126E30(c);

    if (!out)
        return;
    float factor = widescreen_factor_logged();
    if (factor == 1.0f)
        return;

    float *m = (float *)guest_host_ptr(out);
    m[0] /= factor;
}

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
/* HP-bar trace for the deferred-postfix investigation.  The first 512 builds
 * are logged; WC3_HPBAR_TRACE=1 removes the budget and WC3_HPBAR_TRACE=0
 * disables it.  The mode field says whether the build ran in the serial path,
 * inside the collect prefix, or inside the phase apply. */
static int hpb_trace_left = -1;

static int hpb_trace_take(void)
{
    if (hpb_trace_left < 0) {
        const char *s = getenv("WC3_HPBAR_TRACE");
        if (s && s[0] == '0' && s[1] == '\0')
            hpb_trace_left = 0;
        else if (s && s[0] == '1' && s[1] == '\0')
            hpb_trace_left = INT_MAX;
        else
            hpb_trace_left = 512;
    }
    if (hpb_trace_left <= 0) return 0;
    hpb_trace_left--;
    return 1;
}

static uint32_t hpb_rd32(uint32_t va)
{
    return hle_android_guest_ok(va, 4u) ? rd32(va) : 0xFFFFFFFFu;
}

static void hpb_dump_words(const char *tag, uint32_t va, uint32_t off,
                           uint32_t n)
{
    fprintf(stderr, "%s va=%08x", tag, va);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = va + off + i * 4u;
        fprintf(stderr, " %03x:%08x", off + i * 4u, hpb_rd32(a));
    }
    fprintf(stderr, "\n");
}
#endif

/* L_6F654E20 is the only direct caller of BuildHPBars.  It receives
 *   ECX       = owner object
 *   [esp+4]   = argument 2 (0 selects the alternate path that does not call
 *               BuildHPBars)
 *   [esp+8]   = argument 3
 * BuildHPBars is called with ECX = [owner+0x50] and arguments (owner, arg3).
 * Its proxy then reads the frame pointer from [ECX+0x0C] and scales the float
 * at frame+0x58.  Read the owner fields after the real call so a lazily
 * created [owner+0x50] is visible. */
void __wrap_L_6F654E20(CPU *c)
{
    uint32_t owner = c->ecx;
    uint32_t arg2 = rd32(c->esp + 4);
#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    uint32_t arg3 = rd32(c->esp + 8);
#endif

    __real_L_6F654E20(c);

    if (!owner || !arg2)
        return;

    uint32_t bars = rd32(owner + 0x50);
    if (!bars)
        return;

    uint32_t frame = rd32(bars + 0x0C);
    if (!frame)
        return;

    float factor = widescreen_factor_logged();
    float *value = (float *)guest_host_ptr(frame + 0x58);

#if WC3_DEBUG && WC3_DEBUG_PARALLEL
    float before = *value;
    if (factor != 1.0f)
        *value = before / factor;

    if (hpb_trace_take()) {
        unsigned apply = wc3_phase_apply_index();
        unsigned count = wc3_phase_count();
        const char *mode = apply ? "apply"
                                 : (wc3_phase_armed() ? "collect" : "serial");
        fprintf(stderr,
                "[hpb] mode=%s req=%u count=%u full=%d owner=%08x "
                "arg2=%08x arg3=%08x bars=%08x frame=%08x "
                "f58=%.6f->%.6f factor=%.6f caller=%p\n",
                mode, apply, count, wc3_phase_window_full(), owner, arg2,
                arg3, bars, frame, before, *value, factor,
                __builtin_return_address(0));
        hpb_dump_words("[hpb-frame]", frame, 0x000u, 0x20u);
        hpb_dump_words("[hpb-frame]", frame, 0x080u, 0x20u);
        hpb_dump_words("[hpb-frame]", frame, 0x100u, 0x16u);
        hpb_dump_words("[hpb-bars]", bars, 0x000u, 0x04u);
        hpb_dump_words("[hpb-owner]", owner, 0x000u, 0x08u);
        fprintf(stderr,
                "[hpb-owner] owner=%08x +50=%08x +280=%08x +284=%08x "
                "+288=%08x +28c=%08x\n",
                owner, hpb_rd32(owner + 0x50), hpb_rd32(owner + 0x280),
                hpb_rd32(owner + 0x284), hpb_rd32(owner + 0x288),
                hpb_rd32(owner + 0x28C));
        hpb_dump_words("[hpb-owner2]", owner, 0x000u, 0x18u);
        {
            uint32_t sp = hpb_rd32(owner + 0x48u);
            fprintf(stderr, "[hpb-sp] owner=%08x sp=%08x\n", owner, sp);
            if (sp && sp != 0xFFFFFFFFu)
                hpb_dump_words("[hpb-spd]", sp, 0x000u, 0x04u);
        }
        fprintf(stderr,
                "[hpb-glob] ef6c=%08x ffc4=%08x 4470=%08x d687a8=%08x "
                "d68610=%08x d6aac8=%08x\n",
                hpb_rd32(0x6FA7EF6Cu), hpb_rd32(0x6FA7FFC4u),
                hpb_rd32(0x6FA84470u), hpb_rd32(0x6FD687A8u),
                hpb_rd32(0x6FD68610u), hpb_rd32(0x6FD6AAC8u));
    }
#else
    if (factor == 1.0f)
        return;
    *value /= factor;
#endif
}

/* The 1.27 UI caches the first client rectangle it reads at 0x6FD3D3F8 and
 * refreshes it only when the right or bottom field is zero.  On Windows
 * WM_DISPLAYCHANGE supplies the new size; the Android window never changes
 * size, so the cached value would stay at the native surface rectangle after
 * the user selects a smaller guest mode.  Every mouse event converts its
 * client coordinates through that rectangle, which makes taps land at the
 * wrong place.  Clear the two fields the cache check reads; the next
 * GetClientRect fills in the selected mode from hle_android.c. */
void hle_widescreen127_client_size_changed(void)
{
    /* hle_android.c calls this only after game.dll has been mapped; the cache
     * lives in that image. */
    wr32(0x6FD3D400u, 0);
    wr32(0x6FD3D404u, 0);
#if WC3_DEBUG && WC3_DEBUG_RESOLUTION
    fprintf(stderr, "[resolution] invalidate guest client rect cache\n");
#endif
}
