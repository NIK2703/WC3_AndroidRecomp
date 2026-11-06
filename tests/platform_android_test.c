/*
 * platform_android_test.c - device-side check of the Android OS boundary.
 *
 * This is deliberately a standalone arm64 executable, not an APK.  It proves
 * that the core platform table works on the target ABI before any guest code
 * is allowed to depend on it.  Build and run with
 * tools/android/run_platform_test.ps1.
 */
#define _GNU_SOURCE 1

#include "guest_layout.h"
#include "guest_memory.h"
#include "pe.h"
#include "platform_android.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    Wc3ThreadApi *t = (Wc3ThreadApi *)arg;
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
    if (m->reserve(m->ctx, base, size) != 0) {
        printf("WARN memory reserve at %08X failed (errno=%d)\n",
               (unsigned)base, errno);
        return;
    }
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

static void test_guest_memory(void)
{
    check(wc3_guest_memory_init(NULL) == -1, "guest memory rejects NULL");
    const Wc3Platform *p = wc3_platform();
    check(p != NULL, "platform installed");
    if (!p) return;

    check(wc3_guest_memory_init(p) == 0, "guest memory init");
    check(wc3_guest_memory_init(p) == -1, "guest memory init once");

    check(wc3_guest_ok(GUEST_STACK_BASE, 0x1000) == 1, "guest stack ok");
    check(wc3_guest_ok(GUEST_HEAP_BASE, 0x1000) == 1, "guest heap ok");
    check(wc3_guest_ok(GUEST_HLE_HEAP_BASE, 0x1000) == 1, "hle heap ok");
    check(wc3_guest_ok(GUEST_TEB_BASE, 0x1000) == 1, "main TEB ok");
    check(wc3_guest_ok(GUEST_THREAD_TEB_BASE, 0x1000) == 1,
          "worker TEB ok");
    check(wc3_guest_ok(0xFFFFFFF0u, 0x20) == 0, "guest wrap rejected");
    check(wc3_guest_ok(0x1000u, 0x10) == 0, "unmapped guest rejected");

    volatile uint32_t *stack = (volatile uint32_t *)(uintptr_t)GUEST_STACK_BASE;
    stack[0] = 0x51525354u;
    check(stack[0] == 0x51525354u, "guest stack write");
    check(wc3_guest_clamp(GUEST_STACK_BASE + GUEST_STACK_SIZE - 16u, 64u) ==
              16u,
          "guest clamp");

    const uint32_t dynamic_base = GUEST_VA_BASE;
    const uint32_t dynamic_size = 0x10000u;
    check(wc3_guest_range_ok(dynamic_base, dynamic_size) == 0,
          "dynamic range absent");
    check(wc3_guest_map(dynamic_base, dynamic_size) == 0, "guest map");
    check(wc3_guest_range_ok(dynamic_base, dynamic_size) == 1,
          "dynamic range present");
    volatile uint32_t *dyn = (volatile uint32_t *)(uintptr_t)dynamic_base;
    dyn[0] = 0x01020304u;
    check(dyn[0] == 0x01020304u, "dynamic guest write");
    check(wc3_guest_decommit(dynamic_base, dynamic_size) == 0,
          "guest decommit");
    check(wc3_guest_range_ok(dynamic_base, dynamic_size) == 0,
          "dynamic range removed");
    check(wc3_guest_commit(dynamic_base, dynamic_size) == 0, "guest commit");
    check(wc3_guest_range_ok(dynamic_base, dynamic_size) == 1,
          "dynamic range restored");
    check(wc3_guest_unmap(dynamic_base, dynamic_size) == 0, "guest unmap");
    check(wc3_guest_range_ok(dynamic_base, dynamic_size) == 0,
          "dynamic range gone");
}

static uint32_t dummy_import(const char *dll, const char *name,
                             uint16_t ordinal, void *user)
{
    (void)dll;
    (void)name;
    (void)ordinal;
    (void)user;
    return 0x7F000000u;
}

static void test_pe(const Wc3Platform *p, const char *path,
                    uint32_t expected_base, int bind)
{
    Wc3PeImage image;
    memset(&image, 0, sizeof image);
    check(wc3_pe_load(p, path, 0, &image, 0) == 0, "pe load");
    if (!image.base) return;

    check(image.base == expected_base, "pe base");
    check(image.span != 0u, "pe span");
    check(wc3_guest_ok(image.base, image.span) == 1, "pe range mapped");
    check(wc3_rd16g(image.base) == 0x5A4Du, "pe DOS magic");
    uint32_t lfanew = wc3_rd32g(image.base + 0x3Cu);
    check(lfanew != 0u && lfanew < image.span, "pe e_lfanew");
    check(wc3_rd32g(image.base + lfanew) == 0x00004550u, "pe NT magic");
    check(image.entry >= image.base && image.entry < image.base + image.span,
          "pe entry in image");
    check(image.import_rva != 0u, "pe import table");
    if (bind) {
        check(wc3_pe_bind_imports(&image, dummy_import, NULL) == 0,
              "pe imports bound");
    }
    if (expected_base == GUEST_IMAGE_GAME) {
        uint32_t game_main = wc3_pe_export(&image, "GameMain");
        check(game_main >= image.base &&
                  game_main < image.base + image.span,
              "pe export GameMain");
        check(wc3_pe_export_ordinal(&image, 1) == game_main,
              "pe export ordinal");
        check(wc3_pe_export(&image, "NoSuchExport") == 0,
              "pe missing export");
        check(wc3_pe_export_ordinal(&image, 0) == 0,
              "pe missing ordinal");
    }
    (void)wc3_guest_unmap(image.base, image.span);
}

static int first_highlow_relocation(const Wc3PeImage *image,
                                    uint32_t *target_rva)
{
    if (!image || !image->reloc_rva || image->reloc_size < 8u) return 0;
    uint32_t off = 0;
    while (off + 8u <= image->reloc_size) {
        uint32_t block_rva = wc3_rd32g(image->base + image->reloc_rva + off);
        uint32_t block_size = wc3_rd32g(image->base + image->reloc_rva + off + 4u);
        if (block_size < 8u || (block_size & 1u) != 0u ||
            block_size > image->reloc_size - off)
            return 0;
        uint32_t count = (block_size - 8u) / 2u;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t entry = wc3_rd16g(image->base + image->reloc_rva + off +
                                       8u + i * 2u);
            if ((entry >> 12) != 3u) continue;
            uint32_t target = block_rva + (entry & 0x0FFFu);
            if (target > image->span || image->span - target < 4u) return 0;
            *target_rva = target;
            return 1;
        }
        off += block_size;
    }
    return 0;
}

static void test_pe_relocate(const Wc3Platform *p, const char *path)
{
    Wc3PeImage original;
    Wc3PeImage moved;
    memset(&original, 0, sizeof original);
    memset(&moved, 0, sizeof moved);

    check(wc3_pe_load(p, path, 0, &original, 0) == 0,
          "reloc original load");
    if (!original.base) return;

    const uint32_t moved_base = 0x71000000u;
    check(wc3_pe_load(p, path, moved_base, &moved, 1) == 0,
          "reloc moved load");
    if (!moved.base) {
        (void)wc3_guest_unmap(original.base, original.span);
        return;
    }

    check(moved.base == moved_base, "reloc moved base");
    uint32_t target = 0;
    check(first_highlow_relocation(&original, &target) == 1,
          "reloc original has HIGHLOW");
    uint32_t moved_target = 0;
    check(first_highlow_relocation(&moved, &moved_target) == 1,
          "reloc moved has HIGHLOW");
    if (target && target == moved_target) {
        uint32_t before = wc3_rd32g(original.base + target);
        uint32_t after = wc3_rd32g(moved.base + target);
        check(after - before == moved.base - original.base,
              "reloc HIGHLOW applied");
    }

    (void)wc3_guest_unmap(moved.base, moved.span);
    (void)wc3_guest_unmap(original.base, original.span);
}

static uint8_t *read_whole_file(const char *path, size_t *out_len)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length <= 0 || (uint64_t)length > 256u * 1024u * 1024u ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    size_t size = (size_t)length;
    uint8_t *data = malloc(size);
    if (!data) {
        (void)fclose(file);
        return NULL;
    }
    size_t got = fread(data, 1, size, file);
    (void)fclose(file);
    if (got != size) {
        free(data);
        return NULL;
    }
    *out_len = size;
    return data;
}

static int write_whole_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t written = fwrite(data, 1, size, file);
    int closed = fclose(file);
    return written == size && closed == 0 ? 0 : -1;
}

static void test_pe_bad(const Wc3Platform *p, const char *path)
{
    size_t len = 0;
    uint8_t *source = read_whole_file(path, &len);
    check(source != NULL, "pe bad source read");
    if (!source) return;

    const pe_dos_header *dos = (const pe_dos_header *)source;
    int header_ok = len >= sizeof(pe_dos_header) && dos->e_magic == 0x5A4Du &&
                    dos->e_lfanew >= 0 &&
                    (size_t)dos->e_lfanew + sizeof(pe_nt_headers32) <= len;
    check(header_ok, "pe bad source header");
    if (!header_ok) {
        free(source);
        return;
    }

    uint8_t *work = malloc(len);
    check(work != NULL, "pe bad work alloc");
    if (!work) {
        free(source);
        return;
    }

    size_t nt_off = (size_t)dos->e_lfanew;
    const char *bad_path = "/data/local/tmp/wc3-bad.exe";
    Wc3PeImage image;

    memcpy(work, source, len);
    check(write_whole_file(bad_path, work, len) == 0, "pe bad write good");
    memset(&image, 0, sizeof image);
    check(wc3_pe_load(p, bad_path, 0, &image, 0) == 0, "pe bad good load");
    if (image.base) {
        if (image.import_rva) {
            wc3_wr32g(image.base + image.import_rva, 0xFFFFFFFFu);
            check(wc3_pe_bind_imports(&image, dummy_import, NULL) == -1,
                  "pe bad import descriptor");
        }
        (void)wc3_guest_unmap(image.base, image.span);
    }

    memcpy(work, source, len);
    ((pe_nt_headers32 *)(void *)(work + nt_off))->FileHeader.Machine = 0x01C0u;
    check(write_whole_file(bad_path, work, len) == 0, "pe bad write machine");
    check(wc3_pe_load(p, bad_path, 0, &image, 0) == -1, "pe bad machine");
    check(wc3_guest_range_ok(GUEST_IMAGE_WAR3, 0x1000u) == 0,
          "pe bad machine unmapped");

    memcpy(work, source, len);
    ((pe_nt_headers32 *)(void *)(work + nt_off))->OptionalHeader.SizeOfImage = 0;
    check(write_whole_file(bad_path, work, len) == 0, "pe bad write image");
    check(wc3_pe_load(p, bad_path, 0, &image, 0) == -1, "pe bad image size");

    memcpy(work, source, len);
    ((pe_nt_headers32 *)(void *)(work + nt_off))->OptionalHeader.SizeOfHeaders =
        0xFFFFFFFFu;
    check(write_whole_file(bad_path, work, len) == 0, "pe bad write headers");
    check(wc3_pe_load(p, bad_path, 0, &image, 0) == -1, "pe bad headers");

    memcpy(work, source, len);
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(void *)(work + nt_off);
    pe_section_header *sections =
        (pe_section_header *)(void *)((uint8_t *)&nt->OptionalHeader +
                                      nt->FileHeader.SizeOfOptionalHeader);
    int section_ok =
        (uint8_t *)(sections + 1) <= work + len && nt->FileHeader.NumberOfSections;
    check(section_ok, "pe bad source section");
    if (section_ok) {
        sections[0].VirtualSize = 0xFFFFFFFFu;
        check(write_whole_file(bad_path, work, len) == 0,
              "pe bad write section");
        check(wc3_pe_load(p, bad_path, 0, &image, 0) == -1,
              "pe bad section span");
        check(wc3_guest_range_ok(GUEST_IMAGE_WAR3, 0x1000u) == 0,
              "pe bad section unmapped");
    }

    memcpy(work, source, len);
    ((pe_dos_header *)(void *)work)->e_lfanew = (int32_t)(len - 2u);
    check(write_whole_file(bad_path, work, len) == 0, "pe bad write lfanew");
    check(wc3_pe_load(p, bad_path, 0, &image, 0) == -1, "pe bad lfanew");

    (void)unlink(bad_path);
    free(work);
    free(source);
}

static void test_file(const Wc3FileApi *f)
{
    const char *path = "/data/local/tmp/wc3_platform_test.bin";
    (void)unlink(path);
    uintptr_t h = 0;
    check(f->open(f->ctx, path,
                  WC3_FILE_READ | WC3_FILE_WRITE | WC3_FILE_CREATE |
                      WC3_FILE_TRUNCATE,
                  &h) == 0,
          "file open create");
    if (!h) return;
    const char *text = "wc3-platform";
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
    char buf[32] = {0};
    uint32_t got = 0;
    check(f->read(f->ctx, h, buf, sizeof buf, &got) == 0, "file read");
    check(got == strlen(text), "file read length");
    check(memcmp(buf, text, strlen(text)) == 0, "file content");
    uint64_t pos = 0;
    check(f->seek(f->ctx, h, 0, 0, &pos) == 0, "file seek set");
    check(pos == 0, "file seek position");
    check(f->close(f->ctx, h) == 0, "file close read");
    (void)unlink(path);
}

static void test_time(const Wc3TimeApi *t)
{
    uint64_t mono = t->monotonic_ns(t->ctx);
    uint64_t real = t->realtime_ns(t->ctx);
    check(mono != 0, "monotonic clock");
    check(real > 1700000000000000000ull, "realtime clock");
    int32_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    check(t->local_time(t->ctx, real, &y, &mo, &d, &h, &mi, &s, &ms) == 0,
          "local time");
    check(y >= 2024 && y <= 2100, "local year");
    check(mo >= 1 && mo <= 12, "local month");
    check(d >= 1 && d <= 31, "local day");
    check(h >= 0 && h <= 23, "local hour");
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
    desc.name = "wc3-test";
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

typedef struct InputProducer {
    const Wc3InputApi *input;
    uint32_t count;
    _Atomic int done;
} InputProducer;

static int input_producer_main(void *arg)
{
    InputProducer *producer = (InputProducer *)arg;
    for (uint32_t i = 0; i < producer->count; i++) {
        Wc3InputEvent event;
        memset(&event, 0, sizeof event);
        event.kind = WC3_INPUT_KEY_DOWN;
        event.key = i + 1u;
        event.time_ns = 1u;
        while (producer->input->push(producer->input->ctx, &event) != 0)
            usleep(100);
    }
    atomic_store_explicit(&producer->done, 1, memory_order_release);
    return 0;
}

static void test_input(const Wc3InputApi *in, const Wc3ThreadApi *threads)
{
    check(in && in->push && in->set_text_focus, "input table");

    Wc3InputEvent out;
    check(wc3_android_input_pop(NULL) == -1, "input pop rejects null");
    memset(&out, 0xEE, sizeof out);
    check(wc3_android_input_pop(&out) == 0, "input empty");
    check(wc3_android_input_dropped() == 0u, "input initial dropped");

    Wc3InputEvent event;
    memset(&event, 0, sizeof event);
    event.kind = WC3_INPUT_POINTER_DOWN;
    event.pointer_id = 7u;
    event.x = -12;
    event.y = 345;
    event.modifiers = 0x4u;
    check(in->push(in->ctx, &event) == 0, "input push pointer");
    check(wc3_android_input_pop(&out) == 1, "input pop pointer");
    check(out.kind == WC3_INPUT_POINTER_DOWN, "input pointer kind");
    check(out.pointer_id == 7u, "input pointer id");
    check(out.x == -12 && out.y == 345, "input pointer coords");
    check(out.modifiers == 0x4u, "input pointer modifiers");
    check(out.time_ns != 0u, "input timestamp filled");

    memset(&event, 0, sizeof event);
    event.kind = WC3_INPUT_KEY_UP;
    event.key = 0x1B;
    event.time_ns = 0x123456789ull;
    check(in->push(in->ctx, &event) == 0, "input push key");
    check(wc3_android_input_pop(&out) == 1, "input pop key");
    check(out.kind == WC3_INPUT_KEY_UP, "input key kind");
    check(out.key == 0x1Bu, "input key code");
    check(out.time_ns == 0x123456789ull, "input timestamp preserved");

    memset(&event, 0, sizeof event);
    check(in->push(in->ctx, &event) == -1, "input rejects bad kind");
    check(in->push(in->ctx, NULL) == -1, "input rejects null event");
    check(wc3_android_input_dropped() == 0u, "input invalid does not drop");

    const uint32_t capacity = 1024u;
    for (uint32_t i = 0; i < capacity; i++) {
        memset(&event, 0, sizeof event);
        event.kind = WC3_INPUT_KEY_DOWN;
        event.key = i + 1u;
        event.time_ns = 1u;
        check(in->push(in->ctx, &event) == 0, "input fill push");
    }
    check(wc3_android_input_dropped() == 0u, "input fill no drops");
    memset(&event, 0, sizeof event);
    event.kind = WC3_INPUT_KEY_DOWN;
    check(in->push(in->ctx, &event) == -1, "input full rejects");
    check(wc3_android_input_dropped() == 1u, "input full counts drop");

    int order_ok = 1;
    for (uint32_t i = 0; i < capacity; i++) {
        if (wc3_android_input_pop(&out) != 1 ||
            out.kind != WC3_INPUT_KEY_DOWN || out.key != i + 1u)
            order_ok = 0;
    }
    check(order_ok, "input order preserved");
    check(wc3_android_input_pop(&out) == 0, "input drained");
    check(in->set_text_focus(in->ctx, 1) == 0, "input text focus");

    InputProducer producer;
    memset(&producer, 0, sizeof producer);
    producer.input = in;
    producer.count = 4096u;
    atomic_init(&producer.done, 0);

    Wc3ThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.name = "wc3-input";
    desc.stack_size = 128u * 1024u;
    desc.entry = input_producer_main;
    desc.arg = &producer;
    Wc3ThreadHandle thread = 0;
    check(threads->create(threads->ctx, &desc, &thread) == 0,
          "input producer create");
    if (thread) {
        uint32_t expected = 1u;
        uint32_t popped = 0u;
        int concurrent_ok = 1;
        for (;;) {
            int rc = wc3_android_input_pop(&out);
            if (rc == 1) {
                if (out.kind != WC3_INPUT_KEY_DOWN ||
                    out.key != expected)
                    concurrent_ok = 0;
                expected++;
                popped++;
                continue;
            }
            if (rc < 0) {
                concurrent_ok = 0;
                break;
            }
            if (atomic_load_explicit(&producer.done, memory_order_acquire))
                break;
            usleep(100);
        }
        check(concurrent_ok, "input concurrent order");
        check(popped == producer.count, "input concurrent count");
        uint32_t exit_code = 0;
        check(threads->join(threads->ctx, thread, &exit_code) == 0,
              "input producer join");
        check(exit_code == 0u, "input producer exit code");
    }
}

static void test_sync(const Wc3SyncApi *s)
{
    Wc3SyncHandle ev = 0;
    check(s->event_create(s->ctx, 0, 0, &ev) == 0, "event create");
    uint32_t index = 0xFFFFFFFFu;
    check(s->wait(s->ctx, &ev, 1, 0, 30, &index) ==
              (int)WC3_WAIT_TIMEOUT,
          "auto event timeout");
    check(s->event_set(s->ctx, ev) == 0, "auto event set");
    check(s->wait(s->ctx, &ev, 1, 0, 0, &index) ==
              (int)WC3_WAIT_OBJECT_0,
          "auto event waits");
    check(index == 0u, "auto event index");
    check(s->wait(s->ctx, &ev, 1, 0, 0, &index) ==
              (int)WC3_WAIT_TIMEOUT,
          "auto event consumed");
    check(s->event_set(s->ctx, ev) == 0, "auto event set 2");
    check(s->event_reset(s->ctx, ev) == 0, "auto event reset");
    check(s->close(s->ctx, ev) == 0, "auto event close");

    Wc3SyncHandle manual = 0;
    check(s->event_create(s->ctx, 1, 1, &manual) == 0, "manual event create");
    check(s->wait(s->ctx, &manual, 1, 0, 0, &index) ==
              (int)WC3_WAIT_OBJECT_0,
          "manual event wait 1");
    check(s->wait(s->ctx, &manual, 1, 0, 0, &index) ==
              (int)WC3_WAIT_OBJECT_0,
          "manual event wait 2");
    check(s->close(s->ctx, manual) == 0, "manual event close");

    Wc3SyncHandle mutex = 0;
    check(s->mutex_create(s->ctx, 0, &mutex) == 0, "mutex create");
    check(s->wait(s->ctx, &mutex, 1, 0, 0, &index) ==
              (int)WC3_WAIT_OBJECT_0,
          "mutex wait");
    check(s->mutex_release(s->ctx, mutex) == 0, "mutex release");
    check(s->close(s->ctx, mutex) == 0, "mutex close");

    Wc3SyncHandle sem = 0;
    int32_t previous = -1;
    check(s->semaphore_create(s->ctx, 1, 3, &sem) == 0, "semaphore create");
    check(s->wait(s->ctx, &sem, 1, 0, 0, &index) ==
              (int)WC3_WAIT_OBJECT_0,
          "semaphore wait");
    check(s->semaphore_release(s->ctx, sem, 2, &previous) == 0,
          "semaphore release");
    check(previous == 0, "semaphore previous");
    check(s->close(s->ctx, sem) == 0, "semaphore close");

    Wc3SyncHandle cs = 0;
    check(s->critical_section_create(s->ctx, &cs) == 0, "cs create");
    check(s->critical_section_enter(s->ctx, cs) == 0, "cs enter");
    check(s->critical_section_try_enter(s->ctx, cs) == 1, "cs recursive try");
    check(s->critical_section_leave(s->ctx, cs) == 0, "cs leave 1");
    check(s->critical_section_leave(s->ctx, cs) == 0, "cs leave 2");
    check(s->close(s->ctx, cs) == 0, "cs close");
}

typedef struct AbandonArg {
    const Wc3SyncApi *sync;
    Wc3SyncHandle mutex;
    int acquired;
} AbandonArg;

static int abandon_main(void *arg)
{
    AbandonArg *a = (AbandonArg *)arg;
    uint32_t index = 0xFFFFFFFFu;
    int rc = a->sync->wait(a->sync->ctx, &a->mutex, 1, 0, 0, &index);
    if (rc == (int)WC3_WAIT_OBJECT_0 && index == 0u) a->acquired = 1;
    return 0;
}

static void test_abandoned_mutex(const Wc3SyncApi *s,
                                 const Wc3ThreadApi *t)
{
    Wc3SyncHandle mutex = 0;
    check(s->mutex_create(s->ctx, 0, &mutex) == 0,
          "abandoned mutex create");
    if (!mutex) return;

    AbandonArg arg;
    memset(&arg, 0, sizeof arg);
    arg.sync = s;
    arg.mutex = mutex;

    Wc3ThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.name = "wc3-abandon";
    desc.stack_size = 128u * 1024u;
    desc.entry = abandon_main;
    desc.arg = &arg;

    Wc3ThreadHandle thread = 0;
    check(t->create(t->ctx, &desc, &thread) == 0, "abandon thread create");
    if (thread) {
        uint32_t exit_code = 0;
        check(t->join(t->ctx, thread, &exit_code) == 0,
              "abandon thread join");
        check(exit_code == 0u, "abandon thread exit code");
    }
    check(arg.acquired == 1, "abandon thread acquired");

    uint32_t index = 0xFFFFFFFFu;
    int rc = s->wait(s->ctx, &mutex, 1, 0, 0, &index);
    check(rc == (int)WC3_WAIT_ABANDONED_0, "abandoned mutex wait");
    check(index == 0u, "abandoned mutex index");
    check(s->mutex_release(s->ctx, mutex) == 0,
          "abandoned mutex release");
    check(s->close(s->ctx, mutex) == 0, "abandoned mutex close");
}

static void test_asset(const Wc3AssetApi *a)
{
    const char *dir = "/data/local/tmp/wc3-platform-assets";
    (void)mkdir(dir, 0700);
    const char *mpq = "/data/local/tmp/wc3-platform-assets/war3.mpq";
    int fd = open(mpq, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) close(fd);

    check(a->init(a->ctx, dir, "/data/local/tmp") == 0, "asset init");
    check(a->present(a->ctx, "war3.mpq") == 1, "asset present");
    char name[64] = {0};
    check(a->enumerate_mpq(a->ctx, 0, name, sizeof name) == 1,
          "asset enumerate");
    check(strcmp(name, "war3.mpq") == 0, "asset enumerate name");

    const char *patch = "/data/local/tmp/wc3-platform-assets/War3Patch.mpq";
    fd = open(patch, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
    check(a->present(a->ctx, "war3patch.mpq") == 1,
          "asset present case-insensitive");
    check(a->present(a->ctx, "../war3.mpq") == 0,
          "asset present rejects traversal");
    (void)unlink(mpq);
    (void)unlink(patch);
    (void)rmdir(dir);
}

static void test_config(const Wc3ConfigApi *c)
{
    const char *path = "/data/local/tmp/wc3-platform.cfg";
    FILE *file = fopen(path, "wb");
    check(file != NULL, "config file create");
    if (!file) return;
    const char *text =
        " alpha = 42 \r\n"
        "# comment\n"
        "name = first\n"
        "name=last\n"
        "empty=\n"
        "badline\n"
        "vulkan_driver = turnip\n";
    check(fwrite(text, 1, strlen(text), file) == strlen(text),
          "config file write");
    (void)fclose(file);

    check(wc3_android_config_load(path) == 0, "config load");
    check(c->get && c->get_int, "config table");
    const char *value = c->get(c->ctx, "alpha");
    check(value && strcmp(value, "42") == 0, "config trimmed value");
    check(c->get_int(c->ctx, "alpha", -1) == 42, "config int");
    value = c->get(c->ctx, "name");
    check(value && strcmp(value, "last") == 0,
          "config duplicate last wins");
    value = c->get(c->ctx, "empty");
    check(value && strcmp(value, "") == 0, "config empty value");
    check(c->get(c->ctx, "badline") == NULL, "config bad line ignored");
    check(c->get(c->ctx, "missing") == NULL, "config missing key");
    check(c->get_int(c->ctx, "missing", -9) == -9,
          "config int fallback");
    check(c->get_int(c->ctx, "name", 7) == 7,
          "config invalid int fallback");
    value = c->get(c->ctx, "vulkan_driver");
    check(value && strcmp(value, "turnip") == 0, "config driver key");

    const char *stable = c->get(c->ctx, "name");
    check(wc3_android_config_load(path) == -1, "config load once");
    check(stable && strcmp(stable, "last") == 0, "config pointer stable");

    (void)unlink(path);
}

static _Atomic uint32_t g_audio_callbacks;

static void audio_test_callback(void *opaque, void *frames, uint32_t bytes)
{
    (void)opaque;
    memset(frames, 0, bytes);
    atomic_fetch_add_explicit(&g_audio_callbacks, 1, memory_order_relaxed);
}

static void test_audio(const Wc3AudioApi *a)
{
    check(a->open && a->write && a->close && a->set_callback,
          "audio table");

    uintptr_t stream = 0;
    check(a->open(a->ctx, 44100, 2, 8, &stream) == -1,
          "audio rejects 8-bit");
    check(stream == 0u, "audio rejected leaves output");

    check(a->open(a->ctx, 44100, 2, 16, &stream) == 0, "audio open");
    if (!stream) return;

    atomic_store_explicit(&g_audio_callbacks, 0, memory_order_relaxed);
    check(a->set_callback(a->ctx, stream, audio_test_callback, NULL) == 0,
          "audio callback set");

    const uint32_t frames = 4410;
    const uint32_t bytes = frames * 2u * 2u;
    void *pcm = calloc(1, bytes);
    check(pcm != NULL, "audio pcm alloc");
    if (pcm) {
        uint32_t written = 0;
        check(a->write(a->ctx, stream, pcm, bytes, &written) == 0,
              "audio write");
        check(written == bytes, "audio written");
        check(a->write(a->ctx, stream, pcm, bytes - 1u, &written) == -1,
              "audio rejects partial frame");
        free(pcm);
    }

    for (int i = 0; i < 200 && atomic_load_explicit(
                                      &g_audio_callbacks,
                                      memory_order_relaxed) == 0;
         i++) {
        usleep(5000);
    }
    check(atomic_load_explicit(&g_audio_callbacks,
                               memory_order_relaxed) > 0,
          "audio callback ran");
    check(a->close(a->ctx, stream) == 0, "audio close");
}

static void test_surface(const Wc3SurfaceApi *s)
{
    check(s->acquire && s->release && s->resize && s->lost, "surface table");
    uintptr_t window = 1u;
    check(s->acquire(s->ctx, &window) == -1, "surface absent acquire");
    check(window == 1u, "surface absent leaves output");
    check(s->acquire(s->ctx, NULL) == -1, "surface null output");
    s->release(s->ctx, 0u);
    s->release(s->ctx, 1u);
    s->resize(s->ctx, 0u, 0u);
    s->lost(s->ctx);
    check(s->acquire(s->ctx, &window) == -1, "surface lost acquire");
}

int main(int argc, char **argv)
{
    Wc3Platform p;
    check(wc3_android_platform_init(&p) == 0, "platform init");
    check(wc3_platform_install(&p) == 0, "platform install");
    check(p.mem.reserve && p.file.open && p.time.monotonic_ns &&
              p.thread.create && p.sync.wait && p.input.push &&
              p.input.set_text_focus && p.surface.acquire && p.audio.open &&
              p.audio.set_callback && p.config.get && p.asset.init,
          "platform table");
    test_guest_memory();
    if (argc >= 2) test_pe(&p, argv[1], GUEST_IMAGE_WAR3, 1);
    if (argc >= 3) test_pe(&p, argv[2], GUEST_IMAGE_GAME, 1);
    if (argc >= 3) test_pe_relocate(&p, argv[2]);
    if (argc >= 2) test_pe_bad(&p, argv[1]);
    test_memory(&p.mem);
    test_file(&p.file);
    test_time(&p.time);
    test_thread(&p.thread);
    test_input(&p.input, &p.thread);
    test_sync(&p.sync);
    test_abandoned_mutex(&p.sync, &p.thread);
    test_surface(&p.surface);
    test_audio(&p.audio);
    test_config(&p.config);
    test_asset(&p.asset);
    p.log.write(p.log.ctx, 4, "wc3-platform", "device test complete");

    if (g_failures) {
        printf("platform android: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("platform android: OK\n");
    return 0;
}
