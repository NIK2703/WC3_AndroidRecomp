/*
 * platform_headless_test.c - host-side contract test for the verification
 * platform.  This is the T0 counterpart of tests/platform_android_test.c: the
 * same Wc3Platform entries, but no device, no Android headers and no licensed
 * corpus required.
 */
#define _GNU_SOURCE 1

#include "guest_layout.h"
#include "guest_memory.h"
#include "platform_headless.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <unistd.h>
#define make_dir(path) mkdir((path), 0700)
#endif

static int g_failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        g_failures++;
    }
}

static int child_main(void *arg)
{
    const Wc3ThreadApi *t = (const Wc3ThreadApi *)arg;
    uint32_t tls = 0xFFFFFFFFu;
    if (t->tls_alloc(t->ctx, &tls) != 0) return -1;
    if (t->tls_set(t->ctx, tls, 0x12345678u) != 0) return -2;
    if (t->tls_get(t->ctx, tls) != 0x12345678u) return -3;
    return 42;
}

static void test_memory(const Wc3MemApi *m)
{
    const uint32_t base = 0x61000000u;
    const uint32_t size = 0x10000u;
    check(m->reserve(m->ctx, base, size) == 0, "memory reserve");
    check(m->commit(m->ctx, base, size) == 0, "memory commit");
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)base;
    p[0] = 0xA5A5A5A5u;
    p[size / 4u - 1u] = 0x5A5A5A5Au;
    check(p[0] == 0xA5A5A5A5u, "memory write first");
    check(p[size / 4u - 1u] == 0x5A5A5A5Au, "memory write last");
    check(m->protect(m->ctx, base, size, WC3_MEM_READ) == 0,
          "memory protect read");
    check(p[0] == 0xA5A5A5A5u, "memory read after protect");
    check(m->decommit(m->ctx, base, size) == 0, "memory decommit");
    check(m->release(m->ctx, base, size) == 0, "memory release");
}

static void test_guest_memory(const Wc3Platform *p)
{
    check(wc3_guest_memory_init(p) == 0, "guest memory init");
    check(wc3_guest_ok(GUEST_STACK_BASE, 0x1000u) == 1, "guest stack ok");
    check(wc3_guest_ok(GUEST_HEAP_BASE, 0x1000u) == 1, "guest heap ok");
    check(wc3_guest_ok(GUEST_HLE_HEAP_BASE, 0x1000u) == 1,
          "guest hle heap ok");
    check(wc3_guest_ok(0xFFFFFFF0u, 0x20u) == 0, "guest wrap rejected");
    check(wc3_guest_ok(0x1000u, 0x10u) == 0, "guest unmapped rejected");
    volatile uint32_t *stack = (volatile uint32_t *)(uintptr_t)GUEST_STACK_BASE;
    stack[0] = 0x51525354u;
    check(stack[0] == 0x51525354u, "guest stack write");
}

static void test_file(const Wc3FileApi *f)
{
    const char *path = "build/host/headless-file.bin";
    (void)remove(path);
    uintptr_t h = 0;
    check(f->open(f->ctx, path,
                  WC3_FILE_READ | WC3_FILE_WRITE | WC3_FILE_CREATE |
                      WC3_FILE_TRUNCATE,
                  &h) == 0,
          "file open create");
    if (!h) return;
    const char *text = "wc3-headless";
    uint32_t written = 0;
    check(f->write(f->ctx, h, text, (uint32_t)strlen(text), &written) == 0,
          "file write");
    check(written == strlen(text), "file written length");
    check(f->close(f->ctx, h) == 0, "file close write");

    h = 0;
    check(f->open(f->ctx, path, WC3_FILE_READ, &h) == 0, "file open read");
    if (!h) return;
    uint64_t size = 0;
    check(f->size(f->ctx, h, &size) == 0, "file size");
    check(size == strlen(text), "file size value");
    Wc3FileStat st;
    check(f->stat(f->ctx, path, &st) == 0, "file stat");
    check((st.mode & WC3_FILE_STAT_REGULAR) != 0 &&
              (st.mode & WC3_FILE_STAT_DIRECTORY) == 0,
          "file stat mode");
    check(st.size == strlen(text), "file stat size");
    char buf[32] = {0};
    uint32_t got = 0;
    check(f->read(f->ctx, h, buf, sizeof buf, &got) == 0, "file read");
    check(got == strlen(text), "file read length");
    check(memcmp(buf, text, strlen(text)) == 0, "file content");
    uint64_t pos = 0;
    check(f->seek(f->ctx, h, 0, 0, &pos) == 0, "file seek set");
    check(pos == 0, "file seek position");
    check(f->close(f->ctx, h) == 0, "file close read");
    (void)remove(path);
}

static void test_time(const Wc3TimeApi *t)
{
    uint64_t mono = t->monotonic_ns(t->ctx);
    uint64_t real = t->realtime_ns(t->ctx);
    check(mono != 0, "monotonic clock");
    check(real > 1700000000000000000ull, "realtime clock");
    check(wc3_headless_clock_now_ns() != 0u, "host clock now");
    check(wc3_headless_clock_advance_ns(1u) == -1, "host clock not virtual");
    check(wc3_headless_schedule_id() == NULL, "host schedule id absent");
    check(wc3_headless_schedule_hash() == 0u, "host schedule hash absent");
    int32_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    check(t->local_time(t->ctx, real, &y, &mo, &d, &h, &mi, &s, &ms) == 0,
          "local time");
    check(y >= 2024 && y <= 2100, "local year");
    check(mo >= 1 && mo <= 12, "local month");
    check(d >= 1 && d <= 31, "local day");
    check(ms >= 0 && ms <= 999, "local millisecond");
}

static void test_thread(const Wc3ThreadApi *t)
{
    uint32_t tls = 0xFFFFFFFFu;
    check(t->tls_alloc(t->ctx, &tls) == 0, "tls alloc");
    check(tls < 64u, "tls index range");
    check(t->tls_set(t->ctx, tls, 0xAAAAAAAAu) == 0, "tls set");
    check(t->tls_get(t->ctx, tls) == 0xAAAAAAAAu, "tls get");

    Wc3ThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.name = "wc3-headless-test";
    desc.stack_size = 256u * 1024u;
    desc.entry = child_main;
    desc.arg = (void *)t;
    Wc3ThreadHandle h = 0;
    check(t->create(t->ctx, &desc, &h) == 0, "thread create");
    if (h) {
        uint32_t exit_code = 0;
        check(t->join(t->ctx, h, &exit_code) == 0, "thread join");
        check(exit_code == 42u, "thread exit code");
    }
    check(t->tls_get(t->ctx, tls) == 0xAAAAAAAAu, "tls parent isolation");
    check(t->tls_free(t->ctx, tls) == 0, "tls free");
}

static void test_input(const Wc3InputApi *in)
{
    check(in && in->push && in->set_text_focus, "input table");
    check(wc3_headless_input_pop(NULL) == -1, "input pop rejects null");
    Wc3InputEvent out;
    check(wc3_headless_input_pop(&out) == 0, "input empty");

    Wc3InputEvent event;
    memset(&event, 0, sizeof event);
    event.kind = WC3_INPUT_KEY_DOWN;
    event.key = 7u;
    check(in->push(in->ctx, &event) == 0, "input push");
    check(wc3_headless_input_pop(&out) == 1, "input pop");
    check(out.kind == WC3_INPUT_KEY_DOWN && out.key == 7u, "input event");

    event.kind = 0u;
    check(in->push(in->ctx, &event) == -1, "input rejects bad kind");
    check(wc3_headless_input_dropped() == 0u, "input dropped unchanged");
    check(in->set_text_focus(in->ctx, 1) == 0, "input text focus");
}

static void test_sync(const Wc3SyncApi *s)
{
    Wc3SyncHandle e = 0;
    check(s->event_create(s->ctx, 1, 0, &e) == 0, "manual event create");
    check(s->wait(s->ctx, &e, 1, 0, 0, NULL) == WC3_WAIT_TIMEOUT,
          "manual event initially clear");
    check(s->event_set(s->ctx, e) == 0, "manual event set");
    check(s->wait(s->ctx, &e, 1, 0, 10, NULL) == WC3_WAIT_OBJECT_0,
          "manual event wait");
    check(s->wait(s->ctx, &e, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "manual event stays set");
    check(s->event_reset(s->ctx, e) == 0, "manual event reset");
    check(s->wait(s->ctx, &e, 1, 0, 0, NULL) == WC3_WAIT_TIMEOUT,
          "manual event reset wait");
    check(s->close(s->ctx, e) == 0, "manual event close");

    Wc3SyncHandle a = 0;
    check(s->event_create(s->ctx, 0, 0, &a) == 0, "auto event create");
    check(s->event_set(s->ctx, a) == 0, "auto event set");
    check(s->wait(s->ctx, &a, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "auto event wait");
    check(s->wait(s->ctx, &a, 1, 0, 0, NULL) == WC3_WAIT_TIMEOUT,
          "auto event consumed");
    check(s->close(s->ctx, a) == 0, "auto event close");

    Wc3SyncHandle m = 0;
    check(s->mutex_create(s->ctx, 0, &m) == 0, "mutex create");
    check(s->wait(s->ctx, &m, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "mutex acquire");
    check(s->wait(s->ctx, &m, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "mutex recursive acquire");
    check(s->mutex_release(s->ctx, m) == 0, "mutex release one");
    check(s->mutex_release(s->ctx, m) == 0, "mutex release two");
    check(s->mutex_release(s->ctx, m) == -1, "mutex release unowned");
    check(s->close(s->ctx, m) == 0, "mutex close");

    Wc3SyncHandle sem = 0;
    check(s->semaphore_create(s->ctx, 1, 2, &sem) == 0, "semaphore create");
    check(s->wait(s->ctx, &sem, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "semaphore acquire");
    check(s->wait(s->ctx, &sem, 1, 0, 0, NULL) == WC3_WAIT_TIMEOUT,
          "semaphore empty");
    int32_t previous = -1;
    check(s->semaphore_release(s->ctx, sem, 1, &previous) == 0,
          "semaphore release");
    check(previous == 0, "semaphore previous");
    check(s->wait(s->ctx, &sem, 1, 0, 0, NULL) == WC3_WAIT_OBJECT_0,
          "semaphore reacquire");
    check(s->semaphore_release(s->ctx, sem, 2, NULL) == 0,
          "semaphore release to max");
    check(s->semaphore_release(s->ctx, sem, 1, NULL) == -1,
          "semaphore over max");
    check(s->close(s->ctx, sem) == 0, "semaphore close");

    Wc3SyncHandle two[2] = {0, 0};
    check(s->event_create(s->ctx, 0, 0, &two[0]) == 0, "wait_all event 0");
    check(s->event_create(s->ctx, 0, 0, &two[1]) == 0, "wait_all event 1");
    check(s->event_set(s->ctx, two[1]) == 0, "wait_all set second");
    check(s->wait(s->ctx, two, 2, 1, 0, NULL) == WC3_WAIT_TIMEOUT,
          "wait_all needs both");
    uint32_t index = 99u;
    check(s->wait(s->ctx, two, 2, 0, 0, &index) == WC3_WAIT_OBJECT_0 + 1,
          "wait_any picks second");
    check(index == 1u, "wait_any index");
    check(s->event_set(s->ctx, two[0]) == 0, "wait_all set first");
    check(s->event_set(s->ctx, two[1]) == 0, "wait_all set second again");
    check(s->wait(s->ctx, two, 2, 1, 10, NULL) == WC3_WAIT_OBJECT_0,
          "wait_all both");
    check(s->close(s->ctx, two[0]) == 0, "wait_all close 0");
    check(s->close(s->ctx, two[1]) == 0, "wait_all close 1");

    Wc3SyncHandle cs = 0;
    check(s->critical_section_create(s->ctx, &cs) == 0, "cs create");
    check(s->critical_section_enter(s->ctx, cs) == 0, "cs enter");
    check(s->critical_section_try_enter(s->ctx, cs) == 1, "cs recursive");
    check(s->critical_section_leave(s->ctx, cs) == 0, "cs leave one");
    check(s->critical_section_leave(s->ctx, cs) == 0, "cs leave two");
    check(s->critical_section_try_enter(s->ctx, cs) == 1, "cs try enter");
    check(s->critical_section_leave(s->ctx, cs) == 0, "cs leave three");
    check(s->close(s->ctx, cs) == 0, "cs close");
}

typedef struct MutexChild {
    const Wc3SyncApi *sync;
    Wc3SyncHandle mutex;
} MutexChild;

static int mutex_child_main(void *arg)
{
    MutexChild *child = (MutexChild *)arg;
    if (child->sync->wait(child->sync->ctx, &child->mutex, 1, 0, 0, NULL) !=
        WC3_WAIT_OBJECT_0)
        return -1;
    /* Exit while owning the mutex: Windows abandons it. */
    return 0;
}

static void test_abandoned_mutex(const Wc3SyncApi *s, const Wc3ThreadApi *t)
{
    MutexChild child;
    child.sync = s;
    child.mutex = 0;
    check(s->mutex_create(s->ctx, 0, &child.mutex) == 0,
          "abandoned mutex create");
    Wc3ThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.name = "wc3-abandon";
    desc.stack_size = 128u * 1024u;
    desc.entry = mutex_child_main;
    desc.arg = &child;
    Wc3ThreadHandle h = 0;
    check(t->create(t->ctx, &desc, &h) == 0, "abandoned thread create");
    if (h) {
        uint32_t exit_code = 1u;
        check(t->join(t->ctx, h, &exit_code) == 0, "abandoned thread join");
        check(exit_code == 0u, "abandoned child exit");
        check(s->wait(s->ctx, &child.mutex, 1, 0, 10, NULL) ==
                  WC3_WAIT_ABANDONED_0,
              "abandoned mutex wait");
        check(s->mutex_release(s->ctx, child.mutex) == 0,
              "abandoned mutex release");
    }
    check(s->close(s->ctx, child.mutex) == 0, "abandoned mutex close");
}

static void test_window(const Wc3WindowApi *w)
{
    uintptr_t win = 0;
    check(w->create(w->ctx, 800, 600, "headless", &win) == 0,
          "window create");
    uint32_t width = 0, height = 0;
    check(w->size(w->ctx, win, &width, &height) == 0, "window size");
    check(width == 800 && height == 600, "window dimensions");
    check(w->set_title(w->ctx, win, "renamed") == 0, "window title");
    w->destroy(w->ctx, win);
}

static void test_clipboard(const Wc3ClipboardApi *c)
{
    check(c->set_text(c->ctx, "hello") == 0, "clipboard set");
    uint32_t needed = 0;
    char small[3] = {0};
    check(c->get_text(c->ctx, small, sizeof small, &needed) == 0,
          "clipboard get small");
    check(needed == 6u, "clipboard needed");
    check(strcmp(small, "he") == 0, "clipboard truncation");
    char full[16] = {0};
    check(c->get_text(c->ctx, full, sizeof full, &needed) == 0,
          "clipboard get full");
    check(strcmp(full, "hello") == 0, "clipboard full text");
    check(c->set_text(c->ctx, NULL) == 0, "clipboard clear");
    check(c->get_text(c->ctx, full, sizeof full, &needed) == 0,
          "clipboard get empty");
    check(full[0] == '\0' && needed == 1u, "clipboard empty");
}

static void test_locale(const Wc3LocaleApi *l)
{
    check(l->ansi_codepage(l->ctx) != 0u, "ansi codepage");
    check(l->oem_codepage(l->ctx) != 0u, "oem codepage");
    check(l->default_lcid(l->ctx) != 0u, "default lcid");

    uint16_t wide[8] = {0};
    check(l->to_wide(l->ctx, 65001u, "A\xC3\xA9", -1, wide, 8) == 2,
          "utf8 to wide count");
    check(wide[0] == 'A' && wide[1] == 0x00E9u, "utf8 to wide value");
    char narrow[8] = {0};
    check(l->from_wide(l->ctx, 65001u, wide, 2, narrow, 8) == 3,
          "wide to utf8 count");
    check(memcmp(narrow, "A\xC3\xA9", 3) == 0, "wide to utf8 value");

    uint16_t latin[2] = {0};
    check(l->to_wide(l->ctx, 1252u, "\xE9", 1, latin, 2) == 1,
          "1252 to wide count");
    check(latin[0] == 0x00E9u, "1252 to wide value");
}

static uint64_t g_audio_consumed_seen;
static int g_audio_consumed_calls;

static void audio_consumed_probe(void *opaque, uint64_t total)
{
    (void)opaque;
    g_audio_consumed_seen = total;
    g_audio_consumed_calls++;
}

static void test_audio_surface(const Wc3Platform *p)
{
    uintptr_t stream = 0;
    check(p->audio.open(p->audio.ctx, 44100, 2, 16, &stream) == 0,
          "audio open");
    uint32_t written = 0;
    uint8_t frames[4] = {0};
    check(p->audio.write(p->audio.ctx, stream, frames, sizeof frames,
                         &written) == 0,
          "audio write");
    check(written == sizeof frames, "audio written");
    check(p->audio.set_callback(p->audio.ctx, stream, NULL, NULL) == 0,
          "audio callback");

    /* A-12.4 S1: the virtual device clock must be steppable and must never
     * report more consumed bytes than were written.  This is the T1 side of
     * the completion pump (Android drives it from the RT callback instead). */
    check(p->audio.set_consumed && p->audio.consumed_bytes &&
              p->audio.advance && p->audio.latency_frames,
          "audio device-clock API");
    check(p->audio.latency_frames(p->audio.ctx, stream) == 0,
          "headless has no hardware queue");
    g_audio_consumed_seen = 0;
    g_audio_consumed_calls = 0;
    check(p->audio.set_consumed(p->audio.ctx, stream, audio_consumed_probe,
                                NULL) == 0,
          "audio consumed callback");
    check(p->audio.consumed_bytes(p->audio.ctx, stream) == 0,
          "device clock starts at zero");
    check(p->audio.advance(p->audio.ctx, stream, 2) == 0,
          "advance half a block");
    check(g_audio_consumed_calls == 1 && g_audio_consumed_seen == 2,
          "consumed pump fired for 2 bytes");
    check(p->audio.advance(p->audio.ctx, stream, 100) == 0,
          "advance past queued bytes");
    check(g_audio_consumed_seen == 4,
          "device clock bounded by written bytes");
    check(p->audio.consumed_bytes(p->audio.ctx, stream) == 4,
          "consumed counter");

    check(p->audio.close(p->audio.ctx, stream) == 0, "audio close");

    uintptr_t surface = 0;
    check(p->surface.acquire(p->surface.ctx, &surface) == 0,
          "surface acquire");
    check(surface != 0u, "surface handle");
    p->surface.resize(p->surface.ctx, 640, 480);
    uint32_t width = 0, height = 0;
    check(p->window.size(p->window.ctx, surface, &width, &height) == 0,
          "surface resize size");
    check(width == 640 && height == 480, "surface resize value");
    p->surface.lost(p->surface.ctx);
    p->surface.release(p->surface.ctx, surface);
}

/* S-1: the headless pull twin renders the same PCM the Android callback will
 * render, with a steppable clock and a deterministic PCM hash. */
static void test_audio_pull(const Wc3Platform *p)
{
    uint8_t src[1024];
    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)(i * 7u + 3u);

    uintptr_t first = 0, second = 0;
    uint32_t written = 0;
    check(p->audio.publish != NULL, "headless publish is installed");
    check(p->audio.open(p->audio.ctx, 44100, 2, 16, &first) == 0,
          "pull open");
    wc3_headless_audio_enable_pull(1);
    wc3_headless_audio_set_burst(64);
    check(p->audio.publish(p->audio.ctx, first, 0, src, sizeof src,
                           &written) == 0, "pull publish");
    check(written == sizeof src, "pull publish accepted whole region");

    g_audio_consumed_seen = 0;
    g_audio_consumed_calls = 0;
    check(p->audio.set_consumed(p->audio.ctx, first, audio_consumed_probe,
                                NULL) == 0, "pull consumed callback");
    check(p->audio.advance(p->audio.ctx, first, sizeof src) == 0,
          "pull advance");
    check(g_audio_consumed_calls == 4, "burst cadence is deterministic");
    check(g_audio_consumed_seen == sizeof src,
          "consumed pump reached published bytes");

    Wc3HeadlessPullStats a;
    memset(&a, 0, sizeof a);
    check(wc3_headless_audio_pull_stats(first, &a) == 0, "pull stats");
    check(a.pull_mode == 1, "pull mode active");
    check(a.published_frame == 256u, "published frame cursor");
    check(a.play_frame == 256u, "play frame cursor");
    check(a.consumed == sizeof src, "pull device clock");
    check(a.stale_frames == 0u, "no stale frames when fully published");
    check(a.render_calls == 4u, "render calls match bursts");

    check(p->audio.open(p->audio.ctx, 44100, 2, 16, &second) == 0,
          "pull open replay");
    wc3_headless_audio_set_burst(64);
    check(p->audio.publish(p->audio.ctx, second, 0, src, sizeof src,
                           &written) == 0, "pull publish replay");
    check(p->audio.advance(p->audio.ctx, second, sizeof src) == 0,
          "pull advance replay");
    Wc3HeadlessPullStats b;
    memset(&b, 0, sizeof b);
    check(wc3_headless_audio_pull_stats(second, &b) == 0,
          "pull stats replay");
    check(a.pcm_hash == b.pcm_hash,
          "PCM hash is identical across deterministic replays");

    check(p->audio.advance(p->audio.ctx, first, 256u) == 0,
          "pull underrun advance");
    Wc3HeadlessPullStats c;
    memset(&c, 0, sizeof c);
    check(wc3_headless_audio_pull_stats(first, &c) == 0,
          "pull stats underrun");
    check(c.stale_frames > 0u, "underrun frames counted as stale");
    check(c.consumed == sizeof src + 256u,
          "device clock advances through underrun");

    check(p->audio.close(p->audio.ctx, first) == 0, "pull close");
    check(p->audio.close(p->audio.ctx, second) == 0, "pull close replay");
}

static void test_config(const Wc3ConfigApi *c)
{
    const char *value = c->get(c->ctx, "WC3_HEADLESS_TEST");
    check(value != NULL && strcmp(value, "42") == 0, "config string");
    check(c->get(c->ctx, "WC3_HEADLESS_TEST") == value,
          "config pointer stable");
    check(c->get_int(c->ctx, "WC3_HEADLESS_TEST", 0) == 42,
          "config int");
    check(c->get(c->ctx, "WC3_HEADLESS_MISSING") == NULL,
          "config missing");
    check(c->get_int(c->ctx, "WC3_HEADLESS_MISSING", 7) == 7,
          "config fallback");
    check(c->get_int(c->ctx, "WC3_HEADLESS_BAD", 3) == 3,
          "config bad fallback");
}

static void create_file(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp) (void)fclose(fp);
}

static void test_asset(const Wc3AssetApi *a)
{
    const char *root = "build/host/headless-assets";
    const char *save = "build/host/headless-save";
    (void)make_dir(root);
    (void)make_dir(save);
    create_file("build/host/headless-assets/War3.MPQ");
    create_file("build/host/headless-assets/war3x.mpq");
    create_file("build/host/headless-assets/readme.txt");

    check(a->init(a->ctx, root, save) == 0, "asset init");
    check(a->asset_root(a->ctx) &&
              strcmp(a->asset_root(a->ctx), root) == 0,
          "asset root");
    check(a->save_root(a->ctx) && strcmp(a->save_root(a->ctx), save) == 0,
          "save root");
    check(a->present(a->ctx, "war3.mpq") == 1, "asset case insensitive");
    check(a->present(a->ctx, "WAR3X.MPQ") == 1, "asset uppercase");
    check(a->present(a->ctx, "missing.mpq") == 0, "asset missing");
    check(a->present(a->ctx, "../war3.mpq") == 0, "asset escape rejected");

    char name[64] = {0};
    check(a->enumerate_mpq(a->ctx, 0, name, sizeof name) == 1,
          "asset enumerate 0");
    check(strcmp(name, "War3.MPQ") == 0, "asset enumerate sorted 0");
    check(a->enumerate_mpq(a->ctx, 1, name, sizeof name) == 1,
          "asset enumerate 1");
    check(strcmp(name, "war3x.mpq") == 0, "asset enumerate sorted 1");
    check(a->enumerate_mpq(a->ctx, 2, name, sizeof name) == 0,
          "asset enumerate end");

    (void)remove("build/host/headless-assets/War3.MPQ");
    (void)remove("build/host/headless-assets/war3x.mpq");
    (void)remove("build/host/headless-assets/readme.txt");
}

typedef struct SchedCtx {
    const Wc3ThreadApi *thread;
    const Wc3SyncApi *sync;
    const Wc3TimeApi *time;
    Wc3SyncHandle event;
    char *order;
    uint32_t *count;
    char first;
    char second;
    uint32_t timeout_ms;
    uint64_t elapsed_ns;
} SchedCtx;

static int sched_yield_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    c->order[(*c->count)++] = c->first;
    c->thread->yield(c->thread->ctx);
    c->order[(*c->count)++] = c->second;
    return 0;
}

static int sched_wait_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    const uint32_t rc = (uint32_t)c->sync->wait(
        c->sync->ctx, &c->event, 1, 0, UINT32_MAX, NULL);
    c->order[(*c->count)++] = rc == WC3_WAIT_OBJECT_0 ? 'w' : '!';
    return 0;
}

static int sched_wait_char_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    const uint32_t rc = (uint32_t)c->sync->wait(
        c->sync->ctx, &c->event, 1, 0, UINT32_MAX, NULL);
    c->order[(*c->count)++] = rc == WC3_WAIT_OBJECT_0 ? c->first : '!';
    return 0;
}

static int sched_signal_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    if (c->thread->sleep_ns(c->thread->ctx, 10000000ull) != 0) return -1;
    return c->sync->event_set(c->sync->ctx, c->event);
}

static int sched_timeout_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    const uint64_t start =
        c->time ? c->time->monotonic_ns(c->time->ctx) : 0u;
    const uint32_t rc = (uint32_t)c->sync->wait(
        c->sync->ctx, &c->event, 1, 0, c->timeout_ms, NULL);
    c->elapsed_ns =
        (c->time ? c->time->monotonic_ns(c->time->ctx) : 0u) - start;
    c->order[(*c->count)++] = rc == WC3_WAIT_TIMEOUT ? 't' : '!';
    return 0;
}

static int sched_exit_child(void *arg)
{
    SchedCtx *c = (SchedCtx *)arg;
    return c->thread->sleep_ns(c->thread->ctx, 2000000ull);
}

typedef struct SchedMutexCtx {
    const Wc3ThreadApi *thread;
    const Wc3SyncApi *sync;
    Wc3SyncHandle mutex;
    Wc3SyncHandle acquired;
    char *order;
    uint32_t *count;
} SchedMutexCtx;

static int sched_mutex_owner_child(void *arg)
{
    SchedMutexCtx *c = (SchedMutexCtx *)arg;
    if (c->sync->wait(c->sync->ctx, &c->mutex, 1, 0, UINT32_MAX, NULL) !=
        WC3_WAIT_OBJECT_0)
        return -1;
    if (c->sync->event_set(c->sync->ctx, c->acquired) != 0) return -1;
    /* Keep ownership across a blocking point so the waiter is already
     * parked on the mutex when this thread exits. */
    return c->thread->sleep_ns(c->thread->ctx, 2000000ull);
}

static int sched_mutex_waiter_child(void *arg)
{
    SchedMutexCtx *c = (SchedMutexCtx *)arg;
    const uint32_t rc = (uint32_t)c->sync->wait(
        c->sync->ctx, &c->mutex, 1, 0, UINT32_MAX, NULL);
    c->order[(*c->count)++] = rc == WC3_WAIT_ABANDONED_0 ? 'm' : '!';
    if (rc == WC3_WAIT_ABANDONED_0 && c->sync->mutex_release(
                                           c->sync->ctx, c->mutex) != 0)
        return -1;
    return 0;
}

static Wc3ThreadHandle sched_spawn(const Wc3ThreadApi *t, int (*entry)(void *),
                                   void *arg, const char *name)
{
    Wc3ThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.name = name;
    desc.stack_size = 128u * 1024u;
    desc.entry = entry;
    desc.arg = arg;
    Wc3ThreadHandle h = 0;
    check(t->create(t->ctx, &desc, &h) == 0, name);
    return h;
}

static void test_scheduler(Wc3Platform *p, const char *expected_yield,
                           const char *expected_wake)
{
    char order[16];
    uint32_t count = 0;
    memset(order, 0, sizeof order);

    SchedCtx a;
    memset(&a, 0, sizeof a);
    a.thread = &p->thread;
    a.order = order;
    a.count = &count;
    a.first = 'A';
    a.second = 'a';
    SchedCtx b = a;
    b.first = 'B';
    b.second = 'b';

    Wc3ThreadHandle ha = sched_spawn(&p->thread, sched_yield_child, &a, "sched-A");
    Wc3ThreadHandle hb = sched_spawn(&p->thread, sched_yield_child, &b, "sched-B");
    if (ha) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, ha, &exit_code) == 0,
              "sched join A");
        check(exit_code == 0u, "sched A exit");
    }
    if (hb) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hb, &exit_code) == 0,
              "sched join B");
        check(exit_code == 0u, "sched B exit");
    }
    check(count == 4u && memcmp(order, expected_yield, 4) == 0,
          "schedule yield order");

    SchedCtx w;
    memset(&w, 0, sizeof w);
    w.thread = &p->thread;
    w.sync = &p->sync;
    w.order = order;
    w.count = &count;
    check(p->sync.event_create(p->sync.ctx, 0, 0, &w.event) == 0,
          "sched wait event");
    SchedCtx s = w;
    Wc3ThreadHandle hw =
        sched_spawn(&p->thread, sched_wait_child, &w, "sched-wait");
    Wc3ThreadHandle hs =
        sched_spawn(&p->thread, sched_signal_child, &s, "sched-signal");
    count = 0;
    memset(order, 0, sizeof order);
    if (hw) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hw, &exit_code) == 0,
              "sched join wait");
        check(exit_code == 0u, "sched wait exit");
    }
    if (hs) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hs, &exit_code) == 0,
              "sched join signal");
        check(exit_code == 0u, "sched signal exit");
    }
    check(count == 1u && order[0] == 'w', "sched wake wait");
    check(p->sync.close(p->sync.ctx, w.event) == 0, "sched wait close");

    /* Both waiters park on the same manual event, then a third thread sets
     * it.  LIFO/WAKE_REORDER must prefer the latest-created/latest-woken
     * waiter; FIFO/ROUND_ROBIN keep creation order. */
    SchedCtx w1;
    memset(&w1, 0, sizeof w1);
    w1.thread = &p->thread;
    w1.sync = &p->sync;
    w1.order = order;
    w1.count = &count;
    w1.first = '1';
    check(p->sync.event_create(p->sync.ctx, 1, 0, &w1.event) == 0,
          "sched wake order event");
    SchedCtx w2 = w1;
    w2.first = '2';
    SchedCtx setter = w1;
    Wc3ThreadHandle hw1 =
        sched_spawn(&p->thread, sched_wait_char_child, &w1, "sched-w1");
    Wc3ThreadHandle hw2 =
        sched_spawn(&p->thread, sched_wait_char_child, &w2, "sched-w2");
    Wc3ThreadHandle hset =
        sched_spawn(&p->thread, sched_signal_child, &setter, "sched-setter");
    count = 0;
    memset(order, 0, sizeof order);
    if (hw1) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hw1, &exit_code) == 0,
              "sched w1 join");
        check(exit_code == 0u, "sched w1 exit");
    }
    if (hw2) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hw2, &exit_code) == 0,
              "sched w2 join");
        check(exit_code == 0u, "sched w2 exit");
    }
    if (hset) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hset, &exit_code) == 0,
              "sched setter join");
        check(exit_code == 0u, "sched setter exit");
    }
    check(count == 2u && memcmp(order, expected_wake, 2) == 0,
          "sched wake order");
    check(p->sync.close(p->sync.ctx, w1.event) == 0,
          "sched wake order close");

    SchedCtx to;
    memset(&to, 0, sizeof to);
    to.thread = &p->thread;
    to.sync = &p->sync;
    to.time = &p->time;
    to.order = order;
    to.count = &count;
    to.timeout_ms = 5u;
    check(p->sync.event_create(p->sync.ctx, 0, 0, &to.event) == 0,
          "sched timeout event");
    Wc3ThreadHandle ht =
        sched_spawn(&p->thread, sched_timeout_child, &to, "sched-timeout");
    count = 0;
    memset(order, 0, sizeof order);
    if (ht) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, ht, &exit_code) == 0,
              "sched join timeout");
        check(exit_code == 0u, "sched timeout exit");
    }
    check(count == 1u && order[0] == 't', "sched virtual timeout");
    check(to.elapsed_ns == 5000000ull, "sched timeout duration");
    check(p->sync.close(p->sync.ctx, to.event) == 0,
          "sched timeout close");

    /* A thread exit wakes sync waiters spuriously.  The timeout must keep its
     * original deadline instead of restarting from the wake-up point. */
    SchedCtx slow;
    memset(&slow, 0, sizeof slow);
    slow.thread = &p->thread;
    slow.sync = &p->sync;
    slow.time = &p->time;
    slow.order = order;
    slow.count = &count;
    slow.timeout_ms = 10u;
    check(p->sync.event_create(p->sync.ctx, 0, 0, &slow.event) == 0,
          "sched deadline event");
    Wc3ThreadHandle hslow =
        sched_spawn(&p->thread, sched_timeout_child, &slow, "sched-deadline");
    Wc3ThreadHandle hexit =
        sched_spawn(&p->thread, sched_exit_child, &slow, "sched-exit");
    count = 0;
    memset(order, 0, sizeof order);
    if (hslow) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hslow, &exit_code) == 0,
              "sched deadline join");
        check(exit_code == 0u, "sched deadline exit");
    }
    if (hexit) {
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, hexit, &exit_code) == 0,
              "sched exit join");
        check(exit_code == 0u, "sched exit code");
    }
    check(count == 1u && order[0] == 't',
          "sched timeout survives spurious wake");
    check(slow.elapsed_ns == 10000000ull, "sched deadline not extended");
    check(p->sync.close(p->sync.ctx, slow.event) == 0,
          "sched deadline close");

    /* A sync waiter must observe mutex abandonment before the exiting owner
     * hands the token away.  Waiting for `acquired` first keeps this test
     * independent of the selected schedule policy. */
    SchedMutexCtx mx;
    memset(&mx, 0, sizeof mx);
    mx.thread = &p->thread;
    mx.sync = &p->sync;
    mx.order = order;
    mx.count = &count;
    check(p->sync.mutex_create(p->sync.ctx, 0, &mx.mutex) == 0,
          "sched abandon mutex create");
    check(p->sync.event_create(p->sync.ctx, 0, 0, &mx.acquired) == 0,
          "sched abandon event create");
    Wc3ThreadHandle ho =
        sched_spawn(&p->thread, sched_mutex_owner_child, &mx, "sched-owner");
    if (ho) {
        check(p->sync.wait(p->sync.ctx, &mx.acquired, 1, 0, 100u, NULL) ==
                  WC3_WAIT_OBJECT_0,
              "sched owner acquired");
        Wc3ThreadHandle hm = sched_spawn(
            &p->thread, sched_mutex_waiter_child, &mx, "sched-waiter");
        count = 0;
        memset(order, 0, sizeof order);
        if (hm) {
            uint32_t exit_code = 1u;
            check(p->thread.join(p->thread.ctx, hm, &exit_code) == 0,
                  "sched waiter join");
            check(exit_code == 0u, "sched waiter exit");
        }
        uint32_t exit_code = 1u;
        check(p->thread.join(p->thread.ctx, ho, &exit_code) == 0,
              "sched owner join");
        check(exit_code == 0u, "sched owner exit");
        check(count == 1u && order[0] == 'm',
              "sched waiter sees abandoned mutex");
    }
    check(p->sync.close(p->sync.ctx, mx.acquired) == 0,
          "sched abandon event close");
    check(p->sync.close(p->sync.ctx, mx.mutex) == 0,
          "sched abandon mutex close");
}

static uint64_t run_scheduler_policy(Wc3Platform *p,
                                     const Wc3HeadlessScheduleConfig *base,
                                     uint32_t policy, const char *id,
                                     const char *expected_yield,
                                     const char *expected_wake)
{
    Wc3HeadlessScheduleConfig schedule = *base;
    schedule.policy = policy;
    schedule.id = id;
    schedule.mailbox_delay_ns = 0u;
    schedule.input_script = NULL;
    schedule.input_count = 0u;

    Wc3HeadlessConfig config;
    memset(&config, 0, sizeof config);
    config.schedule = &schedule;
    check(wc3_headless_platform_init(p, &config) == 0, id);
    check(wc3_headless_schedule_policy() == (int)policy, "policy value");
    const uint64_t hash = wc3_headless_schedule_hash();
    check(hash != 0u, "policy hash");
    test_scheduler(p, expected_yield, expected_wake);
    wc3_headless_platform_shutdown(p);
    check(wc3_headless_schedule_id() == NULL, "policy id cleared");
    return hash;
}

static void test_virtual_schedule(void)
{
    static const Wc3HeadlessScriptEvent script[] = {
        {100000000ull, {.kind = WC3_INPUT_KEY_DOWN, .key = 1u}},
        {200000000ull, {.kind = WC3_INPUT_KEY_DOWN, .key = 2u}},
    };
    static const Wc3HeadlessConfigEntry entries[] = {
        {"WC3_VIRTUAL", "1"},
    };

    Wc3HeadlessScheduleConfig schedule;
    memset(&schedule, 0, sizeof schedule);
    schedule.policy = WC3_HEADLESS_SCHED_ROUND_ROBIN;
    schedule.seed = 0x12345678u;
    schedule.id = "t1-rr-delay";
    schedule.epoch_unix_ns = 1700000000000000000ull;
    schedule.timezone_minutes = 180;
    schedule.mailbox_delay_ns = 10000000u;
    schedule.input_script = script;
    schedule.input_count = (uint32_t)(sizeof script / sizeof script[0]);

    Wc3HeadlessConfig config;
    memset(&config, 0, sizeof config);
    config.entries = entries;
    config.entry_count = (uint32_t)(sizeof entries / sizeof entries[0]);
    config.schedule = &schedule;

    Wc3Platform p;
    memset(&p, 0, sizeof p);
    check(wc3_headless_platform_init(&p, &config) == 0,
          "virtual platform init");
    check(wc3_headless_schedule_policy() ==
              WC3_HEADLESS_SCHED_ROUND_ROBIN,
          "schedule policy");
    check(wc3_headless_schedule_seed() == 0x12345678u, "schedule seed");
    check(wc3_headless_schedule_id() &&
              strcmp(wc3_headless_schedule_id(), "t1-rr-delay") == 0,
          "schedule id");
    const uint64_t hash = wc3_headless_schedule_hash();
    check(hash != 0u, "schedule hash");
    check(wc3_headless_schedule_hash() == hash, "schedule hash stable");

    check(p.time.monotonic_ns(p.time.ctx) == 0u, "virtual monotonic zero");
    check(p.time.realtime_ns(p.time.ctx) == 1700000000000000000ull,
          "virtual realtime epoch");
    int32_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    check(p.time.local_time(p.time.ctx, 1700000000000000000ull, &y, &mo, &d,
                            &h, &mi, &s, &ms) == 0,
          "virtual local time");
    check(y == 2023 && mo == 11 && d == 15 && h == 1 && mi == 13 && s == 20 &&
              ms == 0,
          "virtual local time fixed offset");

    Wc3InputEvent event;
    check(wc3_headless_input_pop(&event) == 0, "script not due at zero");
    check(wc3_headless_clock_advance_ns(105000000ull) == 0,
          "advance below first release");
    check(p.time.monotonic_ns(p.time.ctx) == 105000000ull,
          "virtual monotonic value");
    check(wc3_headless_input_pop(&event) == 0, "script delay holds event");
    check(wc3_headless_clock_advance_ns(5000000ull) == 0,
          "advance to first release");
    check(wc3_headless_input_pop(&event) == 1, "script first event");
    check(event.kind == WC3_INPUT_KEY_DOWN && event.key == 1u,
          "script first event value");
    check(event.time_ns == 110000000ull, "script first event timestamp");
    check(wc3_headless_input_pop(&event) == 0, "script first consumed");

    check(wc3_headless_clock_advance_ns(99000000ull) == 0,
          "advance below second release");
    check(wc3_headless_input_pop(&event) == 0, "script second holds");
    check(wc3_headless_clock_advance_ns(1000000ull) == 0,
          "advance to second release");
    check(wc3_headless_input_pop(&event) == 1, "script second event");
    check(event.kind == WC3_INPUT_KEY_DOWN && event.key == 2u,
          "script second event value");
    check(event.time_ns == 210000000ull, "script second event timestamp");

    check(p.thread.sleep_ns(p.thread.ctx, 50000000ull) == 0,
          "virtual sleep");
    check(p.time.monotonic_ns(p.time.ctx) == 260000000ull,
          "virtual sleep advanced clock");
    const uint64_t before = p.time.monotonic_ns(p.time.ctx);
    check(wc3_headless_clock_advance_ns(UINT64_MAX) == -1,
          "virtual clock overflow rejected");
    check(p.time.monotonic_ns(p.time.ctx) == before,
          "clock unchanged after overflow");
    check(wc3_headless_clock_advance_ns(0u) == 0, "zero advance");
    check(p.config.get_int(p.config.ctx, "WC3_VIRTUAL", 0) == 1,
          "virtual config entry");

    test_scheduler(&p, "ABab", "12");

    wc3_headless_platform_shutdown(&p);
    check(wc3_headless_schedule_id() == NULL, "first schedule id cleared");

    const uint64_t fifo_hash = run_scheduler_policy(
        &p, &schedule, WC3_HEADLESS_SCHED_FIFO, "t1-fifo", "ABab", "12");
    check(fifo_hash != hash, "fifo hash differs");
    const uint64_t lifo_hash = run_scheduler_policy(
        &p, &schedule, WC3_HEADLESS_SCHED_LIFO, "t1-lifo", "BAba", "21");
    check(lifo_hash != hash && lifo_hash != fifo_hash,
          "lifo hash differs");
    const uint64_t wake_hash = run_scheduler_policy(
        &p, &schedule, WC3_HEADLESS_SCHED_WAKE_REORDER, "t1-wake", "BAba",
        "21");
    check(wake_hash != hash && wake_hash != fifo_hash &&
              wake_hash != lifo_hash,
          "wake hash differs");

    check(wc3_headless_schedule_id() == NULL, "schedule id cleared");
    check(wc3_headless_schedule_hash() == 0u, "schedule hash cleared");
    check(wc3_headless_clock_advance_ns(1u) == -1, "clock cleared");
}

int main(void)
{
    static const Wc3HeadlessConfigEntry entries[] = {
        {"WC3_HEADLESS_TEST", "42"},
        {"WC3_HEADLESS_BAD", "abc"},
    };

    Wc3Platform p;
    Wc3HeadlessConfig config;
    memset(&config, 0, sizeof config);
    config.width = 800;
    config.height = 600;
    config.entries = entries;
    config.entry_count = (uint32_t)(sizeof entries / sizeof entries[0]);
    check(wc3_headless_platform_init(&p, &config) == 0, "platform init");
    check(wc3_headless_platform_init(&p, &config) == -1,
          "platform init once");
    check(p.mem.reserve && p.file.open && p.file.stat && p.time.monotonic_ns &&
              p.thread.create && p.sync.wait && p.input.push &&
              p.input.set_text_focus && p.surface.acquire && p.audio.open &&
              p.audio.set_callback && p.audio.set_consumed &&
              p.audio.consumed_bytes && p.audio.advance &&
              p.audio.latency_frames && p.audio.publish &&
              p.config.get && p.asset.init &&
              p.window.create && p.clipboard.get_text && p.locale.to_wide,
          "platform table");

    test_memory(&p.mem);
    test_file(&p.file);
    test_time(&p.time);
    test_thread(&p.thread);
    test_input(&p.input);
    test_sync(&p.sync);
    test_abandoned_mutex(&p.sync, &p.thread);
    test_window(&p.window);
    test_clipboard(&p.clipboard);
    test_locale(&p.locale);
    test_config(&p.config);
    test_asset(&p.asset);
    test_audio_surface(&p);
    test_audio_pull(&p);
    p.log.write(p.log.ctx, 4, "wc3-headless", "platform test complete");

    check(p.vulkan_driver.select(p.vulkan_driver.ctx, 0) == -1,
          "vulkan absent");
    check(p.vulkan_driver.device_caps(p.vulkan_driver.ctx, 0, NULL) == -1,
          "vulkan caps absent");
    test_guest_memory(&p);

    wc3_headless_platform_shutdown(&p);
    check(p.mem.reserve == NULL && p.file.open == NULL, "platform cleared");
    Wc3InputEvent event;
    check(wc3_headless_input_pop(&event) == -1, "input after shutdown");

    test_virtual_schedule();

    static const Wc3HeadlessConfigEntry duplicate[] = {
        {"WC3_DUP", "1"},
        {"WC3_DUP", "2"},
    };
    Wc3HeadlessConfig bad;
    memset(&bad, 0, sizeof bad);
    bad.entries = duplicate;
    bad.entry_count = 2;
    check(wc3_headless_platform_init(&p, &bad) == -1,
          "duplicate config rejected");
    check(p.mem.reserve == NULL, "failed init leaves table cleared");

    if (g_failures) {
        printf("platform headless: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("platform headless: OK\n");
    return 0;
}
