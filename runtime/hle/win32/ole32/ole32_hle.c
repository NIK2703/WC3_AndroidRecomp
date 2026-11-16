/*
 * ole32_hle.c - DirectShow graph facade for Warcraft III 1.27 cinematics.
 *
 * The shipped game imports exactly three ole32 entry points and drives a
 * deliberately small DirectShow surface:
 *
 *   CoCreateInstance(CLSID_FilterGraph, IID_IGraphBuilder)
 *   IGraphBuilder::RenderFile
 *   IMediaEventEx::SetNotifyWindow / GetEvent / FreeEventParams
 *   IVideoWindow::put_WindowStyle / put_Owner / put_MessageDrain /
 *                  SetWindowPosition
 *   IMediaControl::Run / Stop
 *
 * The guest objects and their vtables live in guest memory and point into the
 * frozen HLE thunk window, following the same pattern as the DirectSound HLE.
 * No game code is replaced: only the external COM implementation is supplied.
 *
 * The actual playback is Wc3Platform.video (the Java MoviePlayer: Media3 owns
 * AVI audio and the clock, FFmpeg decodes video).  This module never touches
 * video frames, audio, or a Surface.  Completion is reported exactly as
 * DirectShow reports it: the registered window receives WMU_CALL (0x8001), the
 * game's WndProc calls GetEvent, and the game then posts its own WM_LBUTTONUP
 * (0x202) that leaves the cinematic.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "platform.h"
#include "wc3_debug.h"
#include "ole32_hle.h"

#define DS_S_OK                 0u
#define DS_E_NOTIMPL            0x80004001u
#define DS_E_NOINTERFACE        0x80004002u
#define DS_E_POINTER            0x80004003u
#define DS_E_FAIL               0x80004005u
#define DS_E_OUTOFMEMORY        0x8007000Eu
#define DS_REGDB_E_CLASSNOTREG  0x80040154u
#define DS_CLASS_E_NOAGGREGATION 0x80040110u
#define DS_VFW_E_NO_EVENTS      0x8004020Au

#define EC_COMPLETE             1u
#define EC_USERABORT            2u
#define EC_ERRORABORT           3u

#define WMU_CALL                0x8001u

enum {
    DS_GRAPH = 0,
    DS_EVENT,
    DS_CONTROL,
    DS_VIDEO,
    DS_KINDS
};

/* 48 slots covers IVideoWindow (SetWindowPosition is slot 39) with room to
 * spare.  Slots the game never calls stay zero; a call into one is a guest
 * fault rather than a silent success, which is the explicit-failure rule. */
#define DS_VT_SLOTS             48u
#define DS_OBJ_SIZE             16u
#define DS_MAGIC                0x44535631u /* "DSV1" */

#define DS_VT_QI                0u
#define DS_VT_ADDREF            1u
#define DS_VT_RELEASE           2u

/* IGraphBuilder, IFilterGraph */
#define DS_GRAPH_RENDERFILE     13u

/* IMediaEvent / IMediaEventEx */
#define DS_EVENT_GETEVENT       8u
#define DS_EVENT_FREEEVENT      12u
#define DS_EVENT_SETNOTIFY      13u

/* IMediaControl */
#define DS_CONTROL_RUN          7u
#define DS_CONTROL_STOP         9u

/* IVideoWindow */
#define DS_VIDEO_WINDOWSTYLE    9u
#define DS_VIDEO_OWNER          29u
#define DS_VIDEO_MESSAGEDRAIN   31u
#define DS_VIDEO_SETPOSITION    39u

static uint32_t g_vtbl[DS_KINDS][DS_VT_SLOTS];
static uint32_t g_obj[DS_KINDS];
static uint32_t g_common_thunk[3];
static uint32_t g_thunk[DS_KINDS][DS_VT_SLOTS];
static _Atomic int g_installed;
static _Atomic int g_running;
static _Atomic int g_cancelled;
static _Atomic int g_event_posted;
static _Atomic uint32_t g_pending_event;
static _Atomic uint32_t g_notify_hwnd;
static _Atomic uint32_t g_notify_msg;
static uint32_t g_refs;
static char g_path[2048];

static int wide_to_utf8(uint32_t src, char *dst, size_t cap);

/* GUIDs are compared as four little-endian dwords, the representation the
 * guest actually stores in .rdata.  Data1/Data2/Data3 are not byte-swapped. */
static const uint32_t iid_iunknown[4] = {
    0x00000000u, 0x00000000u, 0x000000C0u, 0x46000000u
};
static const uint32_t iid_igraphbuilder[4] = {
    0x56A868A9u, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t iid_ifiltergraph[4] = {
    0x56A8689Fu, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t iid_imediaevent[4] = {
    0x56A868B6u, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t iid_imediaeventex[4] = {
    0x56A868C0u, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t iid_imediacontrol[4] = {
    0x56A868B1u, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t iid_ivideowindow[4] = {
    0x56A868B4u, 0x11CE0AD4u, 0x20003AB0u, 0x70A70BAFu
};
static const uint32_t clsid_filtergraph[4] = {
    0xE436EBB3u, 0x11CE524Fu, 0x2000539Fu, 0x70A70BAFu
};

static int guid_is(uint32_t va, const uint32_t want[4])
{
    if (!va || !wc3_legacy_guest_ok(va, 16u)) return 0;
    for (unsigned i = 0; i < 4u; i++)
        if (wc3_legacy_rd32g(va + i * 4u) != want[i]) return 0;
    return 1;
}

static int object_kind(uint32_t self)
{
    if (!self || !wc3_legacy_guest_ok(self, DS_OBJ_SIZE)) return -1;
    if (wc3_legacy_rd32g(self + 4u) != DS_MAGIC) return -1;
    uint32_t kind = wc3_legacy_rd32g(self + 8u);
    return kind < DS_KINDS ? (int)kind : -1;
}

static void ds_stop_video(void)
{
    if (!atomic_exchange_explicit(&g_running, 0, memory_order_acq_rel))
        return;
    const Wc3Platform *p = wc3_platform();
    if (p && p->video.stop) p->video.stop(p->video.ctx);
}

static void ds_post_pending(void)
{
    if (atomic_load_explicit(&g_event_posted, memory_order_acquire))
        return;
    uint32_t code = atomic_load_explicit(&g_pending_event,
                                         memory_order_acquire);
    uint32_t hwnd = atomic_load_explicit(&g_notify_hwnd,
                                         memory_order_acquire);
    uint32_t msg = atomic_load_explicit(&g_notify_msg,
                                        memory_order_acquire);
    if (!code || !hwnd || !msg) return;

    atomic_store_explicit(&g_event_posted, 1, memory_order_release);
    wc3_legacy_post_message(hwnd, msg, 0u, 0u);
}

static void ds_finish(int status)
{
    if (atomic_load_explicit(&g_cancelled, memory_order_acquire)) return;

    uint32_t code = status == 0 ? EC_COMPLETE
                  : status == 2 ? EC_USERABORT
                                : EC_ERRORABORT;
    atomic_store_explicit(&g_pending_event, code, memory_order_release);
    atomic_store_explicit(&g_event_posted, 0, memory_order_release);
    ds_post_pending();

#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] movie finished status=%d event=%u hwnd=%08X\n",
            status, code,
            atomic_load_explicit(&g_notify_hwnd, memory_order_acquire));
#endif
}

static void ds_reset_state(void)
{
    atomic_store(&g_pending_event, 0u);
    atomic_store(&g_event_posted, 0);
    atomic_store(&g_notify_hwnd, 0u);
    atomic_store(&g_notify_msg, 0u);
    atomic_store(&g_running, 0);
    atomic_store(&g_cancelled, 0);
    g_refs = 0;
    g_path[0] = '\0';
}

static int ds_create_objects(void)
{
    if (g_obj[DS_GRAPH]) return 0;

    uint32_t vt[DS_KINDS] = { 0 };
    uint32_t ob[DS_KINDS] = { 0 };

    for (unsigned kind = 0; kind < DS_KINDS; kind++) {
        vt[kind] = wc3_legacy_guest_alloc0(DS_VT_SLOTS * 4u);
        ob[kind] = wc3_legacy_guest_alloc0(DS_OBJ_SIZE);
        if (!vt[kind] || !ob[kind]) goto fail;

        for (unsigned slot = 0; slot < DS_VT_SLOTS; slot++)
            wc3_legacy_wr32g(vt[kind] + slot * 4u, 0u);
        wc3_legacy_wr32g(vt[kind] + DS_VT_QI * 4u, g_common_thunk[0]);
        wc3_legacy_wr32g(vt[kind] + DS_VT_ADDREF * 4u, g_common_thunk[1]);
        wc3_legacy_wr32g(vt[kind] + DS_VT_RELEASE * 4u, g_common_thunk[2]);

        switch (kind) {
        case DS_GRAPH:
            wc3_legacy_wr32g(vt[kind] + DS_GRAPH_RENDERFILE * 4u,
                             g_thunk[DS_GRAPH][DS_GRAPH_RENDERFILE]);
            break;
        case DS_EVENT:
            wc3_legacy_wr32g(vt[kind] + DS_EVENT_GETEVENT * 4u,
                             g_thunk[DS_EVENT][DS_EVENT_GETEVENT]);
            wc3_legacy_wr32g(vt[kind] + DS_EVENT_FREEEVENT * 4u,
                             g_thunk[DS_EVENT][DS_EVENT_FREEEVENT]);
            wc3_legacy_wr32g(vt[kind] + DS_EVENT_SETNOTIFY * 4u,
                             g_thunk[DS_EVENT][DS_EVENT_SETNOTIFY]);
            break;
        case DS_CONTROL:
            wc3_legacy_wr32g(vt[kind] + DS_CONTROL_RUN * 4u,
                             g_thunk[DS_CONTROL][DS_CONTROL_RUN]);
            wc3_legacy_wr32g(vt[kind] + DS_CONTROL_STOP * 4u,
                             g_thunk[DS_CONTROL][DS_CONTROL_STOP]);
            break;
        case DS_VIDEO:
            wc3_legacy_wr32g(vt[kind] + DS_VIDEO_WINDOWSTYLE * 4u,
                             g_thunk[DS_VIDEO][DS_VIDEO_WINDOWSTYLE]);
            wc3_legacy_wr32g(vt[kind] + DS_VIDEO_OWNER * 4u,
                             g_thunk[DS_VIDEO][DS_VIDEO_OWNER]);
            wc3_legacy_wr32g(vt[kind] + DS_VIDEO_MESSAGEDRAIN * 4u,
                             g_thunk[DS_VIDEO][DS_VIDEO_MESSAGEDRAIN]);
            wc3_legacy_wr32g(vt[kind] + DS_VIDEO_SETPOSITION * 4u,
                             g_thunk[DS_VIDEO][DS_VIDEO_SETPOSITION]);
            break;
        default:
            break;
        }

        wc3_legacy_wr32g(ob[kind], vt[kind]);
        wc3_legacy_wr32g(ob[kind] + 4u, DS_MAGIC);
        wc3_legacy_wr32g(ob[kind] + 8u, kind);
    }

    for (unsigned kind = 0; kind < DS_KINDS; kind++) {
        g_vtbl[kind][0] = vt[kind];
        g_obj[kind] = ob[kind];
    }
    return 0;

fail:
    for (unsigned kind = 0; kind < DS_KINDS; kind++) {
        if (vt[kind]) wc3_legacy_guest_free(vt[kind]);
        if (ob[kind]) wc3_legacy_guest_free(ob[kind]);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* COM methods                                                        */
/* ------------------------------------------------------------------ */

static void h_qi(CPU *c)
{
    uint32_t self = wc3_legacy_arg32(c, 0);
    uint32_t riid = wc3_legacy_arg32(c, 1);
    uint32_t ppv = wc3_legacy_arg32(c, 2);
    if (!ppv || !wc3_legacy_guest_ok(ppv, 4u)) {
        c->eax = DS_E_POINTER;
        return;
    }
    wc3_legacy_wr32g(ppv, 0u);

    int kind = object_kind(self);
    if (kind < 0 || !riid || !wc3_legacy_guest_ok(riid, 16u)) {
        c->eax = DS_E_POINTER;
        return;
    }

    int target = -1;
    if (guid_is(riid, iid_iunknown)) {
        target = kind;
    } else if (kind == DS_GRAPH) {
        if (guid_is(riid, iid_igraphbuilder) ||
            guid_is(riid, iid_ifiltergraph))
            target = DS_GRAPH;
        else if (guid_is(riid, iid_imediaeventex) ||
                 guid_is(riid, iid_imediaevent))
            target = DS_EVENT;
        else if (guid_is(riid, iid_imediacontrol))
            target = DS_CONTROL;
        else if (guid_is(riid, iid_ivideowindow))
            target = DS_VIDEO;
    } else if (kind == DS_EVENT) {
        if (guid_is(riid, iid_imediaeventex) ||
            guid_is(riid, iid_imediaevent))
            target = DS_EVENT;
    } else if (kind == DS_CONTROL) {
        if (guid_is(riid, iid_imediacontrol))
            target = DS_CONTROL;
    } else if (kind == DS_VIDEO) {
        if (guid_is(riid, iid_ivideowindow))
            target = DS_VIDEO;
    }

    if (target < 0) {
        c->eax = DS_E_NOINTERFACE;
        return;
    }
    if (g_refs != 0xFFFFFFFFu) g_refs++;
    wc3_legacy_wr32g(ppv, g_obj[target]);
    c->eax = DS_S_OK;
}

static void h_addref(CPU *c)
{
    if (g_refs != 0xFFFFFFFFu) g_refs++;
    c->eax = g_refs;
}

static void h_release(CPU *c)
{
    if (g_refs) g_refs--;
    c->eax = g_refs;
}

static void h_render_file(CPU *c)
{
    uint32_t wide = wc3_legacy_arg32(c, 1);
    char guest[2048];
    char host[2048];

#if WC3_DEBUG && WC3_DEBUG_HLE
    /* A missing terminator shows up here as a path that runs into the next
     * heap object.  Print the actual length so the next run distinguishes a
     * conversion bug from a stale path buffer without another rebuild. */
    if (wide) {
        unsigned units = 0;
        while (units < 1024u &&
               wc3_legacy_guest_ok(wide + units * 2u, 2u) &&
               wc3_legacy_rd16g(wide + units * 2u))
            units++;
        fprintf(stderr, "[ole32] RenderFile wide=%08X units=%u%s\n",
                wide, units,
                units == 1024u ? " (no terminator in 1024)" : "");
    }
#endif

    g_path[0] = '\0';
    if (wide_to_utf8(wide, guest, sizeof guest) == 0) {
        if (wc3_legacy_resolve_path(guest, host, sizeof host) == 0)
            snprintf(g_path, sizeof g_path, "%s", host);
        else
            snprintf(g_path, sizeof g_path, "%s", guest);
    }

#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] RenderFile %s\n", g_path[0] ? g_path : "(empty)");
#endif
    c->eax = DS_S_OK;
}

static void h_run(CPU *c)
{
    const Wc3Platform *p = wc3_platform();
    int ok = -1;

    atomic_store(&g_cancelled, 0);
    atomic_store(&g_running, 1);
    if (g_path[0] && p && p->video.play)
        ok = p->video.play(p->video.ctx, g_path);

    if (ok != 0) {
        atomic_store(&g_running, 0);
        ds_finish(1);
    }
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] Run %s -> %d\n",
            g_path[0] ? g_path : "(empty)", ok);
#endif
    c->eax = DS_S_OK;
}

static void h_stop(CPU *c)
{
    atomic_store(&g_cancelled, 1);
    atomic_store(&g_pending_event, 0u);
    atomic_store(&g_event_posted, 0);
    ds_stop_video();
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] Stop\n");
#endif
    c->eax = DS_S_OK;
}

static void h_set_notify_window(CPU *c)
{
    uint32_t hwnd = wc3_legacy_arg32(c, 1);
    uint32_t msg = wc3_legacy_arg32(c, 2);
    atomic_store(&g_notify_hwnd, hwnd);
    atomic_store(&g_notify_msg, msg);
    if (hwnd && msg) ds_post_pending();
    c->eax = DS_S_OK;
}

static void h_get_event(CPU *c)
{
    uint32_t code_ptr = wc3_legacy_arg32(c, 1);
    uint32_t p1_ptr = wc3_legacy_arg32(c, 2);
    uint32_t p2_ptr = wc3_legacy_arg32(c, 3);
    uint32_t code = atomic_exchange(&g_pending_event, 0u);

    if (!code) {
        c->eax = DS_VFW_E_NO_EVENTS;
        return;
    }
    atomic_store(&g_event_posted, 0);

    if (code_ptr && wc3_legacy_guest_ok(code_ptr, 4u))
        wc3_legacy_wr32g(code_ptr, code);
    if (p1_ptr && wc3_legacy_guest_ok(p1_ptr, 4u))
        wc3_legacy_wr32g(p1_ptr, 0u);
    if (p2_ptr && wc3_legacy_guest_ok(p2_ptr, 4u))
        wc3_legacy_wr32g(p2_ptr, 0u);
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] GetEvent -> %u\n", code);
#endif
    c->eax = DS_S_OK;
}

static void h_free_event_params(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

static void h_put_window_style(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

static void h_put_owner(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

static void h_put_message_drain(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

static void h_set_window_position(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

/* ------------------------------------------------------------------ */
/* ole32 imports                                                      */
/* ------------------------------------------------------------------ */

static void h_co_initialize(CPU *c)
{
    (void)c;
    c->eax = DS_S_OK;
}

static void h_co_uninitialize(CPU *c)
{
    ds_stop_video();
    ds_reset_state();
    c->eax = DS_S_OK;
}

static void h_co_create_instance(CPU *c)
{
    uint32_t rclsid = wc3_legacy_arg32(c, 0);
    uint32_t outer = wc3_legacy_arg32(c, 1);
    uint32_t ppv = wc3_legacy_arg32(c, 4);
    uint32_t riid = wc3_legacy_arg32(c, 3);

    if (!ppv || !wc3_legacy_guest_ok(ppv, 4u)) {
        c->eax = DS_E_POINTER;
        return;
    }
    wc3_legacy_wr32g(ppv, 0u);

    if (outer) {
        c->eax = DS_CLASS_E_NOAGGREGATION;
        return;
    }
    if (!guid_is(rclsid, clsid_filtergraph)) {
        c->eax = DS_REGDB_E_CLASSNOTREG;
        return;
    }
    if (!guid_is(riid, iid_igraphbuilder) &&
        !guid_is(riid, iid_iunknown)) {
        c->eax = DS_E_NOINTERFACE;
        return;
    }

    ds_stop_video();
    ds_reset_state();
    if (ds_create_objects() != 0) {
        c->eax = DS_E_OUTOFMEMORY;
        return;
    }
    g_refs = 1;
    wc3_legacy_wr32g(ppv, g_obj[DS_GRAPH]);
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] CoCreateInstance -> graph %08X\n",
            g_obj[DS_GRAPH]);
#endif
    c->eax = DS_S_OK;
}

/* Convert the UTF-16 file name the guest passes to RenderFile into UTF-8.
 * The path is short; the bound keeps a malformed guest string from walking
 * the whole address space. */
static int wide_to_utf8(uint32_t src, char *dst, size_t cap)
{
    size_t o = 0;
    if (!src) return -1;

    for (unsigned i = 0; i < 4096u; i++) {
        if (!wc3_legacy_guest_ok(src + i * 2u, 2u)) return -1;
        uint32_t u = wc3_legacy_rd16g(src + i * 2u);
        if (!u) break;

        uint32_t cp = u;
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (!wc3_legacy_guest_ok(src + (i + 1u) * 2u, 2u)) return -1;
            uint32_t low = wc3_legacy_rd16g(src + (i + 1u) * 2u);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + ((u - 0xD800u) << 10) + (low - 0xDC00u);
                i++;
            } else {
                cp = 0xFFFDu;
            }
        } else if (u >= 0xDC00u && u <= 0xDFFFu) {
            cp = 0xFFFDu;
        }

        unsigned char enc[4];
        unsigned n;
        if (cp < 0x80u) {
            enc[0] = (unsigned char)cp;
            n = 1;
        } else if (cp < 0x800u) {
            enc[0] = (unsigned char)(0xC0u | (cp >> 6));
            enc[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
            n = 2;
        } else if (cp < 0x10000u) {
            enc[0] = (unsigned char)(0xE0u | (cp >> 12));
            enc[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
            enc[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
            n = 3;
        } else {
            enc[0] = (unsigned char)(0xF0u | (cp >> 18));
            enc[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
            enc[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
            enc[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
            n = 4;
        }
        if (o + n >= cap) break;
        memcpy(dst + o, enc, n);
        o += n;
    }
    dst[o] = '\0';
    return o ? 0 : -1;
}

void wc3_hle_ole32_video_finished(int status)
{
    ds_finish(status);
}

/* ------------------------------------------------------------------ */
/* installation                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int argc;
    void (*fn)(CPU *c);
    unsigned kind;
    unsigned slot;
} DsRow;

static const DsRow g_rows[] = {
    { "DShow_RenderFile",       3, h_render_file,
      DS_GRAPH,   DS_GRAPH_RENDERFILE },
    { "DShow_GetEvent",         5, h_get_event,
      DS_EVENT,   DS_EVENT_GETEVENT },
    { "DShow_FreeEventParams",  4, h_free_event_params,
      DS_EVENT,   DS_EVENT_FREEEVENT },
    { "DShow_SetNotifyWindow",  4, h_set_notify_window,
      DS_EVENT,   DS_EVENT_SETNOTIFY },
    { "DShow_Run",              1, h_run,
      DS_CONTROL, DS_CONTROL_RUN },
    { "DShow_Stop",             1, h_stop,
      DS_CONTROL, DS_CONTROL_STOP },
    { "DShow_put_WindowStyle",  2, h_put_window_style,
      DS_VIDEO,   DS_VIDEO_WINDOWSTYLE },
    { "DShow_put_Owner",        2, h_put_owner,
      DS_VIDEO,   DS_VIDEO_OWNER },
    { "DShow_put_MessageDrain", 2, h_put_message_drain,
      DS_VIDEO,   DS_VIDEO_MESSAGEDRAIN },
    { "DShow_SetWindowPosition", 5, h_set_window_position,
      DS_VIDEO,   DS_VIDEO_SETPOSITION },
};

int wc3_hle_ole32_install(void)
{
    if (atomic_exchange_explicit(&g_installed, 1, memory_order_acq_rel))
        return 0;

    if (wc3_legacy_hle_add("ole32.dll", "CoInitialize", 1, 0,
                           h_co_initialize) != 0)
        return -1;
    if (wc3_legacy_hle_add("ole32.dll", "CoUninitialize", 0, 0,
                           h_co_uninitialize) != 0)
        return -1;
    if (wc3_legacy_hle_add("ole32.dll", "CoCreateInstance", 5, 0,
                           h_co_create_instance) != 0)
        return -1;

    if (wc3_legacy_hle_add("ole32.dll", "DShow_QI", 3, 0, h_qi) != 0)
        return -1;
    if (wc3_legacy_hle_add("ole32.dll", "DShow_AddRef", 1, 0, h_addref) != 0)
        return -1;
    if (wc3_legacy_hle_add("ole32.dll", "DShow_Release", 1, 0,
                           h_release) != 0)
        return -1;
    g_common_thunk[0] = wc3_legacy_hle_thunk_of("ole32.dll", "DShow_QI");
    g_common_thunk[1] = wc3_legacy_hle_thunk_of("ole32.dll", "DShow_AddRef");
    g_common_thunk[2] = wc3_legacy_hle_thunk_of("ole32.dll", "DShow_Release");
    if (!g_common_thunk[0] || !g_common_thunk[1] || !g_common_thunk[2])
        return -1;

    for (unsigned i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        const DsRow *r = &g_rows[i];
        if (wc3_legacy_hle_add("ole32.dll", r->name, r->argc, 0, r->fn) != 0)
            return -1;
        g_thunk[r->kind][r->slot] =
            wc3_legacy_hle_thunk_of("ole32.dll", r->name);
        if (!g_thunk[r->kind][r->slot]) return -1;
    }

#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[ole32] DirectShow facade installed\n");
#endif
    return 0;
}

