/*
 * dsound_hle.c - the guest-visible half of the DirectSound audio architecture,
 * wired into the WC3 HLE seam.
 *
 * The guest has no WAVEHDR pool and no WOM_DONE.  It creates one looping
 * DirectSound buffer, Lock()s a region, writes PCM into guest memory, Unlock()s
 * only that region into a host-owned shadow, and polls GetCurrentPosition.
 * PcmMixer::render() pulls and resamples in the AAudio callback.
 *
 * Here the COM vtable lives in guest memory and points into the project's
 * frozen thunk window (wc3_legacy_hle_thunk_of); the host-owned shadow is a
 * Wc3Voice from runtime/platform/audio_mixer.h.  No OS headers, no host locks,
 * no allocation in the real-time path, no edits to runtime/hle/android or to the
 * generated corpus.
 *
 * IMPORTANT (G-14/G-15): the rows are APPENDED at the end of the registry at
 * boot (ADR-034), so the frozen fast-bind slot indices of the shipped corpus
 * do not move.  A corpus that actually imports dsound.dll needs a re-lift;
 * Mss32 reaches the synthetic module through LoadLibraryA/GetProcAddress
 * instead.  Installation is unconditional.  WINMM is a no-driver guard and
 * never creates a stream.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "direct_call.h"
#include "hle_bridge.h"
#include "platform.h"
#include "wc3_debug.h"
#include "dsound_hle.h"

#define DS_OK                   0u
#define E_NOTIMPL               0x80004001u
#define E_NOINTERFACE           0x80004002u
#define E_POINTER               0x80004003u
#define E_FAIL                  0x80004005u
#define DSERR_GENERIC           0x80004005u
#define DSERR_INVALIDPARAM      0x80070057u
#define DSERR_OUTOFMEMORY       0x8007000Eu
#define DSERR_BADFORMAT         0x88780064u
#define DSERR_UNSUPPORTED       0x8878001Eu
#define DSERR_NO_VOICE          0x8878000Au
#define DSERR_NODRIVER          0x88780078u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define CLASS_E_NOAGGREGATION   0x80040110u

#define DSBCAPS_PRIMARYBUFFER   0x00000001u
#define DSBCAPS_CTRL3D          0x00000010u
#define DSBLOCK_FROMWRITECURSOR 0x00000001u
#define DSBLOCK_ENTIREBUFFER    0x00000002u
#define DSBPLAY_LOOPING         0x00000001u
#define DSBSTATUS_PLAYING       0x00000001u
#define DSBSTATUS_LOOPING       0x00000004u

#define DS_METHODS              24u
#define DS_OBJ_MAX              64u
#define DS_HMOD                 0x000A3000u
#define DS_DISPATCH_RETURN      0x7EFFFFF1u

static void dsound_dispatch(CPU *c, unsigned kind, unsigned method);

/* One named HLE row per COM slot: the registry has no per-thunk payload at
 * this seam, so a tiny wrapper carries (kind, method) into the dispatcher. */
#define DS_ROW(k, m, a) \
    static void ds_row_##k##_##m(CPU *c) { dsound_dispatch(c, k, m); }

DS_ROW(1, 0, 3) DS_ROW(1, 1, 1) DS_ROW(1, 2, 1) DS_ROW(1, 3, 4)
DS_ROW(1, 4, 2) DS_ROW(1, 5, 3) DS_ROW(1, 6, 3) DS_ROW(1, 7, 1)
DS_ROW(1, 8, 2) DS_ROW(1, 9, 2) DS_ROW(1, 10, 2)
DS_ROW(2, 0, 3) DS_ROW(2, 1, 1) DS_ROW(2, 2, 1) DS_ROW(2, 3, 2)
DS_ROW(2, 4, 3) DS_ROW(2, 5, 4) DS_ROW(2, 6, 2) DS_ROW(2, 7, 2)
DS_ROW(2, 8, 2) DS_ROW(2, 9, 2) DS_ROW(2, 10, 3) DS_ROW(2, 11, 8)
DS_ROW(2, 12, 4) DS_ROW(2, 13, 2) DS_ROW(2, 14, 2) DS_ROW(2, 15, 2)
DS_ROW(2, 16, 2) DS_ROW(2, 17, 2) DS_ROW(2, 18, 1) DS_ROW(2, 19, 5)
DS_ROW(2, 20, 1) DS_ROW(2, 21, 3)

typedef struct {
    unsigned kind, method, argc;
    void (*fn)(CPU *c);
} DsRow;

static const DsRow g_rows[] = {
    { 1, 0, 3, ds_row_1_0 }, { 1, 1, 1, ds_row_1_1 },
    { 1, 2, 1, ds_row_1_2 }, { 1, 3, 4, ds_row_1_3 },
    { 1, 4, 2, ds_row_1_4 }, { 1, 5, 3, ds_row_1_5 },
    { 1, 6, 3, ds_row_1_6 }, { 1, 7, 1, ds_row_1_7 },
    { 1, 8, 2, ds_row_1_8 }, { 1, 9, 2, ds_row_1_9 },
    { 1, 10, 2, ds_row_1_10 },
    { 2, 0, 3, ds_row_2_0 }, { 2, 1, 1, ds_row_2_1 },
    { 2, 2, 1, ds_row_2_2 }, { 2, 3, 2, ds_row_2_3 },
    { 2, 4, 3, ds_row_2_4 }, { 2, 5, 4, ds_row_2_5 },
    { 2, 6, 2, ds_row_2_6 }, { 2, 7, 2, ds_row_2_7 },
    { 2, 8, 2, ds_row_2_8 }, { 2, 9, 2, ds_row_2_9 },
    { 2, 10, 3, ds_row_2_10 }, { 2, 11, 8, ds_row_2_11 },
    { 2, 12, 4, ds_row_2_12 }, { 2, 13, 2, ds_row_2_13 },
    { 2, 14, 2, ds_row_2_14 }, { 2, 15, 2, ds_row_2_15 },
    { 2, 16, 2, ds_row_2_16 }, { 2, 17, 2, ds_row_2_17 },
    { 2, 18, 1, ds_row_2_18 }, { 2, 19, 5, ds_row_2_19 },
    { 2, 20, 1, ds_row_2_20 }, { 2, 21, 3, ds_row_2_21 },
};

static uint32_t g_thunk[3][DS_METHODS];
static _Atomic int g_installed;

typedef struct {
    uint32_t self, table, guest_samples;
    uintptr_t voice;
    uintptr_t stream;
    uint32_t bytes, bpf, rate, channels, bits, flags;
    uint32_t refs, kind;
    uint32_t parent_self, children;
    uint32_t lock_offset, lock_bytes;
    int locked, playing, loop, volume, pan;
    int owns_stream, zombie;
    uint32_t frequency;
} DsObject;

static DsObject g_obj[DS_OBJ_MAX];

static const Wc3AudioApi *ds_audio(void);

static DsObject *ds_find(uint32_t self)
{
    if (!self) return NULL;
    for (unsigned i = 0; i < DS_OBJ_MAX; i++)
        if (g_obj[i].self == self) return &g_obj[i];
    return NULL;
}

static DsObject *ds_alloc(uint32_t kind)
{
    for (unsigned i = 0; i < DS_OBJ_MAX; i++) {
        DsObject *o = &g_obj[i];
        if (o->self) continue;
        memset(o, 0, sizeof *o);
        o->kind = kind;
        o->refs = 1u;
        o->volume = 0;
        o->pan = 0;
        o->table = wc3_legacy_guest_alloc0(DS_METHODS * 4u);
        o->self = wc3_legacy_guest_alloc0(8u);
        if (!o->table || !o->self) {
            if (o->table) wc3_legacy_guest_free(o->table);
            if (o->self) wc3_legacy_guest_free(o->self);
            memset(o, 0, sizeof *o);
            return NULL;
        }
        wc3_legacy_wr32g(o->self, o->table);
        wc3_legacy_wr32g(o->self + 4u, kind);
        for (unsigned m = 0; m < DS_METHODS; m++)
            wc3_legacy_wr32g(o->table + m * 4u,
                             g_thunk[kind][m] ? g_thunk[kind][m] : 0u);
        return o;
    }
    return NULL;
}

static void ds_free(DsObject *o)
{
    if (!o || !o->self) return;

    /* A DirectSound buffer keeps its device alive even if the guest drops the
     * device reference first.  Defer the device close until the last child is
     * gone; otherwise closing the device would tear down the only AAudio
     * stream while a playing buffer still points at its voice. */
    if (o->kind == 1u && o->children && !o->zombie) {
        o->zombie = 1;
        return;
    }

    const Wc3AudioApi *a = ds_audio();
    uint32_t parent_self = o->parent_self;
    if (o->voice && a && a->voice_remove)
        (void)a->voice_remove(a->ctx, o->voice);
    if (o->kind == 1u && o->owns_stream && o->stream && a && a->close)
        (void)a->close(a->ctx, o->stream);
    if (o->guest_samples) wc3_legacy_guest_free(o->guest_samples);
    if (o->table) wc3_legacy_guest_free(o->table);
    if (o->self) wc3_legacy_guest_free(o->self);
    memset(o, 0, sizeof *o);

    if (parent_self) {
        DsObject *p = ds_find(parent_self);
        if (p) {
            if (p->children) p->children--;
            if (p->zombie && !p->children) ds_free(p);
        }
    }
}

/* IID_IDirectSound and IID_IDirectSoundBuffer, from guest_audio.inc.
 * The reference stores them as 16 raw bytes; these are the same bytes read as
 * four little-endian dwords:
 *   83 fa 9a 27 | 81 49 ce 11 | a5 21 00 20 | af 0b e5 60
 *   -> 0x279AFA83, 0x11CE4981, 0x200021A5, 0x60E50BAF
 * (not 0x11CE8149 / 0x0020A521: those swap the two halves of Data2/Data3). */
static int ds_guid_is(uint32_t va, unsigned kind)
{
    static const uint32_t dev[4] = {
        0x279AFA83u, 0x11CE4981u, 0x200021A5u, 0x60E50BAFu
    };
    static const uint32_t buf[4] = {
        0x279AFA85u, 0x11CE4981u, 0x200021A5u, 0x60E50BAFu
    };
    static const uint32_t unknown[4] = {
        0u, 0u, 0x000000C0u, 0x46000000u
    };
    const uint32_t *want = kind == 1u ? dev : buf;
    if (!va || !wc3_legacy_guest_ok(va, 16u)) return 0;
    int is_want = 1, is_unknown = 1;
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t got = wc3_legacy_rd32g(va + i * 4u);
        if (got != want[i]) is_want = 0;
        if (got != unknown[i]) is_unknown = 0;
    }
    return is_want || is_unknown;
}

static int ds_parse_format(uint32_t fmt, DsObject *o)
{
    if (!fmt || !wc3_legacy_guest_ok(fmt, 18u)) return 0;
    if (wc3_legacy_rd16g(fmt + 0u) != 1u) return 0;
    uint32_t ch = wc3_legacy_rd16g(fmt + 2u);
    uint32_t rate = wc3_legacy_rd32g(fmt + 4u);
    uint32_t avg = wc3_legacy_rd32g(fmt + 8u);
    uint32_t align = wc3_legacy_rd16g(fmt + 12u);
    uint32_t bits = wc3_legacy_rd16g(fmt + 14u);
    if (ch < 1u || ch > 2u || rate < 100u || rate > 200000u) return 0;
    if (bits != 8u && bits != 16u) return 0;
    uint32_t bpf = ch * (bits / 8u);
    if (align != bpf || avg != rate * bpf) return 0;
    o->channels = ch;
    o->rate = rate;
    o->bits = bits;
    o->bpf = bpf;
    o->frequency = rate;
    return 1;
}

static const Wc3AudioApi *ds_audio(void)
{
    const Wc3Platform *p = wc3_platform();
    return p ? &p->audio : NULL;
}

/* ------------------------------------------------------------------ */
/* synthetic DSOUND.DLL                                                */
/* ------------------------------------------------------------------ */

/* Mss32 imports WINMM statically, but its DirectSound driver is selected
 * through LoadLibraryA("DSOUND.DLL") + GetProcAddress.  The as-built loader
 * has no dsound row, so without this synthetic module the game can never take
 * the DirectSound path even though the COM handlers are installed. */
static uint32_t g_create_thunk;
static uint32_t g_enum_thunk;
static void (*previous_load_library)(CPU *c);
static void (*previous_get_proc_address)(CPU *c);
static void (*previous_get_module_handle)(CPU *c);
static void (*previous_get_module_file_name)(CPU *c);

static int ds_ascii_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned ca = (unsigned char)*a++;
        unsigned cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int ds_guest_cstr(uint32_t va, char *out, size_t cap)
{
    if (!va || !out || cap == 0u) return 0;
    for (size_t i = 0; i < cap; i++) {
        if (!wc3_legacy_guest_ok(va + (uint32_t)i, 1u)) return 0;
        char ch = (char)wc3_legacy_rd8g(va + (uint32_t)i);
        out[i] = ch;
        if (!ch) return 1;
    }
    out[cap - 1u] = '\0';
    return 0;
}

static void ds_write_guest_cstr(uint32_t va, const char *s, uint32_t cap)
{
    if (!va || !cap || !s) return;
    uint32_t i = 0u;
    for (; s[i] && i + 1u < cap; i++)
        wc3_legacy_wr8g(va + i, (uint8_t)s[i]);
    wc3_legacy_wr8g(va + i, 0u);
}

static const char *ds_module_name(uint32_t handle)
{
    return handle == DS_HMOD ? "dsound.dll" : NULL;
}

static uint32_t ds_module_from_name(const char *name)
{
    if (!name) return 0;
    const char *leaf = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    if (ds_ascii_ieq(leaf, "dsound.dll") || ds_ascii_ieq(leaf, "dsound"))
        return DS_HMOD;
    return 0;
}

static void ds_guest_call(CPU *c, uint32_t target, const uint32_t *args,
                          unsigned n)
{
    if (!c || !target || !args || !n) {
        if (c) c->eax = 0u;
        return;
    }
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
        wc3_legacy_wr32g(c->esp, DS_DISPATCH_RETURN);
        dispatch(c, target);
    } else {
        c->eax = 0u;
    }
    c->esp = esp;
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
}

static void h_dsound_enumerate(CPU *c)
{
    uint32_t callback = wc3_legacy_arg32(c, 0);
    uint32_t context = wc3_legacy_arg32(c, 1);
    if (!callback) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    uint32_t desc = wc3_legacy_guest_alloc0(32u);
    uint32_t module = wc3_legacy_guest_alloc0(16u);
    if (!desc || !module) {
        if (desc) wc3_legacy_guest_free(desc);
        if (module) wc3_legacy_guest_free(module);
        c->eax = DSERR_OUTOFMEMORY;
        return;
    }
    ds_write_guest_cstr(desc, "Primary Sound Driver", 32u);
    ds_write_guest_cstr(module, "dsound.dll", 16u);
    /* The default device is identified by a NULL GUID, exactly like Windows'
     * first DirectSoundEnumerateA callback.  Mss32 stores the result and only
     * needs the enumeration to return success. */
    uint32_t args[4] = { 0u, desc, module, context };
    ds_guest_call(c, callback, args, 4u);
    wc3_legacy_guest_free(desc);
    wc3_legacy_guest_free(module);
    c->eax = DS_OK;
}

static void h_load_library_a(CPU *c)
{
    uint32_t name_ptr = wc3_legacy_arg32(c, 0);
    char name[128];
    if (ds_guest_cstr(name_ptr, name, sizeof name)) {
        uint32_t module = ds_module_from_name(name);
        if (module) {
            c->eax = module;
            return;
        }
    }
    if (previous_load_library) previous_load_library(c);
    else {
        wc3_legacy_set_last_error(126u);
        c->eax = 0u;
    }
}

static void h_get_proc_address(CPU *c)
{
    uint32_t module = wc3_legacy_arg32(c, 0);
    uint32_t name_or_ordinal = wc3_legacy_arg32(c, 1);
    if (module != DS_HMOD) {
        if (previous_get_proc_address) previous_get_proc_address(c);
        else {
            wc3_legacy_set_last_error(127u);
            c->eax = 0u;
        }
        return;
    }

    uint32_t thunk = 0u;
    if (name_or_ordinal <= 0xFFFFu) {
        if (name_or_ordinal == 1u) thunk = g_create_thunk;
    } else {
        char name[128];
        if (ds_guest_cstr(name_or_ordinal, name, sizeof name)) {
            if (!strcmp(name, "DirectSoundCreate")) thunk = g_create_thunk;
            else if (!strcmp(name, "DirectSoundEnumerateA"))
                thunk = g_enum_thunk;
        }
    }
    if (!thunk) {
        wc3_legacy_set_last_error(127u);
        c->eax = 0u;
        return;
    }
    c->eax = thunk;
}

static void h_get_module_handle_a(CPU *c)
{
    uint32_t name_ptr = wc3_legacy_arg32(c, 0);
    char name[128];
    if (ds_guest_cstr(name_ptr, name, sizeof name)) {
        uint32_t module = ds_module_from_name(name);
        if (module) {
            c->eax = module;
            return;
        }
    }
    if (previous_get_module_handle) previous_get_module_handle(c);
    else c->eax = 0u;
}

static void h_get_module_file_name_a(CPU *c)
{
    uint32_t module = wc3_legacy_arg32(c, 0);
    uint32_t buffer = wc3_legacy_arg32(c, 1);
    uint32_t capacity = wc3_legacy_arg32(c, 2);
    const char *name = ds_module_name(module);
    if (!name) {
        if (previous_get_module_file_name) previous_get_module_file_name(c);
        else c->eax = 0u;
        return;
    }
    if (!buffer || !capacity) {
        c->eax = 0u;
        return;
    }
    size_t len = strlen(name);
    uint32_t copy = (uint32_t)len;
    if (copy >= capacity) copy = capacity - 1u;
    if (!wc3_legacy_guest_ok(buffer, copy + 1u)) {
        c->eax = 0u;
        return;
    }
    if (copy) memcpy((void *)(uintptr_t)buffer, name, copy);
    wc3_legacy_wr8g(buffer + copy, 0u);
    c->eax = copy;
}

static void ds_query_interface(CPU *c, DsObject *o)
{
    uint32_t riid = wc3_legacy_arg32(c, 1);
    uint32_t out = wc3_legacy_arg32(c, 2);
    if (!out || !wc3_legacy_guest_ok(out, 4u)) {
        c->eax = E_POINTER; return;
    }
    wc3_legacy_wr32g(out, 0u);
    if (!ds_guid_is(riid, o->kind)) { c->eax = E_NOINTERFACE; return; }
    wc3_legacy_wr32g(out, o->self);
    o->refs++;
    c->eax = DS_OK;
}

static void ds_release(CPU *c, DsObject *o)
{
    uint32_t refs = --o->refs;
    if (!refs) ds_free(o);
    c->eax = refs;
}

static void ds_get_caps(CPU *c, DsObject *o)
{
    uint32_t out = wc3_legacy_arg32(c, 1);
    if (o->kind == 1u) {
        /* IDirectSound::GetCaps uses DSCAPS (96 bytes).  Mss32 reads dwFlags
         * and dwPrimaryBuffers and treats bit 5 (DSCAPS_EMULDRIVER) as "no
         * write-primary"; keep it clear so the driver takes its native path. */
        if (!out || !wc3_legacy_guest_ok(out, 96u) ||
            wc3_legacy_rd32g(out) != 96u) {
            c->eax = DSERR_INVALIDPARAM;
            return;
        }
        for (uint32_t i = 4u; i < 96u; i += 4u)
            wc3_legacy_wr32g(out + i, 0u);
        /* 0xF1F: primary mono/stereo/8/16, continuous rate, secondary
         * mono/stereo and 8/16-bit. */
        wc3_legacy_wr32g(out + 4u, 0xF1Fu);
        wc3_legacy_wr32g(out + 8u, 100u);
        wc3_legacy_wr32g(out + 12u, 200000u);
        wc3_legacy_wr32g(out + 16u, 1u);
    } else {
        /* IDirectSoundBuffer::GetCaps uses DSBCAPS (20 bytes).  The Mss32
         * DSSCL_WRITEPRIMARY probe reads dwBufferBytes at +8 and then calls
         * Lock(0, dwBufferBytes); returning the device DSCAPS layout here
         * made that probe lock 100 bytes and fail. */
        if (!out || !wc3_legacy_guest_ok(out, 20u) ||
            wc3_legacy_rd32g(out) != 20u) {
            c->eax = DSERR_INVALIDPARAM;
            return;
        }
        uint32_t status = o->playing ? DSBSTATUS_PLAYING : 0u;
        if (o->playing && o->loop) status |= DSBSTATUS_LOOPING;
        wc3_legacy_wr32g(out + 4u, o->flags | status);
        wc3_legacy_wr32g(out + 8u, o->bytes);
        wc3_legacy_wr32g(out + 12u, 0u);
        wc3_legacy_wr32g(out + 16u, 0u);
    }
    c->eax = DS_OK;
}

static void ds_create_sound_buffer(CPU *c, DsObject *o)
{
    uint32_t desc = wc3_legacy_arg32(c, 1);
    uint32_t out = wc3_legacy_arg32(c, 2);
    if (!out || !wc3_legacy_guest_ok(out, 4u)) {
        c->eax = E_POINTER; return;
    }
    wc3_legacy_wr32g(out, 0u);
    if (!desc || !wc3_legacy_guest_ok(desc, 4u)) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    uint32_t size = wc3_legacy_rd32g(desc);
    if (size != 20u && size != 36u) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    /* DSBUFFERDESC is 20 bytes (legacy) or 36 bytes; only require the bytes
     * the declared size says exist.  A 20-byte descriptor at the end of a page
     * must not be rejected because the old check demanded 36. */
    if (!wc3_legacy_guest_ok(desc, size)) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    if (wc3_legacy_arg32(c, 3) != 0u) {
        /* DirectSound buffers do not support COM aggregation. */
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    uint32_t flags = wc3_legacy_rd32g(desc + 4u);

    DsObject *b = ds_alloc(2u);
    if (!b) { c->eax = DSERR_OUTOFMEMORY; return; }
    b->flags = flags;

    /* A primary buffer has no PCM region of its own: the device format is
     * set with SetFormat and GetCurrentPosition/GetStatus stay in device
     * coordinates.  The guest never creates one, but DirectSound applications
     * probe for it and some iterate it through the same vtable. */
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (flags & DSBCAPS_CTRL3D) {
            ds_free(b);
            c->eax = DSERR_UNSUPPORTED;
            return;
        }
        b->stream = o->stream;
        b->parent_self = o->self;
        b->bytes = 0u;
        b->rate = 44100u;
        b->channels = 2u;
        b->bits = 16u;
        b->bpf = 4u;
        b->frequency = b->rate;
        o->children++;
        wc3_legacy_wr32g(out, b->self);
        c->eax = DS_OK;
        return;
    }

    uint32_t bytes = wc3_legacy_rd32g(desc + 8u);
    uint32_t fmt = wc3_legacy_rd32g(desc + 16u);
    b->bytes = bytes;
    if (!b->bytes || b->bytes > (16u << 20) ||
        !ds_parse_format(fmt, b) || (bytes % b->bpf) != 0u) {
        ds_free(b);
        c->eax = DSERR_BADFORMAT;
        return;
    }
    /* The reference checks the format first, then rejects 3D buffers. */
    if (flags & DSBCAPS_CTRL3D) {
        ds_free(b);
        c->eax = DSERR_UNSUPPORTED;
        return;
    }

    const Wc3AudioApi *a = ds_audio();
    if (!a || !a->voice_add ||
        a->voice_add(a->ctx, o->stream, b->rate, b->channels, b->bits,
                     b->bytes, &b->voice) != 0) {
        ds_free(b);
        c->eax = DSERR_NO_VOICE;
        return;
    }
    b->guest_samples = wc3_legacy_guest_alloc0(b->bytes);
    if (!b->guest_samples) {
        ds_free(b);
        c->eax = DSERR_OUTOFMEMORY;
        return;
    }
    if (b->bits == 8u)
        memset((void *)(uintptr_t)b->guest_samples, 0x80, b->bytes);
    b->parent_self = o->self;
    b->stream = o->stream;
    o->children++;
    wc3_legacy_wr32g(out, b->self);
    c->eax = DS_OK;
}

static void ds_get_current_position(CPU *c, DsObject *o)
{
    const Wc3AudioApi *a = ds_audio();
    uint32_t play = 0u, write = 0u;
    uint64_t pf = 0u;
    if (!o->voice) {
        /* Primary buffer: no guest-owned PCM cursor. */
        play = 0u;
        write = 0u;
    } else if (!a || !a->voice_position ||
               a->voice_position(a->ctx, o->voice, &play, &write, &pf) != 0) {
        c->eax = DSERR_GENERIC;
        return;
    }
    uint32_t pout = wc3_legacy_arg32(c, 1);
    uint32_t wout = wc3_legacy_arg32(c, 2);
    if ((pout && !wc3_legacy_guest_ok(pout, 4u)) ||
        (wout && !wc3_legacy_guest_ok(wout, 4u))) {
        c->eax = E_POINTER;
        return;
    }
    if (pout) wc3_legacy_wr32g(pout, play);
    if (wout) wc3_legacy_wr32g(wout, write);
    c->eax = DS_OK;
}

static void ds_get_format(CPU *c, DsObject *o)
{
    uint32_t out = wc3_legacy_arg32(c, 1);
    uint32_t allocated = wc3_legacy_arg32(c, 2);
    uint32_t written = wc3_legacy_arg32(c, 3);
    if (!out || !wc3_legacy_guest_ok(out, 18u) || allocated < 18u ||
        (written && !wc3_legacy_guest_ok(written, 4u))) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    wc3_legacy_wr16g(out + 0u, 1u);                 /* WAVE_FORMAT_PCM */
    wc3_legacy_wr16g(out + 2u, (uint16_t)o->channels);
    wc3_legacy_wr32g(out + 4u, o->rate);
    wc3_legacy_wr32g(out + 8u, o->rate * o->bpf);
    wc3_legacy_wr16g(out + 12u, (uint16_t)o->bpf);
    wc3_legacy_wr16g(out + 14u, (uint16_t)o->bits);
    wc3_legacy_wr16g(out + 16u, 0u);
    if (written) wc3_legacy_wr32g(written, 18u);
    c->eax = DS_OK;
}

static void ds_set_current_position(CPU *c, DsObject *o)
{
    uint32_t pos = wc3_legacy_arg32(c, 1);
    if (!o->bytes || !o->bpf || pos >= o->bytes || (pos % o->bpf) != 0u) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    if (o->voice) {
        const Wc3AudioApi *a = ds_audio();
        uint32_t play = 0u, write = 0u;
        uint64_t pf = 0u;
        if (!a || !a->voice_position || !a->voice_seek ||
            a->voice_position(a->ctx, o->voice, &play, &write, &pf) != 0) {
            c->eax = DSERR_GENERIC;
            return;
        }
        uint64_t frames = o->bytes / o->bpf;
        if (!frames) frames = 1u;
        uint64_t lap = pf / frames;
        /* SetCurrentPosition() is an offset inside the current lap, not a
         * request to wait until the next lap.  A backward seek must restart
         * playback at once, exactly like the Windows play cursor. */
        uint64_t abs = lap * frames + pos / o->bpf;
        if (a->voice_seek(a->ctx, o->voice, abs) != 0) {
            c->eax = DSERR_GENERIC;
            return;
        }
    }
    c->eax = DS_OK;
}

static void ds_get_status(CPU *c, DsObject *o)
{
    uint32_t out = wc3_legacy_arg32(c, 1);
    if (!out || !wc3_legacy_guest_ok(out, 4u)) {
        c->eax = E_POINTER;
        return;
    }
    uint32_t playing = o->playing ? 1u : 0u;
    uint32_t loop = o->loop ? 1u : 0u;
    const Wc3AudioApi *a = ds_audio();
    if (o->voice && a && a->voice_state) {
        uint64_t pos = 0u;
        if (a->voice_state(a->ctx, o->voice, &playing, &loop, &pos) == 0) {
            o->playing = playing ? 1 : 0;
            o->loop = loop ? 1 : 0;
        }
    }
    wc3_legacy_wr32g(out, playing
        ? (DSBSTATUS_PLAYING | (loop ? DSBSTATUS_LOOPING : 0u)) : 0u);
    c->eax = DS_OK;
}

static void ds_set_format(CPU *c, DsObject *o)
{
    /* SetFormat() is a primary-buffer method.  A secondary buffer's format is
     * fixed by CreateSoundBuffer; the reference returns INVALIDPARAM here. */
    if (!(o->flags & DSBCAPS_PRIMARYBUFFER)) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    DsObject tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!ds_parse_format(wc3_legacy_arg32(c, 1), &tmp)) {
        c->eax = DSERR_BADFORMAT;
        return;
    }
    o->rate = tmp.rate;
    o->channels = tmp.channels;
    o->bits = tmp.bits;
    o->bpf = tmp.bpf;
    o->frequency = tmp.rate;
    c->eax = DS_OK;
}

static void ds_play(CPU *c, DsObject *o)
{
    uint32_t reserved1 = wc3_legacy_arg32(c, 1);
    uint32_t reserved2 = wc3_legacy_arg32(c, 2);
    uint32_t flags = wc3_legacy_arg32(c, 3);
    if (reserved1 != 0u || reserved2 != 0u ||
        (flags & ~DSBPLAY_LOOPING) != 0u) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    o->loop = (flags & DSBPLAY_LOOPING) ? 1 : 0;
    o->playing = 1;
    const Wc3AudioApi *a = ds_audio();
    if (o->voice && a && a->voice_control)
        (void)a->voice_control(a->ctx, o->voice, 1, o->loop,
                               o->frequency, o->volume, o->pan);
    c->eax = DS_OK;
}

static void ds_lock(CPU *c, DsObject *o)
{
    uint32_t offset = wc3_legacy_arg32(c, 1);
    uint32_t length = wc3_legacy_arg32(c, 2);
    uint32_t p1 = wc3_legacy_arg32(c, 3);
    uint32_t p2 = wc3_legacy_arg32(c, 4);
    uint32_t p3 = wc3_legacy_arg32(c, 5);
    uint32_t p4 = wc3_legacy_arg32(c, 6);
    uint32_t flags = wc3_legacy_arg32(c, 7);
    if (!p1 || !p2 || !wc3_legacy_guest_ok(p1, 4u) ||
        !wc3_legacy_guest_ok(p2, 4u) ||
        (p3 && !wc3_legacy_guest_ok(p3, 4u)) ||
        (p4 && !wc3_legacy_guest_ok(p4, 4u)) ||
        (flags & ~3u) || o->locked) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        const Wc3AudioApi *a = ds_audio();
        uint32_t play = 0u, write = 0u;
        uint64_t pf = 0u;
        if (!a || !a->voice_position ||
            a->voice_position(a->ctx, o->voice, &play, &write, &pf) != 0) {
            c->eax = DSERR_GENERIC;
            return;
        }
        offset = write;
    }
    if (flags & DSBLOCK_ENTIREBUFFER) { offset = 0u; length = o->bytes; }

    /* A primary buffer has no guest-retained PCM storage, but Mss32's
     * DSSCL_WRITEPRIMARY probe still performs Lock(0, 0)/Unlock(0, 0) on it.
     * Windows accepts that no-op and returns null spans; rejecting it made the
     * DirectSound driver fall back to WaveOut. */
    if (!o->bytes) {
        if (offset != 0u || length != 0u) {
            c->eax = DSERR_INVALIDPARAM;
            return;
        }
        o->locked = 1;
        o->lock_offset = 0u;
        o->lock_bytes = 0u;
        wc3_legacy_wr32g(p1, 0u);
        wc3_legacy_wr32g(p2, 0u);
        if (p3) wc3_legacy_wr32g(p3, 0u);
        if (p4) wc3_legacy_wr32g(p4, 0u);
        c->eax = DS_OK;
        return;
    }

    if (offset >= o->bytes || length > o->bytes ||
        (offset % o->bpf) != 0u || (length % o->bpf) != 0u) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    uint32_t first = o->bytes - offset;
    if (length < first) first = length;
    uint32_t second = length - first;
    if (second && (!p3 || !p4)) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    o->locked = 1;
    o->lock_offset = offset;
    o->lock_bytes = length;
    wc3_legacy_wr32g(p1, o->guest_samples + offset);
    wc3_legacy_wr32g(p2, first);
    if (p3) wc3_legacy_wr32g(p3, second ? o->guest_samples : 0u);
    if (p4) wc3_legacy_wr32g(p4, second);
    c->eax = DS_OK;
}

static void ds_unlock(CPU *c, DsObject *o)
{
    const Wc3AudioApi *a = ds_audio();
    if (!o->locked || !o->bpf) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    uint32_t first = o->bytes - o->lock_offset;
    if (o->lock_bytes < first) first = o->lock_bytes;
    uint32_t second = o->lock_bytes - first;
    uint32_t p1 = wc3_legacy_arg32(c, 1);
    uint32_t n1 = wc3_legacy_arg32(c, 2);
    uint32_t p2 = wc3_legacy_arg32(c, 3);
    uint32_t n2 = wc3_legacy_arg32(c, 4);

    /* A zero-length Lock/Unlock pair is a legal Windows no-op.  The guest still
     * calls PcmVoice::published(), which clears the stale window and counts
     * one publish while playing; the Mss32 primary-buffer probe uses the same
     * pair with null spans.  Mirror that instead of inventing a failure. */
    if (o->lock_bytes == 0u) {
        uint32_t expect = o->guest_samples
            ? o->guest_samples + o->lock_offset : 0u;
        if (p1 != expect || n1 != 0u || p2 != 0u || n2 != 0u) {
            c->eax = DSERR_INVALIDPARAM;
            return;
        }
        if (o->voice && a && a->voice_published)
            (void)a->voice_published(a->ctx, o->voice);
        o->locked = 0;
        c->eax = DS_OK;
        return;
    }

    if (!o->guest_samples || !o->bytes ||
        p1 != o->guest_samples + o->lock_offset || n1 != first ||
        p2 != (second ? o->guest_samples : 0u) || n2 != second) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    if (!wc3_legacy_guest_ok(p1, first) ||
        (second && !wc3_legacy_guest_ok(p2, second))) {
        c->eax = DSERR_INVALIDPARAM;
        return;
    }
    if (!a || !a->voice_publish2) {
        c->eax = DSERR_GENERIC;
        return;
    }

    /* Publish exactly the two regions the guest locked, at the same circular
     * offsets: samples+lockOffset and samples+0.  This is ONE Unlock and must
     * count as one publish for the stale threshold; publishing the spans
     * separately made a wrapping Unlock count twice.  The second frame base is
     * the absolute continuation (lockOffset/bpf + first/bpf), which wraps to
     * physical frame 0 by construction. */
    uint64_t base1 = o->lock_offset / o->bpf;
    uint64_t base2 = base1 + first / o->bpf;
    uint32_t accepted = 0u;
    uint32_t total = first + second;
    if (a->voice_publish2(a->ctx, o->voice, base1,
                          (const void *)(uintptr_t)p1, first,
                          base2, second ? (const void *)(uintptr_t)p2 : NULL,
                          second, &accepted) != 0 ||
        accepted != total) {
        c->eax = DSERR_GENERIC;
        return;
    }
    o->locked = 0;
    /* Unlock publishes data; it must NOT start playback.  DirectSound starts
     * sound only through Play(), and forcing playing=1 here made a stopped
     * buffer come back to life on every buffer refill. */
    c->eax = DS_OK;
}

static void dsound_dispatch(CPU *c, unsigned kind, unsigned method)
{
    uint32_t self = wc3_legacy_arg32(c, 0);
    DsObject *o = ds_find(self);
    if (!o || o->kind != kind) { c->eax = DSERR_INVALIDPARAM; return; }
    const Wc3AudioApi *a = ds_audio();

    if (method == 0u) { ds_query_interface(c, o); return; }
    if (method == 1u) { o->refs++; c->eax = o->refs; return; }
    if (method == 2u) { ds_release(c, o); return; }

    if (kind == 1u) {
        switch (method) {
        case 3u: ds_create_sound_buffer(c, o); return;
        case 4u: ds_get_caps(c, o); return;
        case 5u: c->eax = DSERR_UNSUPPORTED; return; /* DuplicateSoundBuffer */
        case 6u: {
            /* The HLE validates a registered guest window and level 1..3.
             * We do not keep a window registry at this seam, but a NULL
             * window or an out-of-range level is invalid on Windows too. */
            uint32_t window = wc3_legacy_arg32(c, 1);
            uint32_t level = wc3_legacy_arg32(c, 2);
            /* DSSCL_WRITEPRIMARY (4) is what Mss32's DirectSound driver asks
             * for on a non-emulated device; it then probes the primary buffer.
             * Levels 1..4 are valid, higher values are not. */
            if (!window || level < 1u || level > 4u) {
                c->eax = DSERR_INVALIDPARAM; return;
            }
            c->eax = DS_OK;
            return;
        }
        case 7u: c->eax = DS_OK; return;   /* Compact */
        case 8u: {
            uint32_t out = wc3_legacy_arg32(c, 1);
            if (!out || !wc3_legacy_guest_ok(out, 4u)) {
                c->eax = E_POINTER; return;
            }
            wc3_legacy_wr32g(out, 4u);    /* DSSPEAKER_STEREO */
            c->eax = DS_OK;
            return;
        }
        case 9u: {
            uint32_t config = wc3_legacy_arg32(c, 1);
            /* The mixer renders stereo only: default, headphones and stereo
             * are truthful; quad/5.1/7.1 must not be silently accepted. */
            if (config != 0u && config != 1u && config != 4u) {
                c->eax = DSERR_UNSUPPORTED; return;
            }
            c->eax = DS_OK;
            return;
        }
        case 10u: c->eax = DSERR_ALREADYINITIALIZED; return;
        default: c->eax = E_NOTIMPL; return;
        }
    }

    switch (method) {
    case 3u: {  /* GetCaps on a buffer */
        uint32_t out = wc3_legacy_arg32(c, 1);
        if (!out || !wc3_legacy_guest_ok(out, 20u) ||
            wc3_legacy_rd32g(out) != 20u) {
            c->eax = DSERR_INVALIDPARAM; return;
        }
        wc3_legacy_wr32g(out + 4u, (o->flags & ~DSBSTATUS_LOOPING) | 8u);
        wc3_legacy_wr32g(out + 8u, o->bytes);
        wc3_legacy_wr32g(out + 12u, 0u);
        wc3_legacy_wr32g(out + 16u, 0u);
        c->eax = DS_OK;
        return;
    }
    case 4u: ds_get_current_position(c, o); return;
    case 5u: ds_get_format(c, o); return;
    case 6u: {
        uint32_t out = wc3_legacy_arg32(c, 1);
        if (!out || !wc3_legacy_guest_ok(out, 4u)) {
            c->eax = E_POINTER; return;
        }
        wc3_legacy_wr32g(out, (uint32_t)(int32_t)o->volume);
        c->eax = DS_OK; return;
    }
    case 7u: {
        uint32_t out = wc3_legacy_arg32(c, 1);
        if (!out || !wc3_legacy_guest_ok(out, 4u)) {
            c->eax = E_POINTER; return;
        }
        wc3_legacy_wr32g(out, (uint32_t)(int32_t)o->pan);
        c->eax = DS_OK; return;
    }
    case 8u: {
        uint32_t out = wc3_legacy_arg32(c, 1);
        if (!out || !wc3_legacy_guest_ok(out, 4u)) {
            c->eax = E_POINTER; return;
        }
        wc3_legacy_wr32g(out, o->frequency);
        c->eax = DS_OK; return;
    }
    case 9u: ds_get_status(c, o); return;
    case 10u: c->eax = DSERR_ALREADYINITIALIZED; return;
    case 11u: ds_lock(c, o); return;
    case 12u: ds_play(c, o); return;
    case 13u: ds_set_current_position(c, o); return;
    case 14u: ds_set_format(c, o); return;
    case 15u: {
        int32_t v = (int32_t)wc3_legacy_arg32(c, 1);
        if (v > 0 || v < -10000) { c->eax = DSERR_INVALIDPARAM; return; }
        o->volume = (int)v;
        if (a && a->voice_control)
            (void)a->voice_control(a->ctx, o->voice, -1, -1,
                                   o->frequency, o->volume, o->pan);
        c->eax = DS_OK; return;
    }
    case 16u: {
        int32_t v = (int32_t)wc3_legacy_arg32(c, 1);
        if (v < -10000 || v > 10000) { c->eax = DSERR_INVALIDPARAM; return; }
        o->pan = (int)v;
        if (a && a->voice_control)
            (void)a->voice_control(a->ctx, o->voice, -1, -1,
                                   o->frequency, o->volume, o->pan);
        c->eax = DS_OK; return;
    }
    case 17u: {
        uint32_t f = wc3_legacy_arg32(c, 1);
        if (f && (f < 100u || f > 200000u)) {
            c->eax = DSERR_INVALIDPARAM; return;
        }
        o->frequency = f ? f : o->rate;
        if (a && a->voice_control)
            (void)a->voice_control(a->ctx, o->voice, -1, -1,
                                   o->frequency, o->volume, o->pan);
        c->eax = DS_OK; return;
    }
    case 18u: {
        o->playing = 0;
        if (o->voice && a && a->voice_control)
            (void)a->voice_control(a->ctx, o->voice, 0, o->loop,
                                   o->frequency, o->volume, o->pan);
        c->eax = DS_OK; return;
    }
    case 19u: ds_unlock(c, o); return;
    case 20u:
        /* Restore() reacquires a lost buffer; the mixer shadow never left. */
        c->eax = DS_OK; return;
    case 21u:
        c->eax = DSERR_UNSUPPORTED; return; /* SetNotificationPositions */
    default: c->eax = E_NOTIMPL; return;
    }
}

static void h_dsound_create(CPU *c)
{
    uint32_t guid = wc3_legacy_arg32(c, 0);
    uint32_t out = wc3_legacy_arg32(c, 1);
    uint32_t outer = wc3_legacy_arg32(c, 2);

    /* Windows checks the out pointer first, then aggregation, then the
     * device GUID; the reference preserves that exact precedence. */
    if (!out || !wc3_legacy_guest_ok(out, 4u)) {
        c->eax = E_POINTER; return;
    }
    wc3_legacy_wr32g(out, 0u);
    if (outer) { c->eax = CLASS_E_NOAGGREGATION; return; }
    /* NULL selects the default playback device; an explicit GUID we do not
     * implement must fail with DSERR_NODRIVER, not pretend to work. */
    if (guid) { c->eax = DSERR_NODRIVER; return; }

    const Wc3AudioApi *a = ds_audio();
    if (!a || !a->voice_add || !a->open || !a->primary_voice) {
        c->eax = DSERR_NODRIVER;
        return;
    }

    /* Mss32 chooses DirectSound instead of WaveOut.  Open the single AAudio
     * output here and let the platform create its persistent primary voice.
     * If the platform already has a primary (a second DirectSound device or
     * a reopen), reuse it instead of creating a second stream. */
    uintptr_t stream = 0u;
    uintptr_t voice = a->primary_voice(a->ctx, 0u);
    int owns_stream = 0;
    if (!voice) {
        if (a->open(a->ctx, 44100u, 2u, 16u, &stream) != 0 || !stream) {
            c->eax = DSERR_NODRIVER;
            return;
        }
        voice = a->primary_voice(a->ctx, stream);
        if (!voice) {
            if (a->close) (void)a->close(a->ctx, stream);
            c->eax = DSERR_NODRIVER;
            return;
        }
        owns_stream = 1;
    }

    DsObject *o = ds_alloc(1u);
    if (!o) {
        if (owns_stream && a->close) (void)a->close(a->ctx, stream);
        c->eax = DSERR_OUTOFMEMORY;
        return;
    }
    o->stream = stream;
    o->owns_stream = owns_stream;
    o->rate = 44100u;
    o->channels = 2u;
    o->bits = 16u;
    o->bpf = 4u;
    wc3_legacy_wr32g(out, o->self);
    c->eax = DS_OK;
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr,
            "[dsound] DirectSoundCreate -> %08X refs=%u stream=%s"
            " voice=%08X\n",
            o->self, o->refs, owns_stream ? "open" : "shared",
            (unsigned)voice);
#endif
}

int wc3_hle_dsound_install(void)
{
    if (atomic_exchange_explicit(&g_installed, 1, memory_order_acq_rel))
        return 0;
    memset(g_thunk, 0, sizeof g_thunk);

    if (wc3_legacy_hle_add("dsound.dll", "DirectSoundCreate", 3, 0,
                           h_dsound_create) != 0)
        return -1;
    g_create_thunk = wc3_legacy_hle_thunk_of("dsound.dll",
                                             "DirectSoundCreate");
    if (!g_create_thunk) return -1;
    /* DirectSoundCreate/#1 is bound as well. */
    if (wc3_legacy_hle_bind_ordinal("dsound.dll", 1u, 3, 0,
                                    h_dsound_create) != 0)
        return -1;

    if (wc3_legacy_hle_add("dsound.dll", "DirectSoundEnumerateA", 2, 0,
                           h_dsound_enumerate) != 0)
        return -1;
    g_enum_thunk = wc3_legacy_hle_thunk_of("dsound.dll",
                                           "DirectSoundEnumerateA");
    if (!g_enum_thunk) return -1;

    for (unsigned i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        const DsRow *r = &g_rows[i];
        char name[32];
        snprintf(name, sizeof name, "DSound_%u_%u", r->kind, r->method);
        if (wc3_legacy_hle_add("dsound.dll", name, (int)r->argc, 0,
                               r->fn) != 0)
            return -1;
        g_thunk[r->kind][r->method] =
            wc3_legacy_hle_thunk_of("dsound.dll", name);
        if (!g_thunk[r->kind][r->method]) return -1;
    }

    /* Mss32's DirectSound loader is a KERNEL32 dynamic-load sequence.  Chain
     * after WS2_32 (installed first), so unknown names still reach the as-built
     * loader and the existing network synthetic modules keep working. */
    if (wc3_legacy_hle_chain("KERNEL32.dll", "LoadLibraryA",
                             h_load_library_a,
                             &previous_load_library) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "GetProcAddress",
                             h_get_proc_address,
                             &previous_get_proc_address) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "GetModuleHandleA",
                             h_get_module_handle_a,
                             &previous_get_module_handle) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "GetModuleFileNameA",
                             h_get_module_file_name_a,
                             &previous_get_module_file_name) != 0)
        return -1;

#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr,
            "[dsound] installed %u COM rows; synthetic DSOUND.DLL %08X\n",
            (unsigned)(sizeof g_rows / sizeof g_rows[0]), DS_HMOD);
#endif
    return 0;
}
