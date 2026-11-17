/*
 * selection24.c - 24-unit selection and adaptive 6x2 / 8x3 unit panel.
 *
 * The lifted CInfoPanelGroup and CSelectionWar3 code stays in the generated
 * corpus.  This module supplies the host helpers that the
 * tools/lift/csrc_local/selection24.c pass calls from the addresses listed in
 * docs/research/SELECTION_24_ANALYSIS.md.  The formation helpers
 * wc3_sel24_init24/wc3_sel24_sort24 operate on guest memory directly; they do
 * not call guest code.
 *
 * Guest code is called through push32 + dispatch, never through a host call.
 * Each helper saves and restores the caller's integer registers, esp, eip and
 * flags so the generated caller sees exactly the state it expects.  x87/SSE
 * registers are caller-saved at every patch point and are not preserved here;
 * the original code at those points does not keep live values in them either.
 *
 * This is gameplay/UI code, not HLE.  No host locks, no static TLS and no
 * untagged diagnostics.
 */
#include "selection24.h"
#include "wc3_debug.h"

#include <stdlib.h>

/* Debug-only trace.  Every diagnostic write below is inside an explicit
 * WC3_DEBUG && WC3_DEBUG_SELECT24 guard; the tag names the observed
 * subsystem and release code contains no logging call at all. */
#if WC3_DEBUG && WC3_DEBUG_SELECT24
#include <stdio.h>
#ifdef __ANDROID__
/*
 * Debug-only selection24 log.  run.txt mixes the HLE, DXVK and crash
 * diagnostics into one huge stream; this pass has its own file in the
 * app-private state directory so the events that lead to a crash can be read
 * without filtering run.txt.  It is opened once, truncated per process, line
 * buffered and falls back to stderr when WC3_DATA_DIR is unavailable.
 */
#include <pthread.h>
#include <sys/mman.h>
static FILE *g_sel24_log;
static pthread_once_t g_sel24_log_once = PTHREAD_ONCE_INIT;

static void sel24_open_log(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    char path[1024];

    if (dir && *dir)
        snprintf(path, sizeof(path), "%s/sel24_trace.txt", dir);
    else
        snprintf(path, sizeof(path), "sel24_trace.txt");
    g_sel24_log = fopen(path, "w");
    if (!g_sel24_log)
        return;
    setvbuf(g_sel24_log, NULL, _IOLBF, 0);
    fprintf(g_sel24_log, "[select24] trace start\n");
}

static FILE *sel24_log(void)
{
    pthread_once(&g_sel24_log_once, sel24_open_log);
    return g_sel24_log ? g_sel24_log : stderr;
}

#define SEL24_OUT sel24_log()
#else
#define SEL24_OUT stderr
#endif
#endif /* WC3_DEBUG && WC3_DEBUG_SELECT24 */

/* One definition in the helper library: the product build links this module
 * whenever WC3_SELECTION_24 is ON, so the marker is present exactly once. */
#if defined(__GNUC__)
__attribute__((used))
#endif
const char wc3_selection24_build_marker[] = "WC3_SELECTION_24";

/* Runtime 24/12 cap.  Zero means "not latched yet"; every limit site and the
 * panel helper read the same value for the process lifetime.  The Android host
 * exports WC3_SELECTION_24=1 to enable the 24-unit profile before android_main;
 * a missing variable, or any value other than "1", keeps the product default
 * of the original 12 units. */
static uint32_t g_sel24_cap;

uint32_t wc3_sel24_cap(void)
{
    uint32_t cap = g_sel24_cap;

    if (cap == 0u) {
        const char *value = getenv("WC3_SELECTION_24");
        cap = (value && value[0] == '1' && value[1] == '\0') ? 24u : 12u;
        g_sel24_cap = cap;
    }
    return cap;
}

/* CInfoPanelGroup fields added by the selection24 profile. */
#define SEL24_GROUP_GRID12   0x130u
#define SEL24_GROUP_GRID24   0x134u
#define SEL24_GROUP_ACTIVE   0x138u
#define SEL24_GROUP_INIT     0x13Cu
#define SEL24_GROUP_TOTAL    0x140u

/* CSelectionWar3 field with the total number of selected units. */
#define SEL24_SELECTION_TOTAL 0x1F8u
/* Head of the selection's unit list.  Every node is {next, prev, object}; the
 * object keeps the unit handle at +0x0C and the unit serial at +0x10. */
#define SEL24_SELECTION_HEAD 0x1F4u

/* CSimpleGrid / CSimpleFrame fields used by the original code. */
#define SEL24_GRID_ROWS      0x124u
#define SEL24_GRID_COLS      0x128u
#define SEL24_GRID_SCALE     0x060u
#define SEL24_GRID_VISIBLE   0x090u
#define SEL24_GRID_ROWS_AT   0x154u
#define SEL24_FRAME_VISIBLE  0x090u
#define SEL24_FRAME_REGISTERED 0x094u

/* Row descriptor: 0x10 bytes, button array pointer at +8, button stride 4. */
#define SEL24_ROW_STRIDE     0x010u
#define SEL24_ROW_BUTTONS    0x008u

#define SEL24_BUTTON_VISIBLE 0x090u

/* Size of the CInfoPanelGroup allocation is widened 0x134 -> 0x144. */
#define SEL24_GROUP_SIZE     0x144u

#define SEL24_GRID_SIZE      0x15Cu
#define SEL24_GRID_LINE      0x024u
#define SEL24_BUTTON_SIZE    0x140u
#define SEL24_BUTTON_LINE    0x076u
#define SEL24_ALLOC_FILE     0x6FABF9F8u

/* Original grid12 geometry and the scaled 8x3 grid24 geometry. */
#define SEL24_GRID12_WIDTH   0x3E3E6666u
#define SEL24_GRID12_HEIGHT  0x3DD00000u
#define SEL24_GRID24_WIDTH   0x3E7DDDDDu
#define SEL24_GRID24_HEIGHT  0x3E0AAAABu
#define SEL24_GRID24_OFFSET_X 0xBB088889u
#define SEL24_GRID24_OFFSET_Y 0x00000000u
#define SEL24_GRID24_SCALE   0x3F400000u /* 0.75f */

#define SEL24_GRID24_ROWS    3u
#define SEL24_GRID24_COLS    8u
#define SEL24_GRID12_ROWS    2u
#define SEL24_GRID12_COLS    6u

/* Guest entry points.  All are regular lifted functions. */
#define SEL24_ALLOC          0x6F07C6D2u
#define SEL24_GRID_CTOR      0x6F10E230u
#define SEL24_GRID_RESIZE    0x6F10EE50u
#define SEL24_GRID_INSERT    0x6F10E490u
#define SEL24_GRID_WIDTH     0x6F1116C0u
#define SEL24_GRID_HEIGHT    0x6F111520u
#define SEL24_GRID_ANCHOR    0x6F111600u
#define SEL24_BUTTON_CTOR    0x6F39EDC0u
#define SEL24_FILL_CELL      0x6F3DA470u
#define SEL24_FRAME_HIDE     0x6F10D410u
#define SEL24_FRAME_SHOW     0x6F10DE60u

/* Real return addresses from the corresponding original call sites. */
#define SEL24_RET_GRID_ALLOC 0x6F39ED29u
#define SEL24_RET_BUTTON_ALLOC 0x6F3B91F5u
#define SEL24_RET_GRID_CTOR  0x6F39ED3Fu
#define SEL24_RET_WIDTH      0x6F39ED5Cu
#define SEL24_RET_HEIGHT     0x6F39ED6Fu
#define SEL24_RET_ANCHOR     0x6F39ED93u
#define SEL24_RET_RESIZE     0x6F39EDA2u
#define SEL24_RET_BUTTON     0x6F3B9213u
#define SEL24_RET_INSERT     0x6F3B922Eu
#define SEL24_RET_FILL       0x6F3C7E99u
#define SEL24_RET_HIDE       0x6F3ACCE2u
#define SEL24_RET_SHOW       0x6F10DEC4u

/* The group is allocated without zeroing, so the marker is a magic value.
 * A fresh object is initialised once; the marker prevents a second grid24
 * from being created (or a failed allocation from being retried) if the tail
 * is reached twice. */
#define SEL24_INIT_MAGIC     0x53454C32u /* "SEL2" */

/* Vtable slots of CSimpleFrame. */
#define SEL24_VT_HIDE        0x064u
#define SEL24_VT_SHOW        0x068u

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eip;
    uint32_t cf, zf, sf, of, pf, af;
} Sel24CpuState;

static void sel24_save_state(CPU *c, Sel24CpuState *s)
{
    s->eax = c->eax;
    s->ecx = c->ecx;
    s->edx = c->edx;
    s->ebx = c->ebx;
    s->esp = c->esp;
    s->ebp = c->ebp;
    s->esi = c->esi;
    s->edi = c->edi;
    s->eip = c->eip;
    s->cf = c->cf;
    s->zf = c->zf;
    s->sf = c->sf;
    s->of = c->of;
    s->pf = c->pf;
    s->af = c->af;
}

static void sel24_restore_state(CPU *c, const Sel24CpuState *s)
{
    c->eax = s->eax;
    c->ecx = s->ecx;
    c->edx = s->edx;
    c->ebx = s->ebx;
    c->esp = s->esp;
    c->ebp = s->ebp;
    c->esi = s->esi;
    c->edi = s->edi;
    c->eip = s->eip;
    c->cf = s->cf;
    c->zf = s->zf;
    c->sf = s->sf;
    c->of = s->of;
    c->pf = s->pf;
    c->af = s->af;
}

#if WC3_DEBUG && WC3_DEBUG_SELECT24
/*
 * The LiftPass inserts this call immediately after the original AddUnit cap
 * comparison.  The following `jae` still needs the flags, so the trace saves
 * and restores the full integer CPU state before printing.
 */
void wc3_sel24_trace_addunit(CPU *c, uint32_t group, uint32_t unit,
                             uint32_t count)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    fprintf(SEL24_OUT,
            "[select24] addunit this=%08X unit=%08X count=%u\n",
            group, unit, count);
    sel24_restore_state(c, &saved);
}

/*
 * Read-only replica of the handle table lookup at 0x6F054530, used only to
 * say which of its three checks rejects the pair.  The table layout is
 *   [0x6FD68610] + 0x0C -> array  for handles with bit 31 clear
 *   [0x6FD68610] + 0x1C -> count  for handles with bit 31 clear
 *   [0x6FD68610] + 0x2C -> array  for handles with bit 31 set
 *   [0x6FD68610] + 0x3C -> count  for handles with bit 31 set
 * Each entry is {state, object}; state 0xFFFFFFFE marks a live slot, and the
 * lookup succeeds only when object+0x18 equals the serial it was given.
 */
#define SEL24_HANDLE_TABLE 0x6FD68610u
#define SEL24_HANDLE_LIVE  0xFFFFFFFEu
#define SEL24_HANDLE_MAX   0x00100000u

static int sel24_mapped(uint32_t addr, uint32_t size);
static uint32_t sel24_rd(uint32_t base, uint32_t offset);

static void sel24_describe_handle(uint32_t handle, uint32_t serial, char *out,
                                  size_t outsz, int *ok)
{
    uint32_t table;
    uint32_t index;
    uint32_t count;
    uint32_t base;
    uint32_t entry;
    uint32_t state;
    uint32_t object;

    *ok = 0;
    table = rd32(GVA(SEL24_HANDLE_TABLE));
    if (table == 0u) {
        snprintf(out, outsz, "table=0");
        return;
    }
    if (!sel24_mapped(table, 0x40u)) {
        snprintf(out, outsz, "table=%08X unmapped", table);
        return;
    }
    if ((int32_t)handle < 0) {
        /*
         * The game keeps a second slot array for negative ids at +0x2C/+0x3C
         * and addresses it through the complement of the id, so 0xFFFFFFFE is
         * slot 1 of that array.  Masking the sign bit (the earlier version)
         * produced the meaningless index 0x7FFFFFFE and reported every
         * negative handle as out of range.
         */
        index = ~handle;
        count = rd32(table + 0x3Cu);
        base = rd32(table + 0x2Cu);
    } else {
        index = handle;
        count = rd32(table + 0x1Cu);
        base = rd32(table + 0x0Cu);
    }
    if (count > SEL24_HANDLE_MAX) {
        snprintf(out, outsz, "table=%08X count=%u suspect", table, count);
        return;
    }
    if (index >= count) {
        snprintf(out, outsz,
                 "table=%08X index=%u out-of-range count=%u base=%08X",
                 table, index, count, base);
        return;
    }
    if (base == 0u) {
        snprintf(out, outsz, "table=%08X index=%u base=0", table, index);
        return;
    }
    entry = base + index * 8u;
    if (!sel24_mapped(entry, 8u)) {
        snprintf(out, outsz, "table=%08X index=%u entry=%08X unmapped", table,
                 index, entry);
        return;
    }
    state = rd32(entry);
    if (state != SEL24_HANDLE_LIVE) {
        snprintf(out, outsz,
                 "table=%08X index=%u slot-free state=%08X", table, index,
                 state);
        return;
    }
    object = rd32(entry + 4u);
    if (object == 0u) {
        snprintf(out, outsz, "table=%08X index=%u slot-null", table, index);
        return;
    }
    if (!sel24_mapped(object, 0x1Cu)) {
        snprintf(out, outsz, "table=%08X index=%u object=%08X unmapped", table,
                 index, object);
        return;
    }
    if (rd32(object + 0x18u) != serial) {
        snprintf(out, outsz,
                 "table=%08X index=%u serial=%u mismatch live=%u object=%08X",
                 table, index, serial, rd32(object + 0x18u), object);
        return;
    }
    *ok = 1;
    snprintf(out, outsz, "table=%08X index=%u object=%08X", table, index,
             object);
}

/*
 * Read-only dump of the event descriptor and the guest EBP chain for the
 * failing event.  The descriptor built by 0x6F691F20 lives at [ebp-0x3C] and
 * carries the target unit at +0xC; the two order builders keep their record
 * count and current index at [ebp-0x1C4] and [ebp-0x200].  The stack walk is
 * confined to the guest stack range and every pointer is range- and
 * mapping-checked before it is dereferenced, so a corrupted chain cannot turn
 * the diagnostic into a second fault.
 */
#define SEL24_GUEST_STACK_LO 0x30000000u
#define SEL24_GUEST_STACK_HI 0x40000000u
#define SEL24_GUEST_HEAP_LO  0x40000000u
#define SEL24_GUEST_HEAP_HI  0x50000000u
#define SEL24_LOCAL_MAGIC    0x2B61676Cu

static int sel24_in_range(uint32_t value, uint32_t lo, uint32_t hi)
{
    return value >= lo && value < hi;
}

/* A window is not a map.  VA2 (0x40000000-0x60000000) is reserved in one piece
 * and committed page by page; both the runtime range table and the HLE layout
 * table are seeded with the whole arena, so every table in the process calls an
 * untouched page "ok".  A stale stack word such as 0x49AFC800 passes all of
 * them and faults on the read, turning a bad game state into a fault inside the
 * diagnostic.
 *
 * The guest window is one 4 GiB PROT_NONE VMA as well, so mincore() reports a
 * reserved page as present and cannot answer whether the read is safe.
 * /proc/self/maps is the local source that reports both the mapping and its
 * protection; only a page with the read bit may be dereferenced.  Positive
 * results are cached per thread because consecutive words of one object share
 * a page; a negative result is never cached, since the guest may commit that
 * page later. */
#if defined(__ANDROID__)
static _Thread_local uint32_t sel24_mapped_page; /* 0 = nothing cached */

static int sel24_host_readable(uintptr_t addr)
{
    char line[256];
    unsigned long lo, hi;
    char perms[8];
    FILE *maps = fopen("/proc/self/maps", "r");
    int readable = 0;

    if (!maps) return 0;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 &&
            (unsigned long)addr >= lo && (unsigned long)addr < hi) {
            readable = perms[0] == 'r';
            break;
        }
    }
    fclose(maps);
    return readable;
}

static int sel24_mapped(uint32_t addr, uint32_t size)
{
    const uint32_t base = addr & ~0xFFFu;
    const uint32_t span = (addr - base) + size;
    uint32_t pages = (span + 0xFFFu) >> 12;

    if (pages == 0u) pages = 1u;
    if (pages > 2u) pages = 2u; /* callers read at most 0x40 bytes */
    if (pages == 1u && base != 0u && base == sel24_mapped_page) return 1;
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t host = (uintptr_t)guest_host_ptr(base + (i << 12));
        if (!sel24_host_readable(host)) return 0;
    }
    if (pages == 1u) sel24_mapped_page = base;
    return 1;
}
#else
/* Only the Android build runs the game.  The host build exists for a syntax
 * check, where the two windows above stay in charge. */
static int sel24_mapped(uint32_t addr, uint32_t size)
{
    (void)addr;
    (void)size;
    return 1;
}
#endif

static int sel24_stack_word(uint32_t value)
{
    return sel24_in_range(value, SEL24_GUEST_STACK_LO,
                          SEL24_GUEST_STACK_HI) &&
           (value & 3u) == 0u && sel24_mapped(value, 4u);
}

static int sel24_heap_word(uint32_t value)
{
    return sel24_in_range(value, SEL24_GUEST_HEAP_LO,
                          SEL24_GUEST_HEAP_HI) &&
           (value & 3u) == 0u && sel24_mapped(value, 4u);
}

/* Any guest address this diagnostic may dereference: the guest stack or the
 * identity-mapped heap window.  Everything else is printed as a raw value. */
static int sel24_readable(uint32_t value)
{
    return sel24_stack_word(value) || sel24_heap_word(value);
}

/* Word read that turns an unreadable pointer into 0 instead of a second
 * fault; the trace must never be the reason a run dies. */
static uint32_t sel24_rd(uint32_t base, uint32_t offset)
{
    return sel24_readable(base) && sel24_mapped(base + offset, 4u)
               ? rd32(base + offset)
               : 0u;
}

static void sel24_trace_object_line(const char *tag, uint32_t object,
                                    unsigned words)
{
    unsigned i;

    fprintf(SEL24_OUT, "[select24] %s object=%08X", tag, object);
    for (i = 0; i < words; ++i)
        fprintf(SEL24_OUT, " +%02X=%08X", i * 4u,
                sel24_rd(object, i * 4u));
    fputc('\n', SEL24_OUT);
}

/* The batch record of 0x6F6B98F0 / 0x6F6B9F70 is nine dwords at stride 0x24;
 * the order loop reads +0x00 (unit) and +0x1C (event handle), and the writer
 * fills +0x04, +0x08 and +0x0C..+0x20 as well.  Print all of them, so a record
 * that was never written is distinguishable from one with a stale handle. */
#define SEL24_ORDER_RECORD_STRIDE 0x24u
#define SEL24_ORDER_RECORD_MAX    24u

static void sel24_trace_order_record(const char *tag, uint32_t index,
                                     uint32_t record)
{
    fprintf(SEL24_OUT,
            "[select24] %s idx=%u rec=%08X "
            "e00=%08X e04=%08X e08=%08X e0C=%08X e10=%08X "
            "e14=%08X e18=%08X e1C=%08X e20=%08X\n",
            tag, index, record,
            sel24_rd(record, 0x00u), sel24_rd(record, 0x04u),
            sel24_rd(record, 0x08u), sel24_rd(record, 0x0Cu),
            sel24_rd(record, 0x10u), sel24_rd(record, 0x14u),
            sel24_rd(record, 0x18u), sel24_rd(record, 0x1Cu),
            sel24_rd(record, 0x20u));
}

/*
 * Walk the +0x08 chain of the interface objects that carry {handle, serial}
 * at +0x14/+0x18.  The chain is what links the rejected event to the previous
 * interface that still resolved, so the transition from a live generation to
 * the out-of-range one is visible in one place.  The walk is bounded and
 * cycle-safe; it never follows a pointer outside the guest heap range.
 */
static void sel24_trace_self_chain(uint32_t self)
{
    uint32_t seen[8];
    uint32_t node = self;
    unsigned count = 0u;

    while (node != 0u && sel24_heap_word(node) && count < 8u) {
        uint32_t ref = sel24_rd(node, 4u);
        uint32_t prev = sel24_rd(node, 8u);
        uint32_t handle = sel24_rd(node, 0x14u);
        uint32_t serial = sel24_rd(node, 0x18u);
        uint32_t klass = sel24_rd(node, 0x10u);
        unsigned i;
        int duplicate = 0;

        for (i = 0u; i < count; ++i)
            if (seen[i] == node)
                duplicate = 1;
        if (duplicate)
            break;
        seen[count] = node;
        fprintf(SEL24_OUT,
                "[select24] selfchain i=%u self=%08X handle=%08X serial=%u "
                "ref=%08X prev=%08X class=%08X link2C=%08X link3C=%08X\n",
                count, node, handle, serial, ref, prev, klass,
                sel24_rd(node, 0x2Cu), sel24_rd(node, 0x3Cu));
        ++count;
        node = prev;
    }
    if (count == 0u)
        fprintf(SEL24_OUT, "[select24] selfchain self=%08X not-in-heap\n",
                self);
}

/*
 * Read-only scan of the live half of the handle table.  The rejected index is
 * past the end of the array, so the entry itself cannot be inspected.  The
 * scan answers the two questions the single entry cannot:
 *   - does the pair's serial belong to another live object (a stale handle
 *     copied from a different generation), and
 *   - is the target/ref/self registered under a different index (the lookup
 *     used the wrong handle, not an unregistered object)?
 * Output is capped per category; the summary keeps the true hit counts.
 */
static void sel24_scan_handle_table(uint32_t self, uint32_t handle,
                                    uint32_t serial, uint32_t ref)
{
    uint32_t root = rd32(GVA(SEL24_HANDLE_TABLE));
    uint32_t count;
    uint32_t base;
    uint32_t unit = 0u;
    uint32_t i;
    unsigned self_hits = 0u;
    unsigned ref_hits = 0u;
    unsigned unit_hits = 0u;
    unsigned serial_hits = 0u;
    unsigned self_printed = 0u;
    unsigned ref_printed = 0u;
    unsigned unit_printed = 0u;
    unsigned serial_printed = 0u;

    if (root == 0u || !sel24_mapped(root, 0x40u)) {
        fprintf(SEL24_OUT, "[select24] scan table=%08X unmapped\n", root);
        return;
    }
    if ((int32_t)handle < 0) {
        count = rd32(root + 0x3Cu);
        base = rd32(root + 0x2Cu);
    } else {
        count = rd32(root + 0x1Cu);
        base = rd32(root + 0x0Cu);
    }
    if (count > SEL24_HANDLE_MAX ||
        (count != 0u &&
         (base < SEL24_GUEST_HEAP_LO || base >= SEL24_GUEST_HEAP_HI ||
          count > (SEL24_GUEST_HEAP_HI - base) / 8u))) {
        fprintf(SEL24_OUT,
                "[select24] scan root=%08X base=%08X count=%u suspect\n",
                root, base, count);
        return;
    }
    if (ref >= 0x50u)
        unit = ref - 0x50u;
    fprintf(SEL24_OUT,
            "[select24] scan root=%08X base=%08X count=%u handle=%08X "
            "serial=%u self=%08X ref=%08X unit=%08X\n",
            root, base, count, handle, serial, self, ref, unit);
    for (i = 0u; i < count; ++i) {
        uint32_t entry = base + i * 8u;
        uint32_t state = sel24_rd(entry, 0u);
        uint32_t object;
        uint32_t object_serial;

        if (state != SEL24_HANDLE_LIVE)
            continue;
        object = sel24_rd(entry, 4u);
        if (!sel24_heap_word(object))
            continue;
        object_serial = sel24_rd(object, 0x18u);
        if (self != 0u && object == self) {
            ++self_hits;
            if (self_printed < 4u) {
                fprintf(SEL24_OUT,
                        "[select24] scan self-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++self_printed;
            }
        }
        if (ref != 0u && object == ref) {
            ++ref_hits;
            if (ref_printed < 4u) {
                fprintf(SEL24_OUT,
                        "[select24] scan ref-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++ref_printed;
            }
        }
        if (unit != 0u && object == unit) {
            ++unit_hits;
            if (unit_printed < 4u) {
                fprintf(SEL24_OUT,
                        "[select24] scan unit-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++unit_printed;
            }
        }
        if (serial != 0u && object_serial == serial) {
            ++serial_hits;
            if (serial_printed < 4u) {
                fprintf(SEL24_OUT,
                        "[select24] scan serial-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++serial_printed;
            }
        }
    }
    fprintf(SEL24_OUT,
            "[select24] scan done self-hits=%u ref-hits=%u unit-hits=%u "
            "serial-hits=%u\n",
            self_hits, ref_hits, unit_hits, serial_hits);
}

static void sel24_trace_event_context(CPU *c, uint32_t self, uint32_t arg,
                                      int ok)
{
    uint32_t ebp = c->ebp;
    unsigned frame;
    int local_seen = 0;

    for (frame = 0; frame < 32u; ++frame) {
        uint32_t next;
        uint32_t ra;

        if (!sel24_stack_word(ebp))
            break;
        next = rd32(ebp);
        ra = rd32(ebp + 4u);

        /*
         * L_6F691F20 keeps the event descriptor at [ebp-0x3C]:
         *   +0x00 magic, +0x04 magic, +0x08 zero,
         *   +0x0C target unit, +0x10 event type.
         * The magic is unique enough to find the frame without relying on a
         * return address that differs between callers.
         */
        if (ebp >= SEL24_GUEST_STACK_LO + 0x40u &&
            rd32(ebp - 0x3Cu) == SEL24_LOCAL_MAGIC) {
            uint32_t unit = rd32(ebp - 0x30u);
            uint32_t event = rd32(ebp - 0x2Cu);
            uint32_t vtbl = sel24_heap_word(unit) ? rd32(unit) : 0u;
            uint32_t u14 = sel24_heap_word(unit) ? rd32(unit + 0x14u) : 0u;
            uint32_t u18 = sel24_heap_word(unit) ? rd32(unit + 0x18u) : 0u;

            local_seen = 1;
            fprintf(SEL24_OUT,
                    "[select24] local f%u ebp=%08X unit=%08X event=%08X "
                    "vtbl=%08X unit14=%08X unit18=%08X self=%08X "
                    "arg0=%08X arg1=%08X arg2=%08X\n",
                    frame, ebp, unit, event, vtbl, u14, u18, self,
                    rd32(ebp + 8u), rd32(ebp + 0xCu), rd32(ebp + 0x10u));
            if (!ok && sel24_heap_word(unit)) {
                sel24_trace_object_line("unit", unit, 17u);
                fprintf(SEL24_OUT,
                        "[select24] unit2 unit=%08X u44=%08X u48=%08X "
                        "u4C=%08X u50=%08X u54=%08X\n",
                        unit, sel24_rd(unit, 0x44u), sel24_rd(unit, 0x48u),
                        sel24_rd(unit, 0x4Cu), sel24_rd(unit, 0x50u),
                        sel24_rd(unit, 0x54u));
            }
        }

        /*
         * Builder 0x6F6B98F0 keeps the same object at [ebp-0x20C] and
         * [ebp-0x204], its record count at [ebp-0x1C4] and the current index
         * at [ebp-0x208].  The object repeats the count at +0x40, which makes
         * the match specific; the record array was moved to [ebp-0x960].
         * [ebp-0x200] in this frame is a pointer to a local handed to the
         * event lookup, so reading it as the index would make the check
         * unsatisfiable.
         */
        if (ebp >= SEL24_GUEST_STACK_LO + 0x9A0u) {
            uint32_t object = rd32(ebp - 0x204u);
            uint32_t alias = rd32(ebp - 0x20Cu);
            uint32_t count = rd32(ebp - 0x1C4u);
            uint32_t index = rd32(ebp - 0x208u);
            int strict = object == alias && count <= 24u && index <= count &&
                         sel24_heap_word(object) &&
                         sel24_rd(object, 0x40u) == count;

            if (strict) {
                uint32_t array = ebp - 0x960u;
                uint32_t element = array + index * 0x24u;
                uint32_t i;

                fprintf(SEL24_OUT,
                        "[select24] builderB f%u ebp=%08X count=%u index=%u "
                        "object=%08X object14=%08X object18=%08X "
                        "object1C=%08X array=%08X element=%08X\n",
                        frame, ebp, count, index, object,
                        sel24_rd(object, 0x14u), sel24_rd(object, 0x18u),
                        sel24_rd(object, 0x1Cu), array, element);
                if (sel24_stack_word(array) &&
                    sel24_stack_word(element) &&
                    element + 0x24u <= SEL24_GUEST_STACK_HI) {
                    fprintf(SEL24_OUT,
                            "[select24] element e00=%08X e04=%08X e08=%08X "
                            "e0C=%08X e10=%08X e14=%08X e18=%08X "
                            "e1C=%08X e20=%08X\n",
                            sel24_rd(element, 0x00u),
                            sel24_rd(element, 0x04u),
                            sel24_rd(element, 0x08u),
                            sel24_rd(element, 0x0Cu),
                            sel24_rd(element, 0x10u),
                            sel24_rd(element, 0x14u),
                            sel24_rd(element, 0x18u),
                            sel24_rd(element, 0x1Cu),
                            sel24_rd(element, 0x20u));
                }
                /*
                 * Dump the whole batch as it stands when the failure is
                 * reported.  The loop trace prints the same array on its first
                 * iteration; this copy covers the case where the guard fires
                 * on a frame the loop trace never reached (a chunk that was
                 * compiled without it, or a batch whose loop already exited).
                 */
                if (sel24_stack_word(array)) {
                    uint32_t total = count;

                    if (total > SEL24_ORDER_RECORD_MAX)
                        total = SEL24_ORDER_RECORD_MAX;
                    fprintf(SEL24_OUT,
                            "[select24] builderB array base=%08X count=%u\n",
                            array, count);
                    for (i = 0; i < total; ++i) {
                        uint32_t rec = array + i * SEL24_ORDER_RECORD_STRIDE;

                        if (!sel24_stack_word(rec) ||
                            rec + SEL24_ORDER_RECORD_STRIDE >
                                SEL24_GUEST_STACK_HI)
                            break;
                        sel24_trace_order_record("builderB", i, rec);
                    }
                }
            } else if (!ok &&
                       (sel24_heap_word(object) || sel24_heap_word(alias) ||
                        count != 0u || index != 0u)) {
                /*
                 * The strict identity check misses the failing frame when the
                 * producer stores a different alias or the count copy is
                 * already damaged.  Print the raw candidates and the record at
                 * the current index anyway; this is what distinguishes "the
                 * builder frame was not walked" from "the record itself is
                 * wrong".
                 */
                uint32_t object40 = sel24_rd(object, 0x40u);

                fprintf(SEL24_OUT,
                        "[select24] builderB? f%u ebp=%08X object=%08X "
                        "alias=%08X count=%u index=%u object40=%08X "
                        "strict=%d\n",
                        frame, ebp, object, alias, count, index, object40,
                        strict);
                if (index <= 64u) {
                    uint32_t array = ebp - 0x960u;
                    uint32_t element = array + index * 0x24u;

                    if (sel24_stack_word(array) &&
                        element + 0x24u <= SEL24_GUEST_STACK_HI) {
                        fprintf(SEL24_OUT,
                                "[select24] builderB? element "
                                "e00=%08X e04=%08X e08=%08X e0C=%08X "
                                "e10=%08X e14=%08X e18=%08X e1C=%08X "
                                "e20=%08X\n",
                                sel24_rd(element, 0x00u),
                                sel24_rd(element, 0x04u),
                                sel24_rd(element, 0x08u),
                                sel24_rd(element, 0x0Cu),
                                sel24_rd(element, 0x10u),
                                sel24_rd(element, 0x14u),
                                sel24_rd(element, 0x18u),
                                sel24_rd(element, 0x1Cu),
                                sel24_rd(element, 0x20u));
                    }
                }
            }
        }

        /*
         * L_6F146D30 is called from L_6F146C10 with the source interface at
         * [caller_ebp+8].  Print that object when the walk reaches the
         * producer, so the rejected {handle, serial} can be compared with the
         * interface it was copied from instead of only with the new wrapper.
         */
        if (ra == 0x6F146C3Fu && sel24_stack_word(next)) {
            uint32_t source = rd32(next + 8u);

            int source_readable = sel24_heap_word(source) ||
                                  sel24_stack_word(source);

            fprintf(SEL24_OUT,
                    "[select24] source L_6F146C10 arg=%08X%s",
                    source, source_readable ? " " : "\n");
            if (source_readable) {
                fprintf(SEL24_OUT,
                        "object=%08X +00=%08X +0C=%08X +10=%08X "
                        "+14=%08X +18=%08X\n",
                        source, sel24_rd(source, 0x00u),
                        sel24_rd(source, 0x0Cu), sel24_rd(source, 0x10u),
                        sel24_rd(source, 0x14u), sel24_rd(source, 0x18u));
            }
        }

        if (!ok) {
            fprintf(SEL24_OUT, "[select24] stack f%u ebp=%08X ra=%08X\n",
                    frame, ebp, ra);
        }

        if (next <= ebp || next - ebp > 0x10000u ||
            !sel24_stack_word(next))
            break;
        ebp = next;
    }

    if (!ok && !local_seen) {
        fprintf(SEL24_OUT,
                "[select24] local not found in guest stack (cpu ebp=%08X "
                "esp=%08X eip=%08X)\n",
                c->ebp, c->esp, c->eip);
    }
}

/*
 * Inserted at the entry of 0x6F146D30, which packs {self+0x14, self+0x18}
 * into the selection event.  self == 0 is the "no object" path that sends
 * the {-1, -1} pair from 0x6F146D48.
 */
void wc3_sel24_trace_evt2(CPU *c, uint32_t self, uint32_t arg)
{
    Sel24CpuState saved;
    char probe[160];
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    uint32_t ref = 0u;
    uint32_t prev = 0u;
    uint32_t klass = 0u;
    uint32_t ra = 0u;
    int ok;

    sel24_save_state(c, &saved);
    if (self != 0u && sel24_heap_word(self)) {
        handle = sel24_rd(self, 0x14u);
        serial = sel24_rd(self, 0x18u);
        ref = sel24_rd(self, 4u);
        prev = sel24_rd(self, 8u);
        klass = sel24_rd(self, 0x10u);
    }
    if (sel24_stack_word(c->ebp))
        ra = rd32(c->ebp + 4u);
    sel24_describe_handle(handle, serial, probe, sizeof(probe), &ok);
    fprintf(SEL24_OUT,
            "[select24] evt2 self=%08X arg=%08X handle=%08X serial=%u "
            "ref=%08X prev=%08X class=%08X ra=%08X %s\n",
            self, arg, handle, serial, ref, prev, klass, ra, probe);
    if (!ok && self != 0u && sel24_heap_word(self)) {
        sel24_trace_object_line("self", self, 17u);
        if (sel24_heap_word(ref))
            sel24_trace_object_line("selfref", ref, 17u);
        if (sel24_heap_word(arg))
            sel24_trace_object_line("arg", arg, 9u);
        sel24_trace_self_chain(self);
        sel24_scan_handle_table(self, handle, serial, ref);
        /*
         * L_6F146D30 is called from L_6F146C10 with the source interface at
         * [caller_ebp+8].  Scan for that object too: a source hit means the
         * rejected pair came from a registered interface; a miss means the
         * producer was given a temporary or stale wrapper.  The helper has no
         * separate "probe" argument, so pass source+0x50 as ref; it derives
         * unit=ref-0x50 and searches the table for source.
         */
        if (sel24_stack_word(c->ebp)) {
            uint32_t caller = rd32(c->ebp);
            uint32_t source = sel24_stack_word(caller) ? rd32(caller + 8u) : 0u;

            if (source != 0u && sel24_heap_word(source)) {
                uint32_t source_handle = sel24_rd(source, 0x14u);

                fprintf(SEL24_OUT,
                        "[select24] scan source object=%08X handle=%08X "
                        "serial=%u\n", source, source_handle,
                        sel24_rd(source, 0x18u));
                sel24_scan_handle_table(0u, source_handle, 0u,
                                        source + 0x50u);
            }
        }
    }
    sel24_trace_event_context(c, self, arg, ok);
    sel24_restore_state(c, &saved);
}

/*
 * Inserted inside the null-object early returns of 0x6F057C30, 0x6F689A60
 * and 0x6F67ABE0.  All three guards are product behaviour: they prevent a
 * write to a null object ([0+0x54], [0+0x24] and [0+0x20] respectively).
 * This trace is only compiled into the debug build and records that the guard
 * actually took the null path, the caller return address and the producer
 * context that led to the lookup/event.
 */
void wc3_sel24_trace_guard(CPU *c, uint32_t arg)
{
    Sel24CpuState saved;
    uint32_t ra = 0u;

    sel24_save_state(c, &saved);
    if (sel24_stack_word(c->ebp))
        ra = rd32(c->ebp + 4u);
    fprintf(SEL24_OUT,
            "[select24] guard ecx=0 arg=%08X eip=%08X esp=%08X ebp=%08X "
            "ra=%08X\n",
            arg, c->eip, c->esp, c->ebp, ra);
    sel24_trace_event_context(c, 0u, arg, 0);
    sel24_restore_state(c, &saved);
}

/*
 * 0x6F6926B0 reads the task payload at [factory+0x54] and silently drops the
 * order for that unit when the field is zero; the two guards at 0x6F689FC0
 * and 0x6F691E60 then only see the resulting null.  This trace records every
 * task creation, null payload or not: the wrapper, the payload (with its
 * vtable, class tag and lifetime fields), the descriptor at [ebp-0x3C] that
 * 0x6F055D00 built (class tag at +4, resolved class factory from 0x6F055C40 at
 * +0xC, caller argument at +0x10) and the order target from [ebp+8]/[ebp+0xC].
 * `null=` tells the dropped unit apart without a second run.
 */
void wc3_sel24_trace_tasknull(CPU *c, uint32_t wrapper)
{
    Sel24CpuState saved;
    uint32_t desc;
    uint32_t payload;

    sel24_save_state(c, &saved);
    desc = c->ebp - 0x3Cu;
    payload = sel24_rd(wrapper, 0x54u);
    fprintf(SEL24_OUT,
            "[select24] task wrapper=%08X payload=%08X null=%u vt=%08X "
            "class=%08X handle=%08X serial=%u f50=%08X f54=%08X "
            "unit=%08X arg=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X dpair=%08X/%08X\n",
            wrapper, payload, payload == 0u ? 1u : 0u,
            sel24_rd(payload, 0x00u), sel24_rd(payload, 0x10u),
            sel24_rd(payload, 0x14u), sel24_rd(payload, 0x18u),
            sel24_rd(payload, 0x50u), sel24_rd(payload, 0x54u),
            sel24_rd(c->ebp, 0x08u), sel24_rd(c->ebp, 0x0Cu),
            desc, sel24_rd(desc, 0x04u), sel24_rd(desc, 0x0Cu),
            sel24_rd(desc, 0x10u), sel24_rd(desc, 0x14u),
            sel24_rd(desc, 0x18u));
    sel24_restore_state(c, &saved);
}

/*
 * Inserted at 0x6F154B92, right after both object initialisation slots of
 * 0x6F154B60 ran (the +0x0C ctor and the +0x44 bind).  The birth record of
 * 0x6F154B76 shows the object before the slots ran; this one shows whether a
 * slot (or the virtual factory callback [manager+0x254]) filled the +0x54
 * payload that 0x6F6926B0 later reads.  `object` (edi) and `desc` (ebx) are
 * still live in the caller frame at that point.
 */
void wc3_sel24_trace_evinit(CPU *c, uint32_t object, uint32_t desc)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    fprintf(SEL24_OUT,
            "[select24] evinit object=%08X vt=%08X class=%08X handle=%08X "
            "serial=%u f50=%08X f54=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X dpair=%08X/%08X d24=%08X d28=%08X\n",
            object, sel24_rd(object, 0x00u), sel24_rd(object, 0x10u),
            sel24_rd(object, 0x14u), sel24_rd(object, 0x18u),
            sel24_rd(object, 0x50u), sel24_rd(object, 0x54u), desc,
            sel24_rd(desc, 0x04u), sel24_rd(desc, 0x0Cu),
            sel24_rd(desc, 0x10u), sel24_rd(desc, 0x14u),
            sel24_rd(desc, 0x18u), sel24_rd(desc, 0x24u),
            sel24_rd(desc, 0x28u));
    sel24_restore_state(c, &saved);
}

/*
 * Inserted at 0x6F69273F, the second silent drop of 0x6F6926B0: the result of
 * 0x6F04CDD0 (the "instance of class" check) is zero and the function jumps to
 * its shared epilogue without touching the unit.  The caller's frame still
 * holds the target unit at [ebp+8], the order argument at [ebp+0xC], the class
 * tag at [ebp-0x48] and the wrapper object in edi, so the record pairs the
 * rejected unit with the object it was rejected for.
 */
void wc3_sel24_trace_orderdrop(CPU *c, uint32_t wrapper, uint32_t tag,
                               uint32_t result)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    fprintf(SEL24_OUT,
            "[select24] orderdrop wrapper=%08X result=%08X tag=%08X "
            "unit=%08X arg=%08X vt=%08X class=%08X handle=%08X serial=%u "
            "f50=%08X f54=%08X\n",
            wrapper, result, tag, sel24_rd(c->ebp, 0x08u),
            sel24_rd(c->ebp, 0x0Cu), sel24_rd(wrapper, 0x00u),
            sel24_rd(wrapper, 0x10u), sel24_rd(wrapper, 0x14u),
            sel24_rd(wrapper, 0x18u), sel24_rd(wrapper, 0x50u),
            sel24_rd(wrapper, 0x54u));
    sel24_restore_state(c, &saved);
}

/* Identity traces of the weak-pair check print one line per resolved object
 * (tens of thousands per session) and drown the selection events.  They stay
 * available behind WC3_SEL24_IDENT=1, read once per process. */
static pthread_once_t g_sel24_ident_once = PTHREAD_ONCE_INIT;
static int g_sel24_ident;

static void sel24_read_ident(void)
{
    const char *value = getenv("WC3_SEL24_IDENT");

    g_sel24_ident = (value && value[0] == '1' && value[1] == '\0') ? 1 : 0;
}

static int sel24_ident_trace(void)
{
    pthread_once(&g_sel24_ident_once, sel24_read_ident);
    return g_sel24_ident;
}

/*
 * Inserted around the 0x6F054530 call in 0x6F04D9C0: stage 0 carries the
 * {handle, serial} pair, stage 1 the lookup result.  Only the pairs that the
 * replica already rejects and the zero results are printed, so a normal run
 * stays quiet and a failing run shows both halves of the same lookup.
 */
void wc3_sel24_trace_lookup(CPU *c, uint32_t stage, uint32_t a, uint32_t b)
{
    Sel24CpuState saved;
    char probe[160];
    int ok;

    sel24_save_state(c, &saved);
    if (stage == 0u) {
        sel24_describe_handle(a, b, probe, sizeof(probe), &ok);
        if (!ok) {
            fprintf(SEL24_OUT, "[select24] lookup handle=%08X serial=%u %s\n",
                    a, b, probe);
        }
    } else if (a == 0u) {
        fprintf(SEL24_OUT, "[select24] lookup result=0\n");
    }
    sel24_restore_state(c, &saved);
}

/* Index and record-cursor slots of the two order loops.  The array base is
 * [ebp-0x960] in both.  The cursor points past the record start by 0x18 in one
 * loop and by 0x1C in the other, which is exactly the offset the loop uses for
 * its unit read; the index slot follows the loop shape, not a builder name:
 *   cursor 0x18 -> index [ebp-0x208]  (site 0x6F6B9E8D)
 *   cursor 0x1C -> index [ebp-0x200]  (site 0x6F6BA4C2)
 * In the 0x18 frame [ebp-0x200] holds a pointer to a local passed to the event
 * lookup, and in the 0x1C frame [ebp-0x208] holds the saved edi, so the two
 * slots must not be mixed.  The batch count is [ebp-0x1C4] in both.
 */
#define SEL24_ORDER_COUNT      0xFFFFFE3Cu /* [ebp-0x1C4] */
#define SEL24_ORDER_CURSOR_18  0x18u
#define SEL24_ORDER_CURSOR_1C  0x1Cu
#define SEL24_ORDER_INDEX_18   0xFFFFFDF8u /* [ebp-0x208], cursor 0x18 */
#define SEL24_ORDER_INDEX_1C   0xFFFFFE00u /* [ebp-0x200], cursor 0x1C */

/*
 * Inserted at the entry of the two batch writers 0x6F6BA590 and 0x6F6BA800
 * after `mov esi, [edi]`.  `object` is the object the writer fills, `self` the
 * handler object it was called with, `pair_off` the offset of the handle/serial
 * pair the writer later pushes: 0x38 for the 0x6F6BA590 site, 0x30 for the
 * 0x6F6BA800 site.  Both pairs are printed, so a writer that reads a stale
 * pair is visible even if the offset assumption is wrong.
 */
void wc3_sel24_trace_order_unit(CPU *c, uint32_t pair_off, uint32_t object,
                                uint32_t self)
{
    Sel24CpuState saved;
    uint32_t first = 0u;
    uint32_t o15 = 0u;
    uint32_t o18 = 0u;

    sel24_save_state(c, &saved);
    if (sel24_readable(object)) {
        first = rd32(object);
        o15 = (uint32_t)rd8(object + 0x15u);
        o18 = (uint32_t)rd16(object + 0x18u);
    }
    fprintf(SEL24_OUT,
            "[select24] orderunit pair=%02X self=%08X object=%08X first=%08X "
            "o14=%08X o15=%02X o18=%04X o1C=%08X "
            "o2C=%08X o30=%08X o34=%08X o38=%08X o3C=%08X\n",
            pair_off, self, object, first,
            sel24_rd(object, 0x14u), o15, o18, sel24_rd(object, 0x1Cu),
            sel24_rd(object, 0x2Cu), sel24_rd(object, 0x30u),
            sel24_rd(object, 0x34u), sel24_rd(object, 0x38u),
            sel24_rd(object, 0x3Cu));
    if (sel24_readable(first))
        fprintf(SEL24_OUT,
                "[select24] orderunit pair=%02X first14=%08X first18=%08X "
                "first1C=%08X\n",
                pair_off, rd32(first + 0x14u), rd32(first + 0x18u),
                rd32(first + 0x1Cu));
    sel24_restore_state(c, &saved);
}

/*
 * Inserted after `mov edx, eax` in the two order loops (0x6F6B9E8D and
 * 0x6F6BA4C2).  eax is the event object the per-unit lookup returned and ecx
 * the target unit; a zero event is the unit that silently drops out of the
 * order.  `cursor_off` is 0x18 or 0x1C and selects the matching index slot.
 * The first iteration also dumps the sorted array, so the log shows whether
 * the writer produced twenty-four records at all or only twelve.
 */
void wc3_sel24_trace_order_loop(CPU *c, uint32_t cursor_off, uint32_t cursor,
                                uint32_t unit, uint32_t event)
{
    Sel24CpuState saved;
    uint32_t record = cursor - cursor_off;
    uint32_t index_slot = (cursor_off == SEL24_ORDER_CURSOR_18)
                              ? SEL24_ORDER_INDEX_18
                              : SEL24_ORDER_INDEX_1C;
    int frame_ok = sel24_stack_word(c->ebp);
    uint32_t index = frame_ok ? rd32(c->ebp + index_slot) : 0u;
    uint32_t count = frame_ok ? rd32(c->ebp + SEL24_ORDER_COUNT) : 0u;
    uint32_t object = c->edi;
    uint32_t o15 = 0u;
    uint32_t o18 = 0u;
    uint32_t i;

    sel24_save_state(c, &saved);
    if (sel24_readable(object)) {
        o15 = (uint32_t)rd8(object + 0x15u);
        o18 = (uint32_t)rd16(object + 0x18u);
    }
    fprintf(SEL24_OUT,
            "[select24] orderloop cu=%02X idx=%u count=%u rec=%08X "
            "unit=%08X event=%08X object=%08X o14=%08X o15=%02X o18=%04X "
            "o1C=%08X\n",
            cursor_off, index, count, record, unit, event, object,
            sel24_rd(object, 0x14u), o15, o18, sel24_rd(object, 0x1Cu));
    if (sel24_stack_word(record) &&
        record + SEL24_ORDER_RECORD_STRIDE <= SEL24_GUEST_STACK_HI)
        sel24_trace_order_record("orderrec", index, record);

    if (index == 0u && sel24_stack_word(record)) {
        uint32_t total = count;

        if (total > SEL24_ORDER_RECORD_MAX)
            total = SEL24_ORDER_RECORD_MAX;
        fprintf(SEL24_OUT,
                "[select24] orderdump cu=%02X count=%u base=%08X max=%u\n",
                cursor_off, count, record, SEL24_ORDER_RECORD_MAX);
        for (i = 0; i < total; ++i) {
            uint32_t rec = record + i * SEL24_ORDER_RECORD_STRIDE;

            if (!sel24_stack_word(rec) ||
                rec + SEL24_ORDER_RECORD_STRIDE > SEL24_GUEST_STACK_HI)
                break;
            sel24_trace_order_record("orderdump", i, rec);
        }
    }
    sel24_restore_state(c, &saved);
}

/*
 * Inserted after the element write of the three per-unit assignment lists:
 *   0x6F6BB731  writer 0x6F6BB5F0, class A layout (count +0x4C, array +0x50)
 *   0x6F6BB90C  writer 0x6F6BB8F0, class A layout
 *   0x6F6BBFCC  writer 0x6F6BBEB0, old layout (count +0x28, array +0x2C)
 * The first two share the container the pass already grew to twenty-four
 * slots; the third one still has twelve and its array ends at the security
 * cookie.  tag selects the array base, so the derived index is correct for
 * both layouts, and both counts are printed from the same object.  A point
 * order of fourteen units that leaves the +0x28 list at twelve shows which
 * container the order actually uses; a list whose unit dwords repeat shows a
 * writer that wraps instead of skipping.
 */
void wc3_sel24_trace_order_slots(CPU *c, uint32_t tag, uint32_t list,
                                 uint32_t element)
{
    Sel24CpuState saved;
    uint32_t base = (tag == 3u) ? 0x2Cu : 0x50u;
    uint32_t index = 0u;
    uint32_t command;
    uint32_t unit;

    sel24_save_state(c, &saved);
    if (list != 0u && element >= list + base)
        index = (element - list - base) >> 5;
    command = sel24_rd(list, 0x00u);
    unit = sel24_rd(element, 0x00u);
    fprintf(SEL24_OUT,
            "[select24] slots tag=%u list=%08X elem=%08X idx=%u cmd=%08X "
            "t15=%02X o18=%04X c28=%08X c4C=%08X unit=%08X\n",
            tag, list, element, index, command,
            (sel24_rd(command, 0x14u) >> 8) & 0xFFu,
            sel24_rd(command, 0x18u) & 0xFFFFu,
            sel24_rd(list, 0x28u), sel24_rd(list, 0x4Cu), unit);
    sel24_restore_state(c, &saved);
}

/*
 * Inserted in 0x6F6B93A0 after the calls to 0x6F04CDD0, which are the last
 * points where a per-unit order can still be rejected: the caller returns
 * without touching the unit when the result is zero.  A unit with result=0
 * is a unit that received the order event but did not get the order, which
 * is exactly the class of defect the 12-unit symptom needs.  The function
 * has two delivery phases; phase says which one this result belongs to.
 */
void wc3_sel24_trace_order_apply(CPU *c, uint32_t unit, uint32_t event,
                                 uint32_t result, uint32_t phase)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    fprintf(SEL24_OUT,
            "[select24] apply phase=%u unit=%08X event=%08X result=%08X "
            "unit14=%08X unit18=%08X\n",
            phase, unit, event, result, sel24_rd(unit, 0x14u),
            sel24_rd(unit, 0x18u));
    sel24_restore_state(c, &saved);
}

void wc3_sel24_trace_stale(CPU *c, uint32_t kind, uint32_t a, uint32_t b,
                           uint32_t d)
{
    Sel24CpuState saved;

    if ((kind == 1u || kind == 4u) && !sel24_ident_trace())
        return;
    sel24_save_state(c, &saved);
    fprintf(SEL24_OUT,
            "[select24] stale kind=%u a=%08X b=%08X c=%08X\n", kind, a, b, d);
    sel24_restore_state(c, &saved);
}

/*
 * Inserted in 0x6F6B93A0 around the virtual call that hands the order event
 * to the unit.  Stage 0 prints the unit, the event object, its vtable and the
 * handler that is about to run; stage 1 prints the handler result.  The
 * handler is the implementation that decides what the unit does with the
 * order, so a handler that fails or returns nothing for a particular unit is
 * the remaining explanation for a unit that ignores an accepted order.
 */
void wc3_sel24_trace_order_exec(CPU *c, uint32_t stage, uint32_t unit,
                                uint32_t event, uint32_t vtbl,
                                uint32_t handler, uint32_t result)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    if (stage == 0u) {
        fprintf(SEL24_OUT,
                "[select24] exec unit=%08X event=%08X vtbl=%08X "
                "handler=%08X\n",
                unit, event, vtbl, handler);
    } else {
        fprintf(SEL24_OUT,
                "[select24] exec unit=%08X event=%08X result=%08X\n",
                unit, event, result);
    }
    sel24_restore_state(c, &saved);
}

/*
 * Traces of the selection group removal path.
 *
 * 0x6F28EEF0 removes one unit from the group: it walks the group's unit list
 * and, when the entry is found, decrements the counter at +0x1F8 and unlinks
 * the entry.  The counter is the number the UI and the order builder see, so
 * a unit that dies but is never removed stays in the selection and later
 * receives an order with a dead target.
 *
 * phase 0 is the entry of the removal function (only the group is known
 * there), phase 1 is the actual removal (group, unit and the counter after
 * the decrement).  A removal that enters the function but never reaches
 * phase 1 means the unit was not found in the group's list.
 */
void wc3_sel24_trace_remove(CPU *c, uint32_t phase, uint32_t group,
                            uint32_t unit)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    if (phase == 0u) {
        fprintf(SEL24_OUT,
                "[select24] remove entry group=%08X head=%08X count=%u\n",
                group, sel24_rd(group, 0x1F4u), sel24_rd(group, 0x1F8u));
    } else if (phase == 2u) {
        fprintf(SEL24_OUT,
                "[select24] remove node head=%08X node=%08X count=%u\n",
                group, unit, sel24_rd(group, 0x1F8u));
    } else {
        fprintf(SEL24_OUT,
                "[select24] removeunit group=%08X unit=%08X head=%08X "
                "count=%u\n",
                group, unit, sel24_rd(group, 0x1F4u),
                sel24_rd(group, 0x1F8u));
    }
    sel24_restore_state(c, &saved);
}

/*
 * Traces of the per-unit order application.
 *
 * 0x6F04CDD0 applies the order to one unit and returns 0 when the unit did
 * not take it; L_6F69BD80 then keeps a null event for that unit and the
 * delivery dereferences it (SIGSEGV at 0x0 in L_6F6857E0).  With more than
 * twelve selected units the apply fails for the units past the limit, which
 * is the observed "units beyond twelve obey at random" plus the crash.
 *
 * phase 0 prints the order object and its class tag (0x2B616761 is what the
 * game checks), phase 1 prints the object the apply returned and its +0x78
 * field, which has to point back at the order object for success.
 */
int wc3_sel24_heap_ptr(uint32_t p)
{
    return p >= 0x10000u && p < 0x7F000000u;
}
void wc3_sel24_trace_apply(CPU *c, uint32_t phase, uint32_t a, uint32_t b)
{
    Sel24CpuState saved;

    sel24_save_state(c, &saved);
    if (phase == 0u) {
        fprintf(SEL24_OUT,
                "[select24] orderapply result=%08X source=%08X\n", a, b);
    } else {
        fprintf(SEL24_OUT,
                "[select24] orderev event=%08X source=%08X\n", a, b);
    }
    sel24_restore_state(c, &saved);
}

/*
 * 0x6F154B60 creates a task object through a virtual factory and receives it
 * in eax at 0x6F154B76.  Everything the event chain later resolves goes
 * through the object's +0x10 class tag, +0x14 handle and +0x18 serial, so the
 * birth record separates a registration defect (stale handle at birth) from a
 * lifetime defect (valid handle at birth, stale by the time of delivery).
 * The +0x50/+0x54 fields are printed because 0x6F6926B0 later reads the task
 * payload from +0x54: a zero here means no initialisation slot filled it.
 * `desc` is the descriptor that 0x6F055D00 built (ebx at the call site).
 */
void wc3_sel24_trace_evbirth(CPU *c, uint32_t object, uint32_t desc)
{
    Sel24CpuState saved;
    char probe[160];
    uint32_t klass = 0u;
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    int ok;

    sel24_save_state(c, &saved);
    if (sel24_heap_word(object)) {
        klass = rd32(object + 0x10u);
        handle = rd32(object + 0x14u);
        serial = rd32(object + 0x18u);
    }
    sel24_describe_handle(handle, serial, probe, sizeof(probe), &ok);
    fprintf(SEL24_OUT,
            "[select24] evbirth object=%08X vt=%08X class=%08X handle=%08X "
            "serial=%u f50=%08X f54=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X d24=%08X d28=%08X %s\n",
            object, sel24_rd(object, 0x00u), klass, handle, serial,
            sel24_rd(object, 0x50u), sel24_rd(object, 0x54u), desc,
            sel24_rd(desc, 0x04u), sel24_rd(desc, 0x0Cu),
            sel24_rd(desc, 0x10u), sel24_rd(desc, 0x24u),
            sel24_rd(desc, 0x28u), probe);
    sel24_restore_state(c, &saved);
}

/*
 * 0x6F1CBFB0 releases a slot of the negative handle table.  0x6F1CBFF0 links
 * the slot into the free list with `entry[index].value = *head; *head =
 * index`, so a second release of the same index links it to itself and the
 * next pop moves the head to 0xFFFFFFFE.  `handle` is the signed handle the
 * caller passed (negative for this table), `slotval` is the value stored in
 * the slot right now: 0xFFFFFFFE means the index was already released.
 */
void wc3_sel24_trace_evrelease(CPU *c, uint32_t table, uint32_t handle)
{
    Sel24CpuState saved;
    uint32_t index = ~handle; /* negative table: id is the complement of the slot */
    uint32_t base = 0u;
    uint32_t head = 0u;
    uint32_t count = 0u;
    uint32_t slotval = 0u;
    char probe[96];
    int ok;

    sel24_save_state(c, &saved);
    if (sel24_readable(table)) {
        base = sel24_rd(table, 0x2Cu);
        head = sel24_rd(table, 0x44u);
        count = sel24_rd(table, 0x4Cu);
        if (sel24_readable(base) && index < count)
            slotval = sel24_rd(base, index * 8u);
    }
    sel24_describe_handle(handle, 0u, probe, sizeof(probe), &ok);
    fprintf(SEL24_OUT,
            "[select24] evrelease table=%08X handle=%08X index=%u head=%08X "
            "count=%u slotval=%08X %s\n",
            table, handle, index, head, count, slotval, probe);
    sel24_restore_state(c, &saved);
}
#endif

/*
 * Call one lifted guest function.  args[] is in C order (leftmost first) and
 * is pushed right-to-left.  The return value is c->eax after the call.  The
 * caller's full integer state is restored, including esp, so a callee that
 * cleans its arguments (stdcall) and one that does not (cdecl) behave the
 * same way here.
 */
static uint32_t sel24_invoke(CPU *c, uint32_t target, uint32_t ecx,
                             uint32_t ret, const uint32_t *args,
                             unsigned count)
{
    Sel24CpuState saved;
    uint32_t result;
    unsigned i;

    sel24_save_state(c, &saved);
    c->ecx = ecx;
    for (i = count; i > 0u; --i) {
        push32(c, args[i - 1u]);
    }
    push32(c, ret);
    dispatch(c, target);
    result = c->eax;
    sel24_restore_state(c, &saved);
    return result;
}

static uint32_t sel24_alloc(CPU *c, uint32_t size, uint32_t line,
                            uint32_t ret)
{
    uint32_t args[4];

    args[0] = size;
    args[1] = SEL24_ALLOC_FILE;
    args[2] = line;
    args[3] = 0u;
    return sel24_invoke(c, SEL24_ALLOC, 0u, ret, args, 4u);
}

static void sel24_grid_ctor(CPU *c, uint32_t grid, uint32_t parent)
{
    uint32_t args[1];

    args[0] = parent;
    (void)sel24_invoke(c, SEL24_GRID_CTOR, grid, SEL24_RET_GRID_CTOR,
                       args, 1u);
}

static void sel24_grid_resize(CPU *c, uint32_t grid, uint32_t rows,
                              uint32_t cols)
{
    uint32_t args[2];

    args[0] = rows;
    args[1] = cols;
    (void)sel24_invoke(c, SEL24_GRID_RESIZE, grid, SEL24_RET_RESIZE,
                       args, 2u);
}

static void sel24_grid_insert(CPU *c, uint32_t grid, uint32_t col,
                              uint32_t row, uint32_t button)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = button;
    args[3] = 1u; /* flag: SetParent was already done by the button ctor */
    (void)sel24_invoke(c, SEL24_GRID_INSERT, grid, SEL24_RET_INSERT,
                       args, 4u);
}

static void sel24_grid_width(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel24_invoke(c, SEL24_GRID_WIDTH, grid, SEL24_RET_WIDTH, args, 1u);
}

static void sel24_grid_height(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel24_invoke(c, SEL24_GRID_HEIGHT, grid, SEL24_RET_HEIGHT,
                       args, 1u);
}

static void sel24_grid_anchor(CPU *c, uint32_t grid, uint32_t child_anchor,
                              uint32_t parent, uint32_t parent_anchor,
                              uint32_t x, uint32_t y, uint32_t flag)
{
    uint32_t args[6];

    args[0] = child_anchor;
    args[1] = parent;
    args[2] = parent_anchor;
    args[3] = x;
    args[4] = y;
    args[5] = flag;
    (void)sel24_invoke(c, SEL24_GRID_ANCHOR, grid, SEL24_RET_ANCHOR,
                       args, 6u);
}

static void sel24_button_ctor(CPU *c, uint32_t button, uint32_t grid)
{
    uint32_t args[1];

    args[0] = grid;
    (void)sel24_invoke(c, SEL24_BUTTON_CTOR, button, SEL24_RET_BUTTON,
                       args, 1u);
}

static void sel24_fill_cell(CPU *c, uint32_t group, uint32_t col,
                            uint32_t row, uint32_t unit, uint32_t flag)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = unit;
    args[3] = flag;
    (void)sel24_invoke(c, SEL24_FILL_CELL, group, SEL24_RET_FILL, args, 4u);
}

static void sel24_frame_method(CPU *c, uint32_t frame, uint32_t slot,
                               uint32_t ret, uint32_t visible)
{
    uint32_t vtable;
    uint32_t method;

    if (!frame) {
        return;
    }
    wr32(frame + SEL24_FRAME_VISIBLE, visible);
    vtable = rd32(frame);
    if (!vtable) {
        return;
    }
    method = rd32(vtable + slot);
    if (method) {
        (void)sel24_invoke(c, method, frame, ret, NULL, 0u);
    }
}

static uint32_t sel24_valid_grid(uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t active;

    if (!group) {
        return 0u;
    }
    /* Before init the added fields are outside the original 0x134 allocation
     * and may contain garbage; the original grid12 pointer is still valid. */
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return rd32(group + SEL24_GROUP_GRID12);
    }
    grid12 = rd32(group + SEL24_GROUP_GRID12);
    grid24 = rd32(group + SEL24_GROUP_GRID24);
    active = rd32(group + SEL24_GROUP_ACTIVE);
    if (active == grid12 || (grid24 && active == grid24)) {
        return active;
    }
    return grid12;
}

/*
 * Hide every real button of one grid without touching the grid frame.  The
 * original group hide-all assumes all 12 cells exist; the 24-cell grid can
 * contain null cells after a partial allocation, so every level is checked.
 */
static void sel24_hide_grid(CPU *c, uint32_t grid)
{
    uint32_t rows;
    uint32_t cols;
    uint32_t row_array;
    uint32_t row;
    uint32_t col;

    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL24_GRID_ROWS);
    cols = rd32(grid + SEL24_GRID_COLS);
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array || rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }

    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL24_ROW_STRIDE;
        uint32_t buttons = rd32(row_desc + SEL24_ROW_BUTTONS);
        if (!buttons) {
            continue;
        }
        for (col = 0u; col < cols; ++col) {
            uint32_t button = rd32(buttons + col * 4u);
            if (button) {
                sel24_frame_method(c, button, SEL24_VT_HIDE, SEL24_RET_HIDE,
                                   0u);
            }
        }
    }
}

/*
 * Make sure the active grid frame is registered again after a parent
 * HideAll/ShowAll cycle.  Buttons are re-hidden first: ShowAll recurses into
 * children that have their own visible flag set, and stale buttons must not
 * survive an update that only fills part of the grid.
 */
static void sel24_ensure_visible(CPU *c, uint32_t grid)
{
    if (!grid) {
        return;
    }
    if (rd32(grid + SEL24_FRAME_VISIBLE) == 0u ||
        rd32(grid + SEL24_FRAME_REGISTERED) == 0u) {
        sel24_hide_grid(c, grid);
        sel24_frame_method(c, grid, SEL24_VT_SHOW, SEL24_RET_SHOW, 1u);
    }
}

/*
 * Check that the resize really produced the requested rows/cols and that
 * every row has a button array.  This keeps the helper fail-closed when the
 * grid or a row array could not be allocated.
 */
static int sel24_grid_ready(uint32_t grid, uint32_t rows, uint32_t cols)
{
    uint32_t row_array;
    uint32_t row;

    if (!grid) {
        return 0;
    }
    if (rd32(grid + SEL24_GRID_ROWS) != rows ||
        rd32(grid + SEL24_GRID_COLS) != cols) {
        return 0;
    }
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array) {
        return 0;
    }
    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL24_ROW_STRIDE;
        if (!rd32(row_desc + SEL24_ROW_BUTTONS)) {
            return 0;
        }
    }
    return 1;
}

void wc3_sel24_init_group(CPU *c, uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t row;
    uint32_t col;

    if (!group) {
        return;
    }
    if (rd32(group + SEL24_GROUP_INIT) == SEL24_INIT_MAGIC) {
        return;
    }

    grid12 = rd32(group + SEL24_GROUP_GRID12);
    wr32(group + SEL24_GROUP_INIT, SEL24_INIT_MAGIC);
    wr32(group + SEL24_GROUP_GRID24, 0u);
    wr32(group + SEL24_GROUP_ACTIVE, grid12);
    wr32(group + SEL24_GROUP_TOTAL, 0u);
    if (wc3_sel24_cap() < 24u) {
        /* Runtime switch is off: keep the original 6x2 path and do not
         * allocate the extra grid.  The marker is already set, so note/select/
         * fill resolve to grid12 without touching the missing +0x134 slot. */
#if WC3_DEBUG && WC3_DEBUG_SELECT24
        fprintf(SEL24_OUT, "[select24] init group=%08X grid12=%08X mode=12\n",
                group, grid12);
#endif
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] init group=%08X grid12=%08X marker=%08X\n",
            group, grid12, (unsigned)SEL24_INIT_MAGIC);
#endif
    if (!grid12) {
        return;
    }

    grid24 = sel24_alloc(c, SEL24_GRID_SIZE, SEL24_GRID_LINE,
                         SEL24_RET_GRID_ALLOC);
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT, "[select24] init group=%08X grid24=%08X\n",
            group, grid24);
#endif
    if (!grid24) {
        return;
    }

    sel24_grid_ctor(c, grid24, group);
    wr32(grid24 + SEL24_GRID_SCALE, SEL24_GRID24_SCALE);
    sel24_grid_width(c, grid24, SEL24_GRID24_WIDTH);
    sel24_grid_height(c, grid24, SEL24_GRID24_HEIGHT);
    sel24_grid_anchor(c, grid24, 7u, group, 7u,
                      SEL24_GRID24_OFFSET_X, SEL24_GRID24_OFFSET_Y, 1u);
    sel24_grid_resize(c, grid24, SEL24_GRID24_ROWS, SEL24_GRID24_COLS);
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] init group=%08X grid24=%08X ready=%d rows=%u cols=%u\n",
            group, grid24,
            sel24_grid_ready(grid24, SEL24_GRID24_ROWS,
                             SEL24_GRID24_COLS),
            (unsigned)SEL24_GRID24_ROWS, (unsigned)SEL24_GRID24_COLS);
#endif

    if (!sel24_grid_ready(grid24, SEL24_GRID24_ROWS, SEL24_GRID24_COLS)) {
        /* The partial grid stays a hidden child of the group.  It is not
         * referenced by the group fields, so the 6x2 path remains intact. */
        wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
        sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
        sel24_hide_grid(c, grid24);
        wr32(group + SEL24_GROUP_GRID24, 0u);
        wr32(group + SEL24_GROUP_ACTIVE, grid12);
        return;
    }

    wr32(group + SEL24_GROUP_GRID24, grid24);
    wr32(group + SEL24_GROUP_ACTIVE, grid12);

    for (row = 0u; row < SEL24_GRID24_ROWS; ++row) {
        for (col = 0u; col < SEL24_GRID24_COLS; ++col) {
            uint32_t button = sel24_alloc(c, SEL24_BUTTON_SIZE,
                                          SEL24_BUTTON_LINE,
                                          SEL24_RET_BUTTON_ALLOC);
            if (!button) {
                wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
                sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE,
                                   0u);
                sel24_hide_grid(c, grid24);
                wr32(group + SEL24_GROUP_GRID24, 0u);
                wr32(group + SEL24_GROUP_ACTIVE, grid12);
                return;
            }
            sel24_button_ctor(c, button, grid24);
            sel24_grid_insert(c, grid24, col, row, button);
        }
    }

    /* The 8x3 grid is a dormant child until the first 13..24 update. */
    wr32(grid24 + SEL24_GRID_VISIBLE, 0u);
    sel24_frame_method(c, grid24, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
    sel24_hide_grid(c, grid24);
    wr32(grid12 + SEL24_GRID_VISIBLE, 1u);
}

/*
 * Called from the update prologue with the CSelectionWar3 object.  The full
 * update function and the split entry fragments share the same stack frame,
 * but only the prologue has the selection pointer.  Keep the total count in
 * the group object so the mode choice works in every fragment.
 */
void wc3_sel24_note_selection(CPU *c, uint32_t group, uint32_t selection)
{
    uint32_t total = 0u;

    (void)c;
    if (!group) {
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] note group=%08X marker=%08X grid12=%08X "
            "selection=%08X\n",
            group, rd32(group + SEL24_GROUP_INIT),
            rd32(group + SEL24_GROUP_GRID12), selection);
#endif
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return;
    }
    /* init only writes the marker after it has seen a real grid12; requiring
     * the same pointer here keeps a chance marker match on a short allocation
     * from turning into an out-of-bounds write at +0x140. */
    if (!rd32(group + SEL24_GROUP_GRID12)) {
        return;
    }
    if (selection) {
        total = rd32(selection + SEL24_SELECTION_TOTAL);
    }
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] note group=%08X selection=%08X total=%u stored=%u "
            "cap=%u\n",
            group, selection, total, (unsigned)SEL24_SELECTION_TOTAL,
            wc3_sel24_cap());
    /* The selection entries live in a doubly linked list rooted at
     * selection+0x1F4: +0 is the next link, +4 the previous link (a
     * self-relative offset for the first node, an absolute pointer otherwise)
     * and +8 the refcounted value; 0x6F28F0D8 rewrites the neighbours in
     * exactly that layout.  The head can already point at a node whose memory
     * was released - that is the stale-entry defect this profile guards
     * against - so a linear walk dereferences a freed first word; the build
     * before this change faulted at 0x1A that way.  Report the root entry only
     * and range-check every field; per-unit data comes from the order batch
     * trace, which sees every selected unit by construction. */
    if (selection) {
        uint32_t root = rd32(selection + SEL24_SELECTION_HEAD);

        if (!sel24_in_range(root, SEL24_GUEST_HEAP_LO, 0x70000000u) ||
            (root & 3u) != 0u) {
            fprintf(SEL24_OUT,
                    "[select24] seldump root=%08X bad-root total=%u\n", root,
                    total);
        } else {
            uint32_t obj = rd32(root + 8u);

            if (!sel24_in_range(obj, SEL24_GUEST_HEAP_LO, 0x70000000u) ||
                (obj & 3u) != 0u) {
                fprintf(SEL24_OUT,
                        "[select24] seldump root=%08X obj=%08X bad-object "
                        "total=%u\n",
                        root, obj, total);
            } else {
                uint32_t handle = rd32(obj + 0x0Cu);
                uint32_t serial = rd32(obj + 0x10u);
                char probe[160];
                int ok = 0;

                sel24_describe_handle(handle, serial, probe, sizeof(probe),
                                      &ok);
                fprintf(SEL24_OUT,
                        "[select24] seldump root=%08X obj=%08X handle=%08X "
                        "serial=%u total=%u %s\n",
                        root, obj, handle, serial, total, probe);
            }
        }
    }
#endif
    wr32(group + SEL24_GROUP_TOTAL, total);
}

void wc3_sel24_select(CPU *c, uint32_t group, uint32_t subgroup_units)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t desired;
    uint32_t old;
    uint32_t total;

    if (!group) {
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] select group=%08X marker=%08X subgroup=%u\n",
            group, rd32(group + SEL24_GROUP_INIT), subgroup_units);
#endif
    if (rd32(group + SEL24_GROUP_INIT) != SEL24_INIT_MAGIC) {
        return;
    }
    grid12 = rd32(group + SEL24_GROUP_GRID12);
    grid24 = rd32(group + SEL24_GROUP_GRID24);
    if (!grid12) {
        return;
    }

    total = rd32(group + SEL24_GROUP_TOTAL);
    if (total == 0u || total > 24u) {
        /* The split-entry fragment may run without the update prologue.  The
         * current subgroup's unit count is a safe lower bound: it never asks
         * for more cells than this fragment can fill. */
        total = subgroup_units;
    }

    desired = (wc3_sel24_cap() >= 24u &&
               total > SEL24_GRID12_ROWS * SEL24_GRID12_COLS && grid24)
                  ? grid24
                  : grid12;

    old = rd32(group + SEL24_GROUP_ACTIVE);
    if (old != grid12 && (grid24 == 0u || old != grid24)) {
        old = grid12;
        wr32(group + SEL24_GROUP_ACTIVE, old);
    }
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] select group=%08X subgroup=%u total=%u grid12=%08X "
            "grid24=%08X active=%08X desired=%08X\n",
            group, subgroup_units, total, grid12, grid24, old, desired);
#endif
    if (desired == old) {
        /* A parent HideAll can unregister the active grid; ShowAll would not
         * restore it because its own visible flag is zero. */
        sel24_ensure_visible(c, desired);
        return;
    }

    /* Remove stale buttons from the target grid first, then swap frames.
     * The fill path that follows shows only the buttons it actually fills. */
    sel24_hide_grid(c, desired);
    sel24_frame_method(c, old, SEL24_VT_HIDE, SEL24_RET_HIDE, 0u);
    sel24_frame_method(c, desired, SEL24_VT_SHOW, SEL24_RET_SHOW, 1u);
    wr32(group + SEL24_GROUP_ACTIVE, desired);
}

void wc3_sel24_hide_buttons(CPU *c, uint32_t group)
{
    uint32_t grid = sel24_valid_grid(group);

#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT, "[select24] hide group=%08X grid=%08X\n",
            group, grid);
#endif
    if (grid) {
        sel24_hide_grid(c, grid);
    }
}

void wc3_sel24_fill(CPU *c, uint32_t group, uint32_t index,
                    uint32_t unit, uint32_t flag)
{
    uint32_t grid;
    uint32_t rows;
    uint32_t cols;
    uint32_t row_array;
    uint32_t row;
    uint32_t col;
    uint32_t row_desc;
    uint32_t buttons;
    uint32_t button;

    if (!group) {
        return;
    }
    grid = sel24_valid_grid(group);
#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT, "[select24] fill group=%08X index=%u grid=%08X\n",
            group, index, grid);
#endif
    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL24_GRID_ROWS);
    cols = rd32(grid + SEL24_GRID_COLS);
    if (rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }
    if (index >= rows * cols) {
        return;
    }

    row = index / cols;
    col = index % cols;
    row_array = rd32(grid + SEL24_GRID_ROWS_AT);
    if (!row_array) {
        return;
    }
    row_desc = row_array + row * SEL24_ROW_STRIDE;
    buttons = rd32(row_desc + SEL24_ROW_BUTTONS);
    if (!buttons) {
        return;
    }
    button = rd32(buttons + col * 4u);
    if (!button) {
#if WC3_DEBUG && WC3_DEBUG_SELECT24
        fprintf(SEL24_OUT,
                "[select24] fill group=%08X index=%u button=0\n",
                group, index);
#endif
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_SELECT24
    fprintf(SEL24_OUT,
            "[select24] fill group=%08X index=%u unit=%08X flag=%u grid=%08X "
            "rows=%u cols=%u row=%u col=%u button=%08X\n",
            group, index, unit, flag, grid, rows, cols, row, col, button);
#endif
    sel24_fill_cell(c, group, col, row, unit, flag);
}

/*
 * 24-entry ground-order formation workspace.
 *
 * wc3_sel24_init24 replaces the L_6F169300 -> L_6F169310 zeroing sequence for
 * the widened solver frame: the original zeroes 12 pairs (2 * 0x30 bytes),
 * this one zeroes 24 pairs at count+0x64.
 */
void wc3_sel24_init24(uint32_t pair_base)
{
    uint32_t i;

    for (i = 0u; i < 24u; ++i) {
        wr32(pair_base + i * 8u, 0u);
        wr32(pair_base + i * 8u + 4u, 0u);
    }
}

/* Bit-exact float load for the sort comparison. */
static float sel24_f32(uint32_t bits)
{
    union {
        uint32_t u;
        float f;
    } v;

    v.u = bits;
    return v.f;
}

/*
 * wc3_sel24_sort24 is a transliteration of L_6F16DFD0 with the 24-entry
 * workspace layout: count at count_ptr+0, index at count_ptr+4 (24 dwords)
 * and pair at count_ptr+0x64 (24 pairs, second dword at +0x68).  The control
 * flow, comparisons, swap/rotate and index updates are the lifted ones; only
 * the two pair-base displacements differ from the original 12-entry sort.
 * L_6F16DFD0 itself is left untouched for the 12-entry caller 0x6F16A6E0.
 */
void wc3_sel24_sort24(uint32_t count_ptr)
{
    uint32_t count;
    uint32_t pair_end;
    uint32_t index_end;
    uint32_t saved_index;
    uint32_t saved_pair;
    uint32_t key_orig;
    uint32_t key_bits;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ebp_edi;
    uint32_t esi;
    uint32_t eax;
    uint32_t ecx;

    count = rd32(count_ptr);
    if (count <= 1u)
        return;

    pair_end = count_ptr + 0x64u + count * 8u;
    index_end = count_ptr + 4u + count * 4u;
    ebp_edi = count;

    for (;;) {
        pair_end -= 8u;
        index_end -= 4u;
        ebp_edi -= 1u;
        saved_index = index_end;
        saved_pair = pair_end;
        ebx = ebp_edi;
        edx = ebp_edi;
        key_orig = rd32(pair_end);
        key_bits = key_orig;

        if (ebp_edi != 0u) {
            esi = pair_end;
            for (;;) {
                esi -= 8u;
                edx -= 1u;
                if (!(sel24_f32(key_bits) > rdss(esi)))
                    break;
                ebx = edx;
                key_bits = rd32(esi);
                if (edx == 0u)
                    break;
            }

            if (ebx != ebp_edi) {
                /* saved_index and saved_pair are the guest slot addresses
                 * themselves ([ebp-8]/[ebp-4] in the lifted body), not
                 * pointers to those addresses. */
                eax = saved_index;
                ecx = count_ptr;
                esi = rd32(eax);
                edx = rd32(ecx + ebx * 4u + 4u);
                eax = saved_pair;
                ecx = saved_index;
                eax = rd32(eax + 4u);
                wr32(ecx, edx);
                ecx = count_ptr;
                edx = rd32(ecx + ebx * 8u + 0x64u);
                ecx = saved_pair;
                wr32(ecx, edx);
                edx = rd32(count_ptr + ebx * 8u + 0x68u);
                wr32(ecx + 4u, edx);
                edx = count_ptr;
                ecx = key_orig;
                wr32(edx + ebx * 4u + 4u, esi);
                wr32(edx + ebx * 8u + 0x64u, ecx);
                wr32(edx + ebx * 8u + 0x68u, eax);
            }
        }

        if (ebp_edi <= 1u)
            break;
    }
}
