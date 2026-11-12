/*
 * mss_timer_hle.c - the USER32 timer/pump seam used by Mss32.
 *
 * The DirectSound audio architecture has no WAVEHDR pool and no WOM_DONE.
 * Mss32 still needs its own mixer timer: it creates it with
 * USER32!SetTimer(50 ms) and lets the owning guest thread pump WM_TIMER
 * through GetMessage,
 * PeekMessage and blocking waits.  This file keeps exactly that mechanism.
 *
 * Everything here is HLE bookkeeping only.  No PCM is produced, no AAudio
 * stream is touched, and no host lock is held while a guest callback runs.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "direct_call.h"
#include "hle_bridge.h"
#include "platform.h"
#include "wc3_debug.h"
#include "mss_timer_hle.h"

#define MSS_WM_TIMER          0x0113u
#define MSS_WAIT_TIMEOUT      258u
#define MSS_INFINITE          0xFFFFFFFFu
#define MSS_WAIT_SLICE_MS     10u
#define MSS_TIMER_MAX         32u
#define MSS_DISPATCH_RETURN   0x7EFFFFF1u

typedef struct {
    int used;
    uint32_t hwnd, id, elapse, callback;
    uint32_t owner_gid, due_ms;
    wc3_lifted_fn callback_fn;
} MssTimer;

static MssTimer g_timers[MSS_TIMER_MAX];
static uint32_t g_timer_next_id = 0xA000u;
static const Wc3Platform *g_platform;
static const Wc3TimeApi *g_time;
static _Thread_local int g_in_tick;

/* Short bookkeeping section only: no guest code and no blocking operation
 * runs while it is held.  This is a per-object host lock, not a spinlock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void mss_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void mss_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static uint32_t mss_gid(const CPU *c)
{
    if (c && c->fs_base && wc3_legacy_guest_ok(c->fs_base + 0x24u, 4u))
        return wc3_legacy_rd32g(c->fs_base + 0x24u);
    return 0u;
}

static uint32_t mss_now_ms(void)
{
    if (g_time && g_time->monotonic_ns)
        return (uint32_t)(g_time->monotonic_ns(g_time->ctx) / 1000000ULL);
    return 0u;
}

#if WC3_DEBUG && WC3_DEBUG_AUDIO
/* Mss32.dll is corpus_fixed at 0x35100000; only the mixer-callback probe
 * needs these bounds. */
#define MSS_IMAGE_BASE        0x35100000u
#define MSS_IMAGE_LIMIT       0x35400000u
#define MSS_LOG(...) do { \
    fprintf(stderr, "[mss] " __VA_ARGS__); \
    fputc('\n', stderr); \
} while (0)

static int mss_is_mixer_cb(uint32_t va)
{
    return va >= MSS_IMAGE_BASE && va < MSS_IMAGE_LIMIT;
}
#else
#define MSS_LOG(...) do { } while (0)
#endif

/* stdcall arguments are pushed right-to-left.  The lifted `ret` consumes the
 * arguments; the extra return slot keeps the temporary frame valid.  The
 * caller's CPU state is restored so the original pump continues unchanged. */
static void mss_invoke(CPU *c, uint32_t target, wc3_lifted_fn resolved,
                       const uint32_t *args, unsigned n)
{
    if (!c || !target || !args || !n) return;
    uint32_t esp = c->esp;
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    int ok = 1;
    for (int k = (int)n - 1; k >= 0; k--) {
        if (c->esp < 4u || !wc3_legacy_guest_ok(c->esp - 4u, 4u)) {
            ok = 0;
            break;
        }
        c->esp -= 4u;
        wc3_legacy_wr32g(c->esp, args[k]);
    }
    if (ok && c->esp >= 4u && wc3_legacy_guest_ok(c->esp - 4u, 4u)) {
        c->esp -= 4u;
        wc3_legacy_wr32g(c->esp, MSS_DISPATCH_RETURN);
        if (resolved)
            wc3_dispatch_resolved(c, resolved);
        else
            dispatch(c, target);
    }
    c->esp = esp;
    c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp;
}

static void mss_fire_timer(CPU *c, const MssTimer *t, uint32_t tick_ms)
{
    if (!t->callback) return;
#if WC3_DEBUG && WC3_DEBUG_AUDIO
    if (mss_is_mixer_cb(t->callback))
        MSS_LOG("timer t=%u id=%u cb=%08X", tick_ms, t->id, t->callback);
#endif
    const uint32_t args[4] = { t->hwnd, MSS_WM_TIMER, t->id, tick_ms };
    mss_invoke(c, t->callback, t->callback_fn, args, 4u);
}

/* At most one tick per timer per pump.  due_ms advances from its previous
 * value, so a late pump does not re-base the 50 ms phase and does not emit a
 * catch-up burst. */
static void mss_wall_tick(CPU *c)
{
    if (!c || g_in_tick) return;
    uint32_t now = mss_now_ms();
    uint32_t gid = mss_gid(c);

    mss_lock();
    MssTimer due[MSS_TIMER_MAX];
    unsigned ndue = 0;
    for (unsigned i = 0; i < MSS_TIMER_MAX; i++) {
        MssTimer *t = &g_timers[i];
        if (!t->used || !t->callback || t->owner_gid != gid) continue;
        if ((int32_t)(now - t->due_ms) < 0) continue;
        due[ndue++] = *t;
        uint32_t elapse = t->elapse ? t->elapse : 50u;
        uint32_t late = now - t->due_ms;
        uint32_t periods = late / elapse + 1u;
        t->due_ms += periods * elapse;
    }
    mss_unlock();

    if (!ndue) return;
    g_in_tick = 1;
    for (unsigned i = 0; i < ndue; i++)
        mss_fire_timer(c, &due[i], now);
    g_in_tick = 0;
}

/* Win32 promotes WM_TIMER only on the thread that owns the timer queue. */
static int mss_timer_owned_by(const CPU *c)
{
    if (!c) return 0;
    uint32_t gid = mss_gid(c);
    int found = 0;
    mss_lock();
    for (unsigned i = 0; i < MSS_TIMER_MAX; i++) {
        const MssTimer *t = &g_timers[i];
        if (t->used && t->callback && t->owner_gid == gid) {
            found = 1;
            break;
        }
    }
    mss_unlock();
    return found;
}

/* ---- USER32 message pump --------------------------------------------- */

typedef void (*MssCallFn)(CPU *c);
static MssCallFn g_prev_get_message;
static MssCallFn g_prev_peek_message;
static MssCallFn g_prev_wait_one;
static MssCallFn g_prev_wait_many;

static void h_mss_pump_get(CPU *c)
{
    mss_wall_tick(c);
    if (g_prev_get_message) g_prev_get_message(c);
}

static void h_mss_pump_peek(CPU *c)
{
    mss_wall_tick(c);
    if (g_prev_peek_message) g_prev_peek_message(c);
}

static void mss_set_arg32(CPU *c, unsigned index, uint32_t value)
{
    if (!c) return;
    uint32_t va = c->esp + 4u + index * 4u;
    if (wc3_legacy_guest_ok(va, 4u)) wc3_legacy_wr32g(va, value);
}

/* A blocking wait on the timer-owner thread must keep the Mss32 mixer alive.
 * Re-enter the original wait in 10 ms slices, tick only between slices and
 * preserve the caller's total timeout.  A zero timeout is not a pump point. */
static void mss_wait_sliced(CPU *c, MssCallFn prev, unsigned timeout_arg)
{
    if (!prev) return;
    if (!c || !mss_timer_owned_by(c)) {
        prev(c);
        return;
    }
    uint32_t original = wc3_legacy_arg32(c, timeout_arg);
    if (original == 0u) {
        prev(c);
        return;
    }

    uint32_t start = mss_now_ms();
    for (;;) {
        uint32_t elapsed = mss_now_ms() - start;
        if (original != MSS_INFINITE && elapsed >= original) {
            c->eax = MSS_WAIT_TIMEOUT;
            return;
        }
        uint32_t slice = MSS_WAIT_SLICE_MS;
        if (original != MSS_INFINITE) {
            uint32_t left = original - elapsed;
            if (left < slice) slice = left;
        }
        mss_set_arg32(c, timeout_arg, slice);
        prev(c);
        mss_set_arg32(c, timeout_arg, original);
        if (c->eax != MSS_WAIT_TIMEOUT) return;
        mss_wall_tick(c);
    }
}

static void h_mss_wait_one(CPU *c)
{
    mss_wait_sliced(c, g_prev_wait_one, 1u);
}

static void h_mss_wait_many(CPU *c)
{
    uint32_t count = wc3_legacy_arg32(c, 0u);
    if (count == 0u || count > 64u || !g_prev_wait_many) {
        if (g_prev_wait_many) g_prev_wait_many(c);
        return;
    }
    mss_wait_sliced(c, g_prev_wait_many, 3u);
}

/* ---- USER32 timer table ---------------------------------------------- */

static void h_mss_set_timer(CPU *c)
{
    uint32_t hwnd = wc3_legacy_arg32(c, 0);
    uint32_t id = wc3_legacy_arg32(c, 1);
    uint32_t elapse = wc3_legacy_arg32(c, 2);
    uint32_t cb = wc3_legacy_arg32(c, 3);
    if (elapse == 0u) elapse = 1u;
    uint32_t gid = mss_gid(c);
    uint32_t now = mss_now_ms();

    wc3_lifted_fn callback_fn = cb ? wc3_dispatch_resolve(cb) : NULL;

    mss_lock();
    MssTimer *t = NULL;
    if (id) {
        for (unsigned i = 0; i < MSS_TIMER_MAX; i++) {
            MssTimer *q = &g_timers[i];
            if (q->used && q->owner_gid == gid &&
                q->hwnd == hwnd && q->id == id) {
                t = q;
                break;
            }
        }
    }
    if (!t) {
        for (unsigned i = 0; i < MSS_TIMER_MAX; i++) {
            if (!g_timers[i].used) {
                t = &g_timers[i];
                break;
            }
        }
    }
    if (!t) {
        mss_unlock();
        c->eax = 0u;
        return;
    }
    if (!t->used || !id) {
        if (!id) {
            id = ++g_timer_next_id;
            if (!id) id = ++g_timer_next_id;
        }
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->owner_gid = gid;
        t->hwnd = hwnd;
        t->id = id;
    }
    t->elapse = elapse;
    t->callback = cb;
    t->callback_fn = callback_fn;
    t->due_ms = now + elapse;
    mss_unlock();
    c->eax = id;
    MSS_LOG("timer set hwnd=%u id=%u elapse=%u cb=%08X gid=%u",
            hwnd, id, elapse, cb, gid);
}

static void h_mss_kill_timer(CPU *c)
{
    uint32_t hwnd = wc3_legacy_arg32(c, 0);
    uint32_t id = wc3_legacy_arg32(c, 1);
    uint32_t gid = mss_gid(c);
    int found = 0;
    mss_lock();
    for (unsigned i = 0; i < MSS_TIMER_MAX; i++) {
        MssTimer *t = &g_timers[i];
        if (!t->used || t->owner_gid != gid) continue;
        if (id && t->id != id) continue;
        if (hwnd && t->hwnd != hwnd) continue;
        t->used = 0;
        found = 1;
        if (id) break;
    }
    mss_unlock();
    c->eax = found ? 1u : 0u;
    MSS_LOG("timer kill hwnd=%u id=%u gid=%u found=%d",
            hwnd, id, gid, found);
}

static int mss_bind_or_add(const char *name, int argc, void (*impl)(CPU *c))
{
    if (wc3_legacy_hle_override("USER32.dll", name, impl) == 0)
        return 0;
    return wc3_legacy_hle_add("USER32.dll", name, argc, 0, impl);
}

int wc3_hle_mss_timer_install(void)
{
    static int installed = 0;
    if (installed) return 6;

    g_platform = wc3_platform();
    if (g_platform) g_time = &g_platform->time;

    if (wc3_legacy_hle_chain("USER32.dll", "GetMessageA", h_mss_pump_get,
                             &g_prev_get_message) != 0 ||
        wc3_legacy_hle_chain("USER32.dll", "PeekMessageA", h_mss_pump_peek,
                             &g_prev_peek_message) != 0 ||
        wc3_legacy_hle_chain("KERNEL32.dll", "WaitForSingleObject",
                             h_mss_wait_one, &g_prev_wait_one) != 0 ||
        wc3_legacy_hle_chain("KERNEL32.dll", "WaitForMultipleObjects",
                             h_mss_wait_many, &g_prev_wait_many) != 0 ||
        mss_bind_or_add("SetTimer", 4, h_mss_set_timer) != 0 ||
        mss_bind_or_add("KillTimer", 2, h_mss_kill_timer) != 0) {
        MSS_LOG("USER32/KERNEL32 timer or pump row missing");
        return -1;
    }
    installed = 1;
    MSS_LOG("installed USER32 timer + message/wait pump");
    return 6;
}
