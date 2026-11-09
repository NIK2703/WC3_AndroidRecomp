/*
 * waveout_hle.c - WINMM waveOut on top of the platform Wc3AudioApi.
 *
 * Normative owner: docs/AUDIO_ARCHITECTURE.md, A-4 (target architecture),
 * A-12.2 (root cause read from the lifted Mss32.dll) and A-12.4 (S0-S5).
 *
 * What previous revisions got wrong
 * ---------------------------------
 * rev 2..4 built a SECOND audio sink inside the HLE slice: a blocking-write
 * AAudio stream on its own pthread, a 128 KiB ring and a private device clock.
 * All WOM_DONE completions, WM_TIMER ticks and Miles' mixer were collapsed
 * into one w2_deliver_one() called from pump points.  Device measurements
 * (A-10/A-11) showed 54-75% exact PCM repeats with a period of exactly five
 * 50 ms buffers.  The lifted code settled it (A-12.1):
 *
 *   L_351130B0 waveOutProc  = producer, pushes the finished WAVEHDR into
 *                             Miles' ring [dev+0x7C], w = [dev+0x80]
 *   L_35112AF0 timer service = consumer, reads the ring, r = [dev+0x84],
 *                             renders fresh PCM, calls waveOutWrite
 *                             (ra=351120BB, i.e. NOT from waveOutProc)
 *
 * w2_deliver_one() fired the consumer (TimerProc) BEFORE the producer
 * (waveOutProc) on one arbitrary guest thread, so the consumer saw WHDR_DONE
 * with an empty Miles ring and resubmitted the previous PCM.  No amount of
 * transport tuning can fix a broken SPSC contract.
 *
 * This revision (rev 6) implements A-12.4 S1-S4
 * ---------------------------------------------
 *   S1  No AAudio, no pthread transport, no OS headers in the HLE slice.
 *       waveOutOpen/Write/Close/Reset go through platform->audio.  The
 *       platform owns the one RT callback, a lock-free SPSC byte ring and a
 *       NON-RT completion pump that reports bytes the device really played.
 *   S2  w2_callback_body() runs on a genuine guest thread created by a new
 *       shim method (own CPU / TEB / TLS / dispatcher state).  It is the ONLY
 *       caller of waveOutProc, strictly serialised, one WOM_DONE at a time.
 *       No dispatch() from h_w2_write and none from the message pump.
 *   S3  WM_TIMER is an independent wall-clock actor on gid=0: at most one
 *       coalesced TimerProc per pump, phase kept in due_ms.  WHDR_DONE is
 *       raised when the device CONSUMES the header's bytes, before the
 *       callback, in FIFO order.  waveOutWrite stays asynchronous and rejects
 *       a header still marked WHDR_INQUEUE.  The five WAVEHDRs are the only
 *       back-pressure - there is no wait inside waveOutWrite.
 *   S4  All A/B crutches are gone (DMA, devclock, delays, done_route,
 *       handoff, tick_order, ring size, EXCLUSIVE, resampler).  A-16 removes
 *       the v2 opt-out: legacy WINMM is opt-in via wave2_legacy_on only.
 *
 * PRODUCT PATH (owner decision 2026-11-09, A-15/A-16, ADR-032/033/034): this
 * slice is the active WINMM implementation in every variant, including
 * release, and now runs the FULL nfsu2 architecture through the platform
 * voice mixer: cursor-based region publication, positional completions,
 * sinc resampling to the device rate, PCM_FLOAT output and stale mute.
 * WC3_AUDIO_V2_ENABLED defaults to 1 and there is NO opt-out trigger: the
 * stale wave2_v2_off file from the rev-6 rollback is ignored.  The only
 * rollback is an explicit opt-IN file <WC3_DATA_DIR>/wave2_legacy_on.
 * Device A/B, --gate/--sync-log/--miles-state and blind listening are still
 * REQUIRED before this can be called a fix (A-7, E11, kill criterion A-12.4).
 * The product fast binding of as-built rows lives in winmm_fast_bind.c and
 * runs first in every variant; this install chains on top of it.
 *
 * runtime/legacy is never edited.  All guest memory access, HLE overrides and
 * the callback thread go through legacy_shim.h.  Diagnostics are gated by
 * WC3_DEBUG && WC3_DEBUG_AUDIO.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "direct_call.h"
#include "legacy_shim.h"
#include "platform.h"
#include "wc3_debug.h"
#include "waveout_hle.h"

/* ---- Win32 constants (mirrors winmm.h, kept local) ------------------- */
#define W2_WOM_DONE            0x03BDu
#define W2_WM_TIMER            0x0113u
#define W2_WHDR_DONE           0x00000001u
#define W2_WHDR_PREPARED       0x00000002u
#define W2_WHDR_INQUEUE        0x00000010u
#define W2_CALLBACK_TYPEMASK   0x00070000u
#define W2_CALLBACK_NULL        0x00000000u
#define W2_CALLBACK_FUNCTION   0x00030000u
#define W2_CALLBACK_EVENT      0x00050000u
#define W2_WAVE_FORMAT_QUERY   0x00000001u
#define W2_WAVE_FORMAT_PCM     0x0001u
#define W2_WAVE_MAPPER         0xFFFFFFFFu

#define W2_MMSYSERR_NOERROR       0u
#define W2_MMSYSERR_BADDEVICEID   2u
#define W2_MMSYSERR_ALLOCATED     4u
#define W2_MMSYSERR_INVALHANDLE   5u
#define W2_MMSYSERR_NOMEM         7u
#define W2_MMSYSERR_INVALPARAM   11u
#define W2_WAVERR_BADFORMAT       32u
#define W2_WAVERR_STILLPLAYING    33u
#define W2_WAVERR_UNPREPARED      34u

/* A lifted `ret` is `c->esp += 4; return;`, so any harmless return slot
 * works.  0x7EFFFFF1 is the one already used by sort_hle.c. */
#define W2_DISPATCH_RETURN     0x7EFFFFF1u

#ifndef WC3_AUDIO_V2_ENABLED
/* Product default after the owner's explicit 2026-11-09 decision: the new
 * WINMM -> Wc3AudioApi.publish -> AAudio pull path is active in release.
 * Legacy WINMM is reachable only through <WC3_DATA_DIR>/wave2_legacy_on. */
#define WC3_AUDIO_V2_ENABLED 1
#endif

/* Miles prepares five 50 ms headers.  16 leaves room for a second device and
 * for waveOutReset without ever allocating on the RT path. */
#define W2_HDR_MAX             16u
#define W2_MAILBOX_MAX         64u
/* S-2: one preallocated staging buffer per in-flight header.  The pull path
 * must never malloc/free inside waveOutWrite (nfsu2 publishes only the region
 * it wrote; the platform owns the PCM copy). */
#define W2_STAGE_BYTES         (16u * 1024u)
#define W2_LOG_BUDGET          4000u
#define W2_DUMP_LIMIT          (8u << 20)

typedef struct {
    uint32_t hdr;
    uint32_t ghwo, cb, inst, ev;
    uint64_t end_byte;      /* cumulative device byte that finishes this header */
    uint64_t end_frame;     /* absolute SOURCE frame that finishes this header */
    wc3_lifted_fn cb_fn;    /* O-98: resolved once in waveOutOpen */
} W2Pending;

/* Only a partial platform write creates a spill.  The pump feeds the rest
 * from a non-RT host thread; waveOutWrite never waits. */
typedef struct {
    W2Pending meta;
    uint8_t *pcm;
    uint32_t len, off;
} W2Spill;

typedef struct {
    uint32_t hdr, ghwo, cb, inst, ev;
    wc3_lifted_fn cb_fn;    /* O-98: survives the pump -> callback mailbox */
} W2Done;

typedef struct {
    _Atomic int used;
    _Atomic int closing;
    uint32_t ghwo, cb, inst, ev;
    wc3_lifted_fn cb_fn;    /* O-98: fixed callback target, resolved at open */
    int tag, channels, bits, rate;
    uint32_t src_align, out_align;
    uintptr_t stream;
    const Wc3AudioApi *audio;

    W2Pending pend[W2_HDR_MAX];
    unsigned pend_head, pend_count;
    W2Spill spill[W2_HDR_MAX];
    unsigned spill_head, spill_count;
    uint64_t written_bytes;    /* cumulative bytes accepted by the platform ring */
    uint64_t next_frame;       /* producer cursor, anchored to the play cursor */
    int reset_pending;         /* waveOutReset(): restart at frame 0 on next write */
    uintptr_t voice;           /* primary host-owned looping voice (nfsu2 port) */
    uint8_t *stage;            /* W2_HDR_MAX * W2_STAGE_BYTES, open() only */
} W2Dev;

static W2Dev g_dev[2];
static const Wc3TimeApi *g_time;
static const Wc3Platform *g_platform;
static _Atomic int g_enabled;

/* One short, non-blocking critical section for HLE-side bookkeeping.  There
 * is deliberately no pthread mutex here: the HLE layer owns no OS threads and
 * includes no OS headers (H5/H7).  No guest code runs while it is held. */
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static void w2_lock(void)
{
    /* AGENTS.md forbids a bare spinlock.  This bookkeeping flag is held only
     * for a few non-blocking stores, but a contended waiter must yield through
     * the platform instead of burning a core. */
    for (;;) {
        if (!atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire))
            return;
        if (g_platform && g_platform->thread.yield)
            g_platform->thread.yield(g_platform->thread.ctx);
    }
}
static void w2_unlock(void)
{
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

/* SPSC mailbox: producer = the platform completion pump, consumer = the
 * winmm callback guest thread.  acquire/release only. */
static W2Done g_mail[W2_MAILBOX_MAX];
static _Atomic uint32_t g_mail_head;
static _Atomic uint32_t g_mail_tail;

static int w2_mail_push(const W2Done *q)
{
    uint32_t h = atomic_load_explicit(&g_mail_head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&g_mail_tail, memory_order_acquire);
    if (h - t >= W2_MAILBOX_MAX) return 0;
    g_mail[h & (W2_MAILBOX_MAX - 1u)] = *q;
    atomic_store_explicit(&g_mail_head, h + 1u, memory_order_release);
    return 1;
}

static int w2_mail_pop(W2Done *q)
{
    uint32_t t = atomic_load_explicit(&g_mail_tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&g_mail_head, memory_order_acquire);
    if (t == h) return 0;
    *q = g_mail[t & (W2_MAILBOX_MAX - 1u)];
    atomic_store_explicit(&g_mail_tail, t + 1u, memory_order_release);
    return 1;
}

/* ---- Miles SPSC contract (A-12.4 S0) --------------------------------- */
static _Atomic uint32_t g_last_hdr;
static _Atomic uint32_t g_stat_rejected;
static _Atomic uint32_t g_stat_overflow, g_stat_spill;
static _Thread_local int w2_in_callback;
static _Thread_local int w2_in_tick;

#if WC3_DEBUG && WC3_DEBUG_AUDIO
static _Atomic uint32_t g_stat_writes, g_stat_done;
static _Atomic uint32_t g_log_budget;
#define W2LOG(...) do { \
    if (atomic_fetch_add(&g_log_budget, 1u) < W2_LOG_BUDGET) { \
        fprintf(stderr, "[wave2] " __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

static FILE *g_dump_file;
static uint32_t g_dump_bytes;
static int g_dump_state = -1;

/* WAVEHDR.dwUser at +0xC points at a cell holding Miles' device pointer;
 * [dev+0x78] = N, [dev+0x80] = w, [dev+0x84] = r.  Reading them gives a
 * direct check of the contract instead of a statistical PCM proxy. */
static uint32_t w2_miles_dev(uint32_t hdr)
{
    if (!hdr || !wc3_legacy_guest_ok(hdr, 32u)) return 0u;
    uint32_t user = wc3_legacy_rd32g(hdr + 0x0Cu);
    if (!user || !wc3_legacy_guest_ok(user, 4u)) return 0u;
    uint32_t dev = wc3_legacy_rd32g(user);
    if (!dev || !wc3_legacy_guest_ok(dev + 0x84u, 4u)) return 0u;
    return dev;
}
#else
#define W2LOG(...) do { } while (0)
#endif

static uint32_t w2_now_ms(void)
{
    if (g_time && g_time->monotonic_ns)
        return (uint32_t)(g_time->monotonic_ns(g_time->ctx) / 1000000ULL);
    return 0u;
}

static const char *w2_data_dir(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    return (dir && *dir) ? dir : NULL;
}

/* Configuration, not diagnostics: a release build must be able to roll back
 * without being rebuilt.  fopen() keeps the HLE slice free of OS headers. */
static int w2_trigger(const char *name)
{
    const char *dir = w2_data_dir();
    if (!dir) return 0;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

#if WC3_DEBUG && WC3_DEBUG_AUDIO
static int w2_dump_on(void)
{
    if (g_dump_state < 0) {
        const char *dir = w2_data_dir();
        char path[768];
        if (!dir) { g_dump_state = 0; return 0; }
        snprintf(path, sizeof path, "%s/wave2_dump_on", dir);
        FILE *probe = fopen(path, "rb");
        g_dump_state = probe ? 1 : 0;
        if (probe) fclose(probe);
        if (g_dump_state) {
            snprintf(path, sizeof path, "%s/wave2_dump.pcm", dir);
            g_dump_file = fopen(path, "wb");
            if (!g_dump_file) g_dump_state = 0;
            else fprintf(stderr, "[wave2] dump start\n");
        }
    }
    return g_dump_state;
}

/* The dump is the exact guest-rate PCM handed to the platform sink in
 * waveOutWrite, i.e. the bytes the speaker will play. */
static void w2_dump_pcm(const uint8_t *p, uint32_t n)
{
    if (!w2_dump_on() || !g_dump_file || !n) return;
    if (g_dump_bytes + n > W2_DUMP_LIMIT) return;
    fwrite(p, 1, n, g_dump_file);
    g_dump_bytes += n;
    if ((g_dump_bytes / n) % 64u == 63u) fflush(g_dump_file);
}

static void w2_miles_log(const CPU *c, const char *ev, uint32_t hdr)
{
    uint32_t w = 0u, r = 0u, N = 5u, gid = 0u;
    uint32_t dev = w2_miles_dev(hdr);
    if (dev && wc3_legacy_guest_ok(dev + 0x78u, 16u)) {
        N = wc3_legacy_rd32g(dev + 0x78u);
        w = wc3_legacy_rd32g(dev + 0x80u);
        r = wc3_legacy_rd32g(dev + 0x84u);
        if (N == 0u || N > 64u) N = 5u;
    }
    if (c && c->fs_base && wc3_legacy_guest_ok(c->fs_base + 0x24u, 4u))
        gid = wc3_legacy_rd32g(c->fs_base + 0x24u);
    W2LOG("miles t=%u ev=%s w=%u r=%u N=%u gid=%u hdr=%08X",
          w2_now_ms(), ev, w, r, N, gid, hdr);
}
#else
#define w2_dump_pcm(p, n) do { } while (0)
#define w2_miles_log(c, ev, hdr) do { } while (0)
#endif

static W2Dev *w2_dev(uint32_t ghwo)
{
    for (unsigned i = 0; i < sizeof g_dev / sizeof g_dev[0]; i++)
        if (atomic_load_explicit(&g_dev[i].used, memory_order_acquire) &&
            g_dev[i].ghwo == ghwo) return &g_dev[i];
    return NULL;
}

static W2Dev *w2_active(void)
{
    for (unsigned i = 0; i < sizeof g_dev / sizeof g_dev[0]; i++)
        if (atomic_load_explicit(&g_dev[i].used, memory_order_acquire))
            return &g_dev[i];
    return NULL;
}

unsigned long long wc3_hle_winmm_played_frames(void)
{
    W2Dev *d = w2_active();
    if (!d || !d->audio || !d->audio->consumed_bytes || !d->out_align) return 0;
    return (unsigned long long)(d->audio->consumed_bytes(d->audio->ctx,
                                                         d->stream) /
                                d->out_align);
}

/* ---- guest callback invocation (lock-free, no host lock held) -------- */

/* stdcall arguments are pushed right-to-left; the lifted `ret` needs a
 * harmless return slot, so W2_DISPATCH_RETURN goes last.  Never called while
 * g_lock is held: dispatch() must not be reachable from a locked region. */
static void w2_invoke(CPU *c, uint32_t target, wc3_lifted_fn resolved,
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
        wc3_legacy_wr32g(c->esp, W2_DISPATCH_RETURN);
        /* O-98: the fixed target is normally resolved at open/set-time.  An
         * HLE thunk or an unrecognised VA keeps the old dispatcher path. */
        if (resolved)
            wc3_dispatch_resolved(c, resolved);
        else
            dispatch(c, target);
    }
    c->esp = esp;
    c->ebx = ebx; c->esi = esi; c->edi = edi; c->ebp = ebp;
}

static void w2_invoke_proc(CPU *c, const W2Done *q)
{
    if (!q->cb) return;                 /* CALLBACK_NULL / event-only */
    const uint32_t args[5] = { q->ghwo, W2_WOM_DONE, q->inst, q->hdr, 0u };
    w2_invoke(c, q->cb, q->cb_fn, args, 5u);
}

/* ---- S2: the winmm callback thread ----------------------------------- */
/* Runs as a genuine guest thread (own CPU/TEB/TLS, g_self_thread set by the
 * as-built guest_thread_spawn).  It is the only producer of WOM_DONE into
 * Miles' ring and must never be the caller of waveOutWrite. */
static void w2_callback_body(CPU *c, void *opaque)
{
    (void)opaque;
    for (;;) {
        if (wc3_legacy_guest_callback_wait() < 0) return;
        W2Done q;
        while (w2_mail_pop(&q)) {
            /* waveOutReset/Close also publish completions, so a closed device
             * must not swallow them: winmm delivers WOM_DONE for every header
             * it owned.  The record carries everything the callback needs. */
            w2_in_callback = 1;
            w2_miles_log(c, "done", q.hdr);
            atomic_store(&g_last_hdr, q.hdr);
            w2_invoke_proc(c, &q);
            w2_in_callback = 0;
#if WC3_DEBUG && WC3_DEBUG_AUDIO
            atomic_fetch_add(&g_stat_done, 1u);
#endif
        }
    }
}

/* ---- S3: independent, coalesced WM_TIMER ------------------------------ */
/* WM_TIMER belongs to USER32 and runs on wall clock, on the guest thread that
 * created the timer.  The pump wrapper IS the Win32 message pump; it is
 * deliberately NOT tied to WOM_DONE.
 *
 * v2 replaces waveOutOpen, which also means the quarantined wave_init() never
 * runs and g_wave_ret_thunk stays 0: legacy timer_service() becomes a no-op
 * for EVERY timer, not just Miles'.  So this slice owns the whole timer table
 * while active.  Like Win32, at most one tick per timer per pump and the phase
 * is preserved in due_ms (no catch-up bursts).  Thread ownership comes from
 * the TEB, so no OS header is needed. */
#define W2_TIMER_MAX 32u
typedef struct {
    int used;
    uint32_t hwnd, id, elapse, callback;
    uint32_t owner_gid, due_ms;
    wc3_lifted_fn callback_fn; /* O-98: resolved once in SetTimer */
} W2Timer;
static W2Timer g_timers[W2_TIMER_MAX];
static uint32_t g_timer_next_id = 0xA000u;
#if WC3_DEBUG && WC3_DEBUG_AUDIO
static _Atomic uint32_t g_last_tick_ms;
#endif

#if WC3_DEBUG && WC3_DEBUG_AUDIO
static int w2_is_mss_addr(uint32_t va)
{
    return va >= 0x35100000u && va < 0x35400000u;
}
#endif

static uint32_t w2_gid(const CPU *c)
{
    if (c && c->fs_base && wc3_legacy_guest_ok(c->fs_base + 0x24u, 4u))
        return wc3_legacy_rd32g(c->fs_base + 0x24u);
    return 0u;
}

static void w2_fire_timer(CPU *c, const W2Timer *t, uint32_t tick_ms)
{
    if (!t->callback) return;   /* hwnd-only timers need a real message queue */
#if WC3_DEBUG && WC3_DEBUG_AUDIO
    if (w2_is_mss_addr(t->callback)) {
        uint32_t prev = atomic_exchange(&g_last_tick_ms, tick_ms);
        W2LOG("tick t=%u dt=%u cb=%08X", tick_ms,
              prev ? tick_ms - prev : 0u, t->callback);
        w2_miles_log(c, "timer", atomic_load(&g_last_hdr));
    }
#endif
    const uint32_t args[4] = { t->hwnd, W2_WM_TIMER, t->id, tick_ms };
    w2_invoke(c, t->callback, t->callback_fn, args, 4u);
}

static void w2_wall_tick(CPU *c)
{
    if (!c || w2_in_tick) return;
    uint32_t now = w2_now_ms();
    uint32_t gid = w2_gid(c);
    w2_lock();
    /* Snapshot under the short bookkeeping lock; every dispatch happens
     * after it is released, so no host lock is held across guest code. */
    W2Timer due[W2_TIMER_MAX];
    unsigned ndue = 0;
    for (unsigned i = 0; i < W2_TIMER_MAX; i++) {
        W2Timer *t = &g_timers[i];
        if (!t->used || !t->callback || t->owner_gid != gid) continue;
        if ((int32_t)(now - t->due_ms) < 0) continue;
        due[ndue++] = *t;
        uint32_t elapse = t->elapse ? t->elapse : 50u;
        t->due_ms = now + elapse;
    }
    w2_unlock();

    if (!ndue) return;
    w2_in_tick = 1;
    for (unsigned i = 0; i < ndue; i++) w2_fire_timer(c, &due[i], now);
    w2_in_tick = 0;
}

/* ---- S1: completion pump --------------------------------------------- */
/* Called by the platform's NON-RT pump thread with the cumulative number of
 * bytes the real-time callback actually removed from the ring.  It raises
 * WHDR_DONE in guest memory at the moment of consumption, then publishes the
 * WOM_DONE for the callback thread.  No dispatch(), no allocation in the RT
 * path, no artificial delay. */
static void w2_finish_locked(const W2Pending *p)
{
    if (p->hdr && wc3_legacy_guest_ok(p->hdr, 32u)) {
        uint32_t f = wc3_legacy_rd32g(p->hdr + 16u);
        f = (f & ~W2_WHDR_INQUEUE) | W2_WHDR_DONE | W2_WHDR_PREPARED;
        wc3_legacy_wr32g(p->hdr + 16u, f);
    }
    W2Done q = { p->hdr, p->ghwo, p->cb, p->inst, p->ev, p->cb_fn };
    if (!w2_mail_push(&q)) atomic_fetch_add(&g_stat_overflow, 1u);
}

static void w2_on_pump(void *opaque, uint64_t consumed_total)
{
    W2Dev *d = (W2Dev *)opaque;
    if (!d || atomic_load_explicit(&d->closing, memory_order_acquire)) return;
    int pushed = 0;

    w2_lock();
    /* A partially accepted header is fed here, from a normal host thread.
     * audio.write() is the only producer of the platform SPSC ring. */
    while (d->spill_count && d->audio && !d->audio->publish &&
           d->audio->write) {
        W2Spill *s = &d->spill[d->spill_head];
        uint32_t left = s->len - s->off;
        uint32_t accepted = 0;
        if (left && d->audio->write(d->audio->ctx, d->stream,
                                    s->pcm + s->off, left, &accepted) != 0)
            break;
        if (accepted > left) accepted = left;
        s->off += accepted;
        d->written_bytes += accepted;
        if (s->off < s->len) break;      /* ring full: try on the next wake */
        if (d->pend_count < W2_HDR_MAX) {
            W2Pending *p = &d->pend[(d->pend_head + d->pend_count) %
                                    W2_HDR_MAX];
            *p = s->meta;
            p->end_byte = d->written_bytes;
            d->pend_count++;
        }
        free(s->pcm);
        s->pcm = NULL;
        d->spill_head = (d->spill_head + 1u) % W2_HDR_MAX;
        d->spill_count--;
    }

    /* Positional completion (full nfsu2 port): a header finishes only when
     * the mixer's PLAY cursor has passed its published region.  The old rule
     * compared a producer byte counter with a wall-clock device counter; when
     * the guest stalled those diverged and completions fired instantly, which
     * is the "step 5" resubmission avalanche.  In the nfsu2 topology the
     * device clock never drives the producer. */
    uint64_t play = 0u;
    int positional = (d->voice && d->audio->play_frame) ? 1 : 0;
    if (positional) play = d->audio->play_frame(d->audio->ctx, d->stream);
    while (d->pend_count) {
        W2Pending *p = &d->pend[d->pend_head];
        int done = positional ? (p->end_frame <= play)
                              : (p->end_byte <= consumed_total);
        if (!done) break;
        w2_finish_locked(p);
        d->pend_head = (d->pend_head + 1u) % W2_HDR_MAX;
        d->pend_count--;
        pushed++;
    }
    w2_unlock();

    if (pushed) {
        W2LOG("pump consumed=%llu completed=%d queued=%u",
              (unsigned long long)consumed_total, pushed,
              (unsigned)atomic_load(&g_stat_done));
        wc3_legacy_guest_callback_wake();
    }
}

/* ---- HLE handlers ----------------------------------------------------- */

static void h_w2_get_num_devs(CPU *c) { c->eax = 1u; }

typedef struct {
    uint16_t wMid, wPid;
    uint32_t vDriverVersion;
    char szPname[32];
    uint32_t dwFormats;
    uint16_t wChannels, wReserved1;
    uint32_t dwSupport;
} W2CapsA;

static void h_w2_get_dev_caps(CPU *c)
{
    uint32_t dev = wc3_legacy_arg32(c, 0);
    uint32_t out = wc3_legacy_arg32(c, 1);
    uint32_t sz = wc3_legacy_arg32(c, 2);
    if (dev != 0u && dev != W2_WAVE_MAPPER) {
        c->eax = W2_MMSYSERR_BADDEVICEID;
        return;
    }
    W2CapsA caps;
    memset(&caps, 0, sizeof caps);
    caps.wMid = 1u; caps.wPid = 2u; caps.vDriverVersion = 0x0501u;
    memcpy(caps.szPname, "WC3 Platform Audio", 19);
    caps.dwFormats = 0x00000FFFu;
    caps.wChannels = 2u;
    caps.dwSupport = 0x00000003u;   /* WAVECAPS_LRVOLUME | WAVECAPS_PITCH */
    if (out && sz && wc3_legacy_guest_ok(out, sz)) {
        uint32_t n = sz < sizeof caps ? sz : (uint32_t)sizeof caps;
        const uint8_t *p = (const uint8_t *)&caps;
        for (uint32_t i = 0; i < n; i++) wc3_legacy_wr8g(out + i, p[i]);
    }
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_open(CPU *c)
{
    uint32_t phwo = wc3_legacy_arg32(c, 0);
    uint32_t devid = wc3_legacy_arg32(c, 1);
    uint32_t pwfx = wc3_legacy_arg32(c, 2);
    uint32_t cb = wc3_legacy_arg32(c, 3);
    uint32_t inst = wc3_legacy_arg32(c, 4);
    uint32_t flags = wc3_legacy_arg32(c, 5);

    if (!phwo || !wc3_legacy_guest_ok(phwo, 4u) ||
        !pwfx || !wc3_legacy_guest_ok(pwfx, 16u)) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    int tag = (int)wc3_legacy_rd16g(pwfx + 0);
    int ch = (int)wc3_legacy_rd16g(pwfx + 2);
    int rate = (int)wc3_legacy_rd32g(pwfx + 4);
    int bits = (int)wc3_legacy_rd16g(pwfx + 14);
    uint32_t type = flags & W2_CALLBACK_TYPEMASK;

    if (devid != 0u && devid != W2_WAVE_MAPPER) {
        c->eax = W2_MMSYSERR_BADDEVICEID;
        return;
    }
    if (tag != W2_WAVE_FORMAT_PCM || bits != 16 || ch < 1 || ch > 2 ||
        rate < 8000 || rate > 192000) {
        c->eax = W2_WAVERR_BADFORMAT;
        return;
    }
    if (flags & W2_WAVE_FORMAT_QUERY) {
        c->eax = W2_MMSYSERR_NOERROR;
        return;
    }
    if (type != W2_CALLBACK_NULL && type != W2_CALLBACK_FUNCTION &&
        type != W2_CALLBACK_EVENT) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    if (type == W2_CALLBACK_FUNCTION && !cb) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    if (!g_platform || !g_platform->audio.open ||
        !g_platform->audio.write || !g_platform->audio.close ||
        !g_platform->audio.set_consumed ||
        !g_platform->audio.consumed_bytes) {
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }

    /* O-98: waveOutOpen fixes the callback address for the life of the
     * device.  Resolve it once here; a NULL result keeps the normal
     * dispatcher as the fallback for an HLE thunk or an unknown VA. */
    wc3_lifted_fn cb_fn = (type == W2_CALLBACK_FUNCTION && cb)
        ? wc3_dispatch_resolve(cb) : NULL;

    /* S2: the serialised winmm callback thread must exist before the first
     * completion can be published.  Created exactly once. */
    if (wc3_legacy_guest_callback_thread_start(w2_callback_body, NULL) != 0) {
        W2LOG("callback guest thread unavailable");
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }

    int slot = -1;
    w2_lock();
    for (unsigned i = 0; i < sizeof g_dev / sizeof g_dev[0]; i++)
        if (!atomic_load_explicit(&g_dev[i].used, memory_order_acquire)) {
            slot = (int)i; break;
        }
    if (slot < 0) {
        w2_unlock();
        c->eax = W2_MMSYSERR_ALLOCATED;
        return;
    }
    W2Dev *d = &g_dev[slot];
    memset(d, 0, sizeof *d);
    d->ghwo = 0x000A0000u + (uint32_t)slot * 4u;
    d->cb = (type == W2_CALLBACK_FUNCTION) ? cb : 0u;
    d->cb_fn = cb_fn;
    d->inst = inst;
    d->ev = (type == W2_CALLBACK_EVENT) ? (cb ? cb : inst) : 0u;
    d->tag = tag; d->channels = ch; d->bits = bits; d->rate = rate;
    d->src_align = (uint32_t)ch * (uint32_t)(bits / 8);
    d->out_align = (uint32_t)ch * 2u;
    d->audio = &g_platform->audio;
    /* S-2: no per-write malloc.  Allocate the staging pool once, at open. */
    if (d->audio->publish) {
        d->stage = (uint8_t *)calloc(W2_HDR_MAX, W2_STAGE_BYTES);
        if (!d->stage) {
            w2_unlock();
            c->eax = W2_MMSYSERR_NOMEM;
            return;
        }
    }
    atomic_store_explicit(&d->used, 1, memory_order_release);
    uint32_t ghwo = d->ghwo;
    w2_unlock();

    uintptr_t stream = 0;
    if (d->audio->open(d->audio->ctx, (uint32_t)rate, (uint32_t)ch,
                       (uint32_t)bits, &stream) != 0) {
        W2LOG("platform audio open failed");
        free(d->stage);
        d->stage = NULL;
        atomic_store_explicit(&d->used, 0, memory_order_release);
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }
    d->stream = stream;
    /* Full nfsu2 port: bind WINMM to the platform's primary LOOPING voice.
     * waveOutWrite becomes a Lock/Unlock of one region anchored at
     * play_cursor + 10 ms instead of a push onto a producer counter.  This
     * is the guest-visible half that makes the "step 5" repeat impossible. */
    if (d->audio->primary_voice) {
        d->voice = d->audio->primary_voice(d->audio->ctx, stream);
        if (d->voice && d->audio->voice_control)
            (void)d->audio->voice_control(d->audio->ctx, d->voice, 1, 1,
                                          (uint32_t)rate, 0, 0);
    }
    if (!d->voice)
        W2LOG("voice mixer unavailable: falling back to producer-counter "
              "publication (host test stubs only)");
    if (d->audio->set_consumed(d->audio->ctx, stream, w2_on_pump, d) != 0) {
        W2LOG("platform consumed pump unavailable");
        (void)d->audio->close(d->audio->ctx, stream);
        free(d->stage);
        d->stage = NULL;
        atomic_store_explicit(&d->used, 0, memory_order_release);
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }

    wc3_legacy_wr32g(phwo, ghwo);
    c->eax = W2_MMSYSERR_NOERROR;
#if WC3_DEBUG && WC3_DEBUG_AUDIO
    /* Measured, not declared: WC3_AUDIO_LATENCY_MS asks for a buffer
     * duration and AAudio reports what it actually granted.  This is an A/B
     * input only (see A-13), never a "clicks are fixed" claim. */
    uint32_t latency_frames = d->audio->latency_frames
        ? d->audio->latency_frames(d->audio->ctx, stream) : 0u;
    W2LOG("open ghwo=%u type=%u cb=%08X rate=%d ch=%d bits=%d "
          "platform=Wc3AudioApi callback_thread=guest "
          "device_buffer_frames=%u (%.1f ms)",
          ghwo, type, cb, rate, ch, bits, latency_frames,
          rate > 0 ? (double)latency_frames * 1000.0 / (double)rate : 0.0);
#else
    W2LOG("open ghwo=%u type=%u cb=%08X rate=%d ch=%d bits=%d "
          "platform=Wc3AudioApi callback_thread=guest",
          ghwo, type, cb, rate, ch, bits);
#endif
}

/* Flush everything still owned by winmm.  Called with g_lock held. */
static void w2_flush_locked(W2Dev *d)
{
    while (d->pend_count) {
        w2_finish_locked(&d->pend[d->pend_head]);
        d->pend_head = (d->pend_head + 1u) % W2_HDR_MAX;
        d->pend_count--;
    }
    while (d->spill_count) {
        W2Spill *s = &d->spill[d->spill_head];
        w2_finish_locked(&s->meta);
        free(s->pcm);
        s->pcm = NULL;
        d->spill_head = (d->spill_head + 1u) % W2_HDR_MAX;
        d->spill_count--;
    }
}

static void h_w2_close(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    W2Dev *d = w2_dev(ghwo);
    if (!d) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    atomic_store_explicit(&d->closing, 1, memory_order_release);
    w2_lock();
    w2_flush_locked(d);
    const Wc3AudioApi *audio = d->audio;
    uintptr_t stream = d->stream;
    w2_unlock();
    wc3_legacy_guest_callback_wake();

    /* close() stops the RT callback and joins the completion pump before
     * freeing the ring, so w2_on_pump cannot touch a freed device. */
    if (audio && audio->close) (void)audio->close(audio->ctx, stream);
    w2_lock();
    free(d->stage);
    d->stage = NULL;
    memset(d, 0, sizeof *d);
    atomic_store_explicit(&d->used, 0, memory_order_release);
    w2_unlock();
    c->eax = W2_MMSYSERR_NOERROR;
    W2LOG("close ghwo=%u", ghwo);
}

static void h_w2_prepare_header(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    uint32_t hdr = wc3_legacy_arg32(c, 1);
    if (!w2_dev(ghwo)) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    if (!hdr || !wc3_legacy_guest_ok(hdr, 32u)) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    uint32_t f = wc3_legacy_rd32g(hdr + 16);
    if (f & W2_WHDR_INQUEUE) {
        c->eax = W2_WAVERR_STILLPLAYING;
        return;
    }
    f = (f & ~W2_WHDR_DONE) | W2_WHDR_PREPARED;
    wc3_legacy_wr32g(hdr + 16, f);
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_unprepare_header(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    uint32_t hdr = wc3_legacy_arg32(c, 1);
    if (!w2_dev(ghwo)) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    if (!hdr || !wc3_legacy_guest_ok(hdr, 32u)) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    uint32_t f = wc3_legacy_rd32g(hdr + 16);
    if (f & W2_WHDR_INQUEUE) {
        c->eax = W2_WAVERR_STILLPLAYING;
        return;
    }
    wc3_legacy_wr32g(hdr + 16, f & ~W2_WHDR_PREPARED);
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_write(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    uint32_t hdr = wc3_legacy_arg32(c, 1);
    W2Dev *d = w2_dev(ghwo);
    if (!d) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    if (!hdr || !wc3_legacy_guest_ok(hdr, 32u)) {
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }

    /* winmm forbids waveOutWrite from inside waveOutProc.  The callback has
     * its own guest thread, so a TLS flag can detect the violation directly
     * instead of inferring it from PCM statistics (A-12.4 S0). */
    if (w2_in_callback) {
        W2LOG("miles t=%u ev=violation w=0 r=0 N=5 gid=0 hdr=%08X "
              "(waveOutWrite from waveOutProc)", w2_now_ms(), hdr);
        c->eax = W2_WAVERR_STILLPLAYING;
        return;
    }

    w2_miles_log(c, "write", hdr);
    atomic_store(&g_last_hdr, hdr);

    uint32_t data = wc3_legacy_rd32g(hdr + 0);
    uint32_t len = wc3_legacy_rd32g(hdr + 4);
    uint32_t f = wc3_legacy_rd32g(hdr + 16);

    w2_lock();
    if (atomic_load_explicit(&d->closing, memory_order_acquire) ||
        !d->src_align) {
        w2_unlock();
        c->eax = W2_MMSYSERR_INVALHANDLE;
        return;
    }
    if (!(f & W2_WHDR_PREPARED)) {
        w2_unlock();
        c->eax = W2_WAVERR_UNPREPARED;
        return;
    }
    if (f & W2_WHDR_INQUEUE) {
        atomic_fetch_add(&g_stat_rejected, 1u);
        w2_unlock();
        c->eax = W2_WAVERR_STILLPLAYING;
        W2LOG("reject hdr=%08X still INQUEUE", hdr);
        return;
    }
    uint32_t frames = len / d->src_align;
    uint32_t need = frames * d->out_align;
    if (data && len && !wc3_legacy_guest_ok(data, len)) {
        w2_unlock();
        c->eax = W2_MMSYSERR_INVALPARAM;
        return;
    }
    if (d->pend_count + d->spill_count >= W2_HDR_MAX) {
        w2_unlock();
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }

    W2Pending meta;
    meta.hdr = hdr;
    meta.ghwo = d->ghwo;
    meta.cb = d->cb;
    meta.cb_fn = d->cb_fn;
    meta.inst = d->inst;
    meta.ev = d->ev;
    meta.end_byte = 0u;
    int first_after_reset = d->reset_pending;

    wc3_legacy_wr32g(hdr + 16,
                     (f & ~W2_WHDR_DONE) | W2_WHDR_INQUEUE | W2_WHDR_PREPARED);

    /* winmm completes a zero-length header (and Miles' one-frame device
     * probe) immediately, without audible playback.  No dispatch() here:
     * it goes through the callback thread like every other WOM_DONE. */
    if (need == 0u || need <= 64u) {
        w2_finish_locked(&meta);
        w2_unlock();
        wc3_legacy_guest_callback_wake();
        c->eax = W2_MMSYSERR_NOERROR;
        return;
    }

    /* S-2 pull path: publish ONLY the submitted region into the host-owned
     * ring.  No malloc/free in waveOutWrite, no spill queue, no partial
     * accept: five headers (44160 B) always fit the 64 KiB platform ring. */
    if (d->audio->publish) {
        if (!d->stage || need > W2_STAGE_BYTES) {
            wc3_legacy_wr32g(hdr + 16, f);
            w2_unlock();
            c->eax = W2_MMSYSERR_NOMEM;
            return;
        }
        unsigned si = (d->pend_head + d->pend_count) % W2_HDR_MAX;
        uint8_t *buf = d->stage + (size_t)si * W2_STAGE_BYTES;
        if (data && wc3_legacy_guest_ok(data, need))
            memcpy(buf, (const void *)(uintptr_t)data, need);
        else
            memset(buf, 0, need);
        w2_dump_pcm(buf, need);

        /* nfsu2 contract: the producer writes at the PLAY cursor plus one
         * 10 ms lookahead, exactly like DirectSound GetCurrentPosition.  A
         * late guest is re-anchored forward (a silence gap), never allowed to
         * publish into the past and never allowed to replay stale PCM. */
        uint32_t nframes = need / d->out_align;
        uint64_t frame_base;
        if (d->voice && d->audio->play_frame) {
            if (first_after_reset) {
                /* waveOutReset(): the ring is silent and its cursor was
                 * zeroed, so the first new header starts at frame 0 rather
                 * than one lookahead into the discarded buffer. */
                frame_base = 0u;
            } else {
                uint64_t play = d->audio->play_frame(d->audio->ctx, d->stream);
                uint32_t dev_rate = d->audio->device_rate
                    ? d->audio->device_rate(d->audio->ctx, d->stream)
                    : (uint32_t)d->rate;
                uint64_t lookahead = (uint64_t)(dev_rate / 100u);
                uint64_t anchor = play + (lookahead ? lookahead : 1u);
                if (d->next_frame < anchor) d->next_frame = anchor;
                frame_base = d->next_frame;
            }
        } else {
            frame_base = d->written_bytes / d->out_align;
        }
        uint32_t accepted = 0;
        int rc;
        if (d->voice && d->audio->voice_publish)
            rc = d->audio->voice_publish(d->audio->ctx, d->voice,
                                         frame_base, buf, need, &accepted);
        else
            rc = d->audio->publish(d->audio->ctx, d->stream, frame_base,
                                   buf, need, &accepted);
        if (rc != 0 || accepted != need) {
            wc3_legacy_wr32g(hdr + 16, f);
            w2_unlock();
            c->eax = W2_MMSYSERR_NOMEM;
            return;
        }
        if (first_after_reset) {
            if (d->voice) {
                if (d->audio->voice_seek)
                    (void)d->audio->voice_seek(d->audio->ctx, d->voice,
                                               frame_base);
                if (d->audio->voice_control)
                    (void)d->audio->voice_control(d->audio->ctx, d->voice,
                                                  1, 1, (uint32_t)d->rate,
                                                  0, 0);
            }
            d->reset_pending = 0;
        }
        d->written_bytes += need;
        d->next_frame = frame_base + nframes;
        meta.end_byte = d->written_bytes;
        meta.end_frame = frame_base + nframes;
        d->pend[si] = meta;
        d->pend_count++;
        w2_unlock();
#if WC3_DEBUG && WC3_DEBUG_AUDIO
        atomic_fetch_add(&g_stat_writes, 1u);
        W2LOG("pull write hdr=%08X frame=%llu bytes=%u",
              hdr, (unsigned long long)frame_base, need);
#endif
        c->eax = W2_MMSYSERR_NOERROR;
        return;
    }

    /* Legacy linear write() A/B path.  Copy at submit.  A-9.7.2's DMA
     * hypothesis was disproved on the device: the five WAVEHDRs have distinct
     * lpData, so a submit-time snapshot is correct. */
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) {
        wc3_legacy_wr32g(hdr + 16, f);
        w2_unlock();
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }
    if (data && wc3_legacy_guest_ok(data, need))
        memcpy(buf, (const void *)(uintptr_t)data, need);
    else
        memset(buf, 0, need);
    w2_dump_pcm(buf, need);

    uint32_t accepted = 0;
    int rc = d->audio->write(d->audio->ctx, d->stream, buf, need, &accepted);
    if (rc != 0 || accepted > need) {
        free(buf);
        wc3_legacy_wr32g(hdr + 16, f);
        w2_unlock();
        c->eax = W2_MMSYSERR_NOMEM;
        return;
    }
    d->written_bytes += accepted;
    d->reset_pending = 0;
    if (accepted == need) {
        free(buf);
        meta.end_byte = d->written_bytes;
        W2Pending *p = &d->pend[(d->pend_head + d->pend_count) % W2_HDR_MAX];
        *p = meta;
        d->pend_count++;
    } else {
        /* Asynchronous, never blocking: the pump finishes the tail. */
        W2Spill *s = &d->spill[(d->spill_head + d->spill_count) % W2_HDR_MAX];
        s->meta = meta;
        s->pcm = buf;
        s->len = need;
        s->off = accepted;
        d->spill_count++;
        atomic_fetch_add(&g_stat_spill, 1u);
    }
    w2_unlock();

#if WC3_DEBUG && WC3_DEBUG_AUDIO
    unsigned n = atomic_fetch_add(&g_stat_writes, 1u);
    if ((n % 100u) == 0u)
        W2LOG("stats writes=%u done=%u rejected=%u overflow=%u spill=%u "
              "played=%llu", n, atomic_load(&g_stat_done),
              atomic_load(&g_stat_rejected), atomic_load(&g_stat_overflow),
              atomic_load(&g_stat_spill),
              (unsigned long long)(d->audio->consumed_bytes(
                  d->audio->ctx, d->stream) / d->out_align));
#endif
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_reset(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    W2Dev *d = w2_dev(ghwo);
    if (!d) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    w2_lock();
    w2_flush_locked(d);
    /* Windows: waveOutReset stops playback, marks queued headers done, and
     * resets the current position.  The persistent voice ring would otherwise
     * replay the old buffer on the next write, so discard it as well. */
    d->next_frame = 0u;
    d->written_bytes = 0u;
    d->reset_pending = 1;
    if (d->voice && d->audio) {
        if (d->audio->voice_reset) {
            (void)d->audio->voice_reset(d->audio->ctx, d->voice);
        } else {
            if (d->audio->voice_seek)
                (void)d->audio->voice_seek(d->audio->ctx, d->voice, 0u);
            if (d->audio->voice_control)
                (void)d->audio->voice_control(d->audio->ctx, d->voice, 0, 1,
                                               (uint32_t)d->rate, 0, 0);
        }
    }
    w2_unlock();
    wc3_legacy_guest_callback_wake();
    c->eax = W2_MMSYSERR_NOERROR;
    W2LOG("reset ghwo=%u", ghwo);
}

static void h_w2_get_id(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    uint32_t out = wc3_legacy_arg32(c, 1);
    if (!w2_dev(ghwo)) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    if (out && wc3_legacy_guest_ok(out, 4u)) wc3_legacy_wr32g(out, 0u);
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_get_volume(CPU *c)
{
    uint32_t ghwo = wc3_legacy_arg32(c, 0);
    uint32_t out = wc3_legacy_arg32(c, 1);
    if (!w2_dev(ghwo)) { c->eax = W2_MMSYSERR_INVALHANDLE; return; }
    if (out && wc3_legacy_guest_ok(out, 4u)) wc3_legacy_wr32g(out, 0xFFFFFFFFu);
    c->eax = W2_MMSYSERR_NOERROR;
}

static void h_w2_set_volume(CPU *c)
{
    (void)c;
    c->eax = W2_MMSYSERR_NOERROR;
}

/* ---- USER32 wrappers -------------------------------------------------- */
/* Only WM_TIMER is injected here.  No WOM_DONE is dispatched from the pump
 * or from waveOutWrite: that is what collapsed the three Win32 actors in
 * rev 2..4 (A-12.2). */
typedef void (*W2PumpFn)(CPU *c);
static W2PumpFn w2_prev_get_message;
static W2PumpFn w2_prev_peek_message;

static void h_w2_pump_get(CPU *c)
{
    w2_wall_tick(c);
    if (w2_prev_get_message) w2_prev_get_message(c);
}

static void h_w2_pump_peek(CPU *c)
{
    w2_wall_tick(c);
    if (w2_prev_peek_message) w2_prev_peek_message(c);
}

/* Own the complete USER32 timer table while v2 is active: the quarantined
 * timer_service() is inert without wave_init().  The Mss32 50 ms mixer timer
 * is just one entry; it gets no special pacing. */
static void h_w2_set_timer(CPU *c)
{
    uint32_t hwnd = wc3_legacy_arg32(c, 0);
    uint32_t id = wc3_legacy_arg32(c, 1);
    uint32_t elapse = wc3_legacy_arg32(c, 2);
    uint32_t cb = wc3_legacy_arg32(c, 3);
    if (elapse == 0u) elapse = 1u;
    uint32_t gid = w2_gid(c);
    uint32_t now = w2_now_ms();

    /* O-98: SetTimer fixes the callback for this timer slot.  Resolve it
     * once, outside the bookkeeping lock; SetTimer with a new callback
     * refreshes it. */
    wc3_lifted_fn callback_fn = cb ? wc3_dispatch_resolve(cb) : NULL;

    w2_lock();
    W2Timer *t = NULL;
    if (id) {
        for (unsigned i = 0; i < W2_TIMER_MAX; i++) {
            W2Timer *q = &g_timers[i];
            if (q->used && q->owner_gid == gid &&
                q->hwnd == hwnd && q->id == id) { t = q; break; }
        }
    }
    if (!t) {
        for (unsigned i = 0; i < W2_TIMER_MAX; i++)
            if (!g_timers[i].used) { t = &g_timers[i]; break; }
    }
    if (!t) {
        w2_unlock();
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
    w2_unlock();
    c->eax = id;
    W2LOG("timer set hwnd=%u id=%u elapse=%u cb=%08X gid=%u",
          hwnd, id, elapse, cb, gid);
}

static void h_w2_kill_timer(CPU *c)
{
    uint32_t hwnd = wc3_legacy_arg32(c, 0);
    uint32_t id = wc3_legacy_arg32(c, 1);
    uint32_t gid = w2_gid(c);
    int found = 0;
    w2_lock();
    for (unsigned i = 0; i < W2_TIMER_MAX; i++) {
        W2Timer *t = &g_timers[i];
        if (!t->used || t->owner_gid != gid) continue;
        if (id && t->id != id) continue;
        if (hwnd && t->hwnd != hwnd) continue;
        t->used = 0;
        found = 1;
        if (id) break;
    }
    w2_unlock();
    c->eax = found ? 1u : 0u;
    W2LOG("timer kill hwnd=%u id=%u gid=%u found=%d", hwnd, id, gid, found);
}

typedef struct {
    const char *name;
    void (*impl)(CPU *c);
} W2Override;

static const W2Override g_overrides[] = {
    { "waveOutOpen",             h_w2_open },
    { "waveOutClose",            h_w2_close },
    { "waveOutWrite",            h_w2_write },
    { "waveOutReset",            h_w2_reset },
    { "waveOutPrepareHeader",    h_w2_prepare_header },
    { "waveOutUnprepareHeader",  h_w2_unprepare_header },
    { "waveOutGetNumDevs",       h_w2_get_num_devs },
    { "waveOutGetDevCapsA",      h_w2_get_dev_caps },
    { "waveOutGetID",            h_w2_get_id },
    { "waveOutGetVolume",        h_w2_get_volume },
    { "waveOutSetVolume",        h_w2_set_volume },
};

int wc3_hle_winmm_waveout_install(void)
{
    static int installed = 0;
    if (installed) return 15;   /* chaining a wrapper twice would recurse */

    g_platform = wc3_platform();
    if (g_platform) g_time = &g_platform->time;

    /* A-16 / ADR-034: the full nfsu2 port is the PRODUCT path in every
     * variant.  There is deliberately NO opt-out trigger.  The old
     * wave2_v2_off file that can still sit in <WC3_DATA_DIR> from the rev-6
     * rollback is intentionally IGNORED; legacy WINMM is now opt-IN via
     * wave2_legacy_on.  wc3_legacy_hle_add() appends rows at the end of the
     * registry, so the dsound rows installed in dispatch_boot.c never shift
     * the frozen fast-bind slots of the 11 as-built WINMM rows. */
    int enable = WC3_AUDIO_V2_ENABLED ? 1 : 0;
    if (w2_trigger("wave2_legacy_on")) enable = 0;
#if WC3_DEBUG
    const char *env = getenv("WC3_AUDIO_V2");
    if (env) enable = env[0] != '0';
#endif
    if (!enable) {
        atomic_store(&g_enabled, 0);
        /* O-91: fast binding is product code in winmm_fast_bind.c and runs in
         * every variant.  v2 does not duplicate it here. */
#if WC3_DEBUG && WC3_DEBUG_AUDIO
        fprintf(stderr, "[wave2] disabled: as-built WINMM opt-in "
                        "(wave2_legacy_on)\n");
#endif
        installed = 1;
        return 0;
    }

    if (wc3_legacy_hle_chain("USER32.dll", "GetMessageA", h_w2_pump_get,
                             &w2_prev_get_message) != 0 ||
        wc3_legacy_hle_chain("USER32.dll", "PeekMessageA", h_w2_pump_peek,
                             &w2_prev_peek_message) != 0 ||
        wc3_legacy_hle_override("USER32.dll", "SetTimer",
                                h_w2_set_timer) != 0 ||
        wc3_legacy_hle_override("USER32.dll", "KillTimer",
                                h_w2_kill_timer) != 0) {
        W2LOG("USER32 rows missing");
        return -1;
    }
    int n = 0;
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("WINMM.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0) {
            W2LOG("missing WINMM row %s", g_overrides[i].name);
            return -1;
        }
        n++;
    }
    atomic_store(&g_enabled, 1);
    installed = 1;
    W2LOG("installed %d WINMM rows + 2 USER32 pump chains + 2 timer "
          "overrides: rev 6 S1 platform sink / S2 guest callback thread / "
          "S3 wall-clock timer service", n);
    return n + 4;
}
