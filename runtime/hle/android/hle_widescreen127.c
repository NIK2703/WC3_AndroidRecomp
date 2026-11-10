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
#include <stdio.h>
#include <stdint.h>

#include "host.h"

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

    float *m = (float *)(uintptr_t)out;
    m[0] /= factor;
}

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
    if (factor == 1.0f)
        return;

    float *value = (float *)(uintptr_t)(frame + 0x58);
    *value /= factor;
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
