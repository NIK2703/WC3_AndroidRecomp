/*
 * selection.c - 12/24/40-unit selection and adaptive 6x2 / 8x3 / 10x4 panel.
 *
 * The lifted CInfoPanelGroup and CSelectionWar3 code stays in the generated
 * corpus.  This module supplies the host helpers that the
 * tools/lift/csrc_local/selection.c pass calls from the addresses listed in
 * docs/research/SELECTION_24_ANALYSIS.md.  The formation helpers
 * wc3_sel_workspace_init/wc3_sel_workspace_sort operate on guest memory directly; they do
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
#include "selection.h"
#include "wc3_debug.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* The corpus always carries the 40-entry structural layout.  The runtime
 * WC3_SELECTION_CAP only lowers how many slots the game fills. */
#define SEL_STRUCT_CAP 40u

/* Debug-only trace.  Every diagnostic write below is inside an explicit
 * WC3_DEBUG && WC3_DEBUG_SELECTION guard; the tag names the observed
 * subsystem and release code contains no logging call at all. */
#if WC3_DEBUG && WC3_DEBUG_SELECTION
#include <stdio.h>
#ifdef __ANDROID__
/*
 * Debug-only selection log.  run.txt mixes the HLE, DXVK and crash
 * diagnostics into one huge stream; this pass has its own file in the
 * app-private state directory so the events that lead to a crash can be read
 * without filtering run.txt.  It is opened once, truncated per process, line
 * buffered and falls back to stderr when WC3_DATA_DIR is unavailable.
 */
#include <pthread.h>
#include <sys/mman.h>
static FILE *g_sel_log;
static pthread_once_t g_sel_log_once = PTHREAD_ONCE_INIT;

static void sel_open_log(void)
{
    const char *dir = getenv("WC3_DATA_DIR");
    char path[1024];

    if (dir && *dir)
        snprintf(path, sizeof(path), "%s/sel_trace.txt", dir);
    else
        snprintf(path, sizeof(path), "sel_trace.txt");
    g_sel_log = fopen(path, "w");
    if (!g_sel_log)
        return;
    setvbuf(g_sel_log, NULL, _IOLBF, 0);
    fprintf(g_sel_log, "[select24] trace start\n");
}

static FILE *sel_log(void)
{
    pthread_once(&g_sel_log_once, sel_open_log);
    return g_sel_log ? g_sel_log : stderr;
}

#define SEL_OUT sel_log()
#else
#define SEL_OUT stderr
#endif
#endif /* WC3_DEBUG && WC3_DEBUG_SELECTION */

#if WC3_DEBUG && WC3_DEBUG_SELECTION
/*
 * The event-birth/event-release, per-unit order and sort-workspace traces are
 * opt-in.  They describe paths that are already ruled out for the 12-unit
 * cluster failure and cost tens of megabytes during a save load: the first
 * debug run produced a 164 MB trace and stalled the guest.  The targeted
 * order/cluster/list/formation records stay enabled.  Set
 * WC3_SEL_VERBOSE=1 in the launch environment to bring the detailed
 * records back for a future investigation.
 */
static int sel_verbose_trace(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("WC3_SEL_VERBOSE");

        enabled = (value && value[0] == '1' && value[1] == '\0') ? 1 : 0;
    }
    return enabled;
}
#endif

/* One definition in the helper library: the product build links this module
 * whenever the selection profile is ON, so the marker is present exactly
 * once. */
#if defined(__GNUC__)
__attribute__((used))
#endif
const char wc3_selection_build_marker[] = "WC3_SELECTION";

/* Runtime 12/24/40 cap.  Zero means "not latched yet"; every limit site and
 * the panel helper read the same value for the process lifetime.  The Android
 * host exports WC3_SELECTION_CAP=12|24|40 before android_main.  A missing
 * variable, or any other value, keeps the product default of the original
 * 12 units. */
static uint32_t g_sel_cap;

static uint32_t sel_parse_cap(const char *value)
{
    if (value && value[0] == '1' && value[1] == '2' && value[2] == '\0')
        return 12u;
    if (value && value[0] == '2' && value[1] == '4' && value[2] == '\0')
        return 24u;
    if (value && value[0] == '4' && value[1] == '0' && value[2] == '\0')
        return 40u;
    return 0u;
}

uint32_t wc3_sel_cap(void)
{
    uint32_t cap = g_sel_cap;

    if (cap == 0u) {
        const char *value = getenv("WC3_SELECTION_CAP");

        cap = sel_parse_cap(value);
        if (cap == 0u)
            cap = 12u;
        if (cap > SEL_STRUCT_CAP)
            cap = SEL_STRUCT_CAP;
        g_sel_cap = cap;
    }
    return cap;
}

/* The third dword of every PrClGen slot is initialised to zero and is not
 * used by the order path.  Slot 0 uses it as the save-stream width marker:
 * 0x57430000 | cap.  A legacy stream has zero there, so the reader keeps the
 * pre-40 24-slot default.  The marker is stored in the stream only; the
 * writer never modifies the live object, and the reader clears the field
 * after consuming it. */
#define SEL_STREAM_MARKER      0x57430000u
#define SEL_STREAM_MARKER_MASK 0xFFFFFF00u
#define SEL_STREAM_LEGACY_CAP  24u
#define SEL_STREAM_SLOT0_SERIAL_AT 0x20u
#define SEL_STREAM_SLOT0_THIRD_AT  0x24u
#define SEL_STREAM_BOOLS_AT        0x1FCu

uint32_t wc3_sel_stream_default_cap(void)
{
    return SEL_STREAM_LEGACY_CAP;
}

uint32_t wc3_sel_stream_slot_write(uint32_t self, uint32_t slot_ptr,
                                     uint32_t value)
{
    if (slot_ptr == self + SEL_STREAM_SLOT0_THIRD_AT)
        return SEL_STREAM_MARKER | (wc3_sel_cap() & 0xFFu);
    return value;
}

void wc3_sel_stream_slot_read(uint32_t slot_serial, uint32_t bool_base,
                                uint32_t cap_addr)
{
    uint32_t marker;
    uint32_t cap;

    /* The helper is emitted at every slot read.  Only slot 0 carries the
     * marker; bool_base is the slot-valid array at self+0x1FC. */
    if (slot_serial != bool_base - (SEL_STREAM_BOOLS_AT -
                                    SEL_STREAM_SLOT0_SERIAL_AT))
        return;
    marker = rd32(slot_serial + 4u);
    if ((marker & SEL_STREAM_MARKER_MASK) != SEL_STREAM_MARKER)
        return;
    cap = marker & 0xFFu;
    if (cap != 12u && cap != 24u && cap != 40u)
        return;
    if (cap > SEL_STRUCT_CAP)
        return; /* keep the read within the fixed 40-entry layout */
    wr32(cap_addr, cap);
    wr32(slot_serial + 4u, 0u);
}

/*
 * CPrCluster member vector at cluster+0x1C:
 *
 *   +0x00  vtable 0x6FA90D5C
 *   +0x0C  data base (element array)
 *   +0x14  growth chunk in elements (zero means fixed size)
 *   +0x18  capacity in elements
 *   +0x1C  count in elements
 *
 * The member vector is originally a fixed 12-slot vector: the constructor at
 * 0x6F14FC40 writes capacity 12 (or the runtime 24 cap) and growth chunk zero.
 * Live 24-profile runs still see 12-capacity clusters from the pool, so the
 * insertion at 0x6F16C077 returns zero from 0x6F16C0AA once twelve members are
 * present.  The caller 0x6F16B7B0 ignores that result and unlinks the unit from
 * PrClGen, which is the "only the first twelve units obey" failure for a
 * ground/formation order.  Giving the vector the original 12-element growth
 * chunk activates the game's own grow path: 0x6F16C077 computes the new size,
 * 0x6F1C5130 reallocates and frees the old block, and 0x6F16C0B3 installs the
 * new capacity/count and copies the new members.  A 12-capacity cluster grows
 * 12 -> 24 on the first overflow; a larger vector keeps growing by another 12
 * when it already holds members, so a cluster reused across orders can still
 * accept a full 40-unit selection within the fixed 40-entry structure.
 *
 * Only the CPrCluster member vector is touched.  Both its vtable and the vtable
 * of the owning cluster are checked, because 0x6F16C077 is the generic insert
 * for this 0x2C-byte element type and other fixed vectors must not become
 * growable.  The baseline 12-unit profile returns before reading memory.
 */
#define SEL_CLUSTER_VECTOR_AT   0x1Cu
#define SEL_CLUSTER_VECTOR_VT   0x6FA90D5Cu
#define SEL_CLUSTER_VT          0x6FA90D64u
#define SEL_CLUSTER_VT_EMBEDDED 0x6FA91964u

/* CPrCluster/PrClGen tail in the compiled structural layout.  The cap only
 * limits how many of the available slots the game fills. */
#define SEL_CLUSTER_PTRS  0x1FCu
#define SEL_CLUSTER_TAIL  0x29Cu
#define SEL_CLUSTER_FLAGS 0x2C0u
#define SEL_VECTOR_GROW         0x14u
#define SEL_VECTOR_GROW_CHUNK   12u

void wc3_sel_cluster_prepare(uint32_t vector)
{
    uint32_t owner_vtable;

    if (wc3_sel_cap() < 24u || vector < SEL_CLUSTER_VECTOR_AT)
        return;
    if (rd32(vector) != GVA(SEL_CLUSTER_VECTOR_VT))
        return;
    owner_vtable = rd32(vector - SEL_CLUSTER_VECTOR_AT);
    if (owner_vtable != GVA(SEL_CLUSTER_VT) &&
        owner_vtable != GVA(SEL_CLUSTER_VT_EMBEDDED))
        return;
    if (rd32(vector + SEL_VECTOR_GROW) == 0u)
        wr32(vector + SEL_VECTOR_GROW, SEL_VECTOR_GROW_CHUNK);
}

/* CInfoPanelGroup fields added by the selection profile.  The extra 10x4
 * child exists in the fixed 40-entry allocation. */
#define SEL_GROUP_GRID12   0x130u
#define SEL_GROUP_GRID24   0x134u
#define SEL_GROUP_ACTIVE   0x138u
#define SEL_GROUP_INIT     0x13Cu
#define SEL_GROUP_TOTAL    0x140u
#define SEL_GROUP_GRID40   0x144u

/* Selection-group entry: count +0, index[40] +4, pair[40] +pair_base
 * (key/value dwords), tail after the pair array. */
#define SEL_GROUP_ENTRY_SIZE 0x1F4u
#define SEL_GROUP_PAIR       0xA4u
#define SEL_GROUP_TAIL       0x1E4u
#define SEL_GROUP_INDEX      0x4u

/* CSelectionWar3 field with the total number of selected units. */
#define SEL_SELECTION_TOTAL 0x1F8u
/* Head of the selection's unit list.  Every node is {next, prev, object}; the
 * object keeps the unit handle at +0x0C and the unit serial at +0x10. */
#define SEL_SELECTION_HEAD 0x1F4u

/* CSimpleGrid / CSimpleFrame fields used by the original code. */
#define SEL_GRID_ROWS      0x124u
#define SEL_GRID_COLS      0x128u
#define SEL_GRID_SCALE     0x060u
#define SEL_GRID_VISIBLE   0x090u
#define SEL_GRID_ROWS_AT   0x154u
#define SEL_FRAME_VISIBLE  0x090u
#define SEL_FRAME_REGISTERED 0x094u

/* Row descriptor: 0x10 bytes, button array pointer at +8, button stride 4. */
#define SEL_ROW_STRIDE     0x010u
#define SEL_ROW_BUTTONS    0x008u

#define SEL_BUTTON_VISIBLE 0x090u

/* Size of the widened CInfoPanelGroup allocation. */
#define SEL_GROUP_SIZE     0x150u

#define SEL_GRID_SIZE      0x15Cu
#define SEL_GRID_LINE      0x024u
#define SEL_BUTTON_SIZE    0x140u
#define SEL_BUTTON_LINE    0x076u
#define SEL_ALLOC_FILE     0x6FABF9F8u

/* Original grid12 geometry and the scaled 8x3 / 10x4 grids.  grid24 keeps the
 * committed 0.80 scale and screen footprint.  grid40 keeps its local 10x4
 * extents and uses 0.67 instead of the committed 0.65, so only the 40-unit
 * icons are enlarged; 0.67 keeps the 0.209375-wide screen footprint well
 * inside the bottom panel.  The grid anchor's local +Y points up, so grid40
 * gets -0.006 local (-0.00402 screen) to move the top row down into the free
 * space at the bottom of the black panel.  grid24 stays anchored
 * bottom-centre (7/7) with no Y offset.  The X offset preserves the original
 * -0.0015625 screen-space centre correction at the selected scale. */
#define SEL_GRID12_WIDTH   0x3E3E6666u
#define SEL_GRID12_HEIGHT  0x3DD00000u
#define SEL_GRID24_WIDTH   0x3E7DDDDDu
#define SEL_GRID24_HEIGHT  0x3E0AAAABu
#define SEL_GRID24_OFFSET_X 0xBB000000u /* -0.0015625 / 0.80 */
#define SEL_GRID24_OFFSET_Y 0x00000000u
#define SEL_GRID24_SCALE   0x3F4CCCCDu /* 0.80f */
#define SEL_GRID40_WIDTH   0x3EA00000u /* 0.3125f */
#define SEL_GRID40_HEIGHT  0x3E3C6A7Fu /* 0.184f */
#define SEL_GRID40_OFFSET_X 0xBB18D5F8u /* -0.0015625 / 0.67 */
#define SEL_GRID40_OFFSET_Y 0xBBC49BA6u /* -0.006 local = -0.00402 screen (down) */
#define SEL_GRID40_SCALE   0x3F2B851Fu /* 0.67f */

#define SEL_GRID24_ROWS    3u
#define SEL_GRID24_COLS    8u
#define SEL_GRID40_ROWS    4u
#define SEL_GRID40_COLS    10u
#define SEL_GRID12_ROWS    2u
#define SEL_GRID12_COLS    6u

/* Guest entry points.  All are regular lifted functions. */
#define SEL_ALLOC          0x6F07C6D2u
#define SEL_GRID_CTOR      0x6F10E230u
#define SEL_GRID_RESIZE    0x6F10EE50u
#define SEL_GRID_INSERT    0x6F10E490u
#define SEL_GRID_WIDTH     0x6F1116C0u
#define SEL_GRID_HEIGHT    0x6F111520u
#define SEL_GRID_ANCHOR    0x6F111600u
#define SEL_BUTTON_CTOR    0x6F39EDC0u
#define SEL_FILL_CELL      0x6F3DA470u
#define SEL_FRAME_HIDE     0x6F10D410u
#define SEL_FRAME_SHOW     0x6F10DE60u

/* Real return addresses from the corresponding original call sites. */
#define SEL_RET_GRID_ALLOC 0x6F39ED29u
#define SEL_RET_BUTTON_ALLOC 0x6F3B91F5u
#define SEL_RET_GRID_CTOR  0x6F39ED3Fu
#define SEL_RET_WIDTH      0x6F39ED5Cu
#define SEL_RET_HEIGHT     0x6F39ED6Fu
#define SEL_RET_ANCHOR     0x6F39ED93u
#define SEL_RET_RESIZE     0x6F39EDA2u
#define SEL_RET_BUTTON     0x6F3B9213u
#define SEL_RET_INSERT     0x6F3B922Eu
#define SEL_RET_FILL       0x6F3C7E99u
#define SEL_RET_HIDE       0x6F3ACCE2u
#define SEL_RET_SHOW       0x6F10DEC4u

/* The group is allocated without zeroing, so the marker is a magic value.
 * A fresh object is initialised once; the marker prevents a second grid24
 * from being created (or a failed allocation from being retried) if the tail
 * is reached twice. */
#define SEL_INIT_MAGIC     0x53454C32u /* "SEL2" */

/* Vtable slots of CSimpleFrame. */
#define SEL_VT_HIDE        0x064u
#define SEL_VT_SHOW        0x068u

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eip;
    uint32_t cf, zf, sf, of, pf, af;
} Sel24CpuState;

static void sel_save_state(CPU *c, Sel24CpuState *s)
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

static void sel_restore_state(CPU *c, const Sel24CpuState *s)
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

#if WC3_DEBUG && WC3_DEBUG_SELECTION
/* Sequence of the point-order request currently being built.  The Android
 * game issues group orders on the control thread; the counter only ties the
 * formation/list/unit records of one request together in the log. */
static uint32_t g_sel_order_seq;

/*
 * The LiftPass inserts this call immediately after the original AddUnit cap
 * comparison.  The following `jae` still needs the flags, so the trace saves
 * and restores the full integer CPU state before printing.
 */
void wc3_sel_trace_addunit(CPU *c, uint32_t group, uint32_t unit,
                             uint32_t count)
{
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    fprintf(SEL_OUT,
            "[select24] addunit this=%08X unit=%08X count=%u\n",
            group, unit, count);
    sel_restore_state(c, &saved);
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
#define SEL_HANDLE_TABLE 0x6FD68610u
#define SEL_HANDLE_LIVE  0xFFFFFFFEu
#define SEL_HANDLE_MAX   0x00100000u

static int sel_mapped(uint32_t addr, uint32_t size);
static uint32_t sel_rd(uint32_t base, uint32_t offset);

static void sel_describe_handle(uint32_t handle, uint32_t serial, char *out,
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
    table = rd32(GVA(SEL_HANDLE_TABLE));
    if (table == 0u) {
        snprintf(out, outsz, "table=0");
        return;
    }
    if (!sel_mapped(table, 0x40u)) {
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
    if (count > SEL_HANDLE_MAX) {
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
    if (!sel_mapped(entry, 8u)) {
        snprintf(out, outsz, "table=%08X index=%u entry=%08X unmapped", table,
                 index, entry);
        return;
    }
    state = rd32(entry);
    if (state != SEL_HANDLE_LIVE) {
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
    if (!sel_mapped(object, 0x1Cu)) {
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
#define SEL_GUEST_STACK_LO 0x30000000u
#define SEL_GUEST_STACK_HI 0x40000000u
#define SEL_GUEST_HEAP_LO  0x40000000u
#define SEL_GUEST_HEAP_HI  0x50000000u
#define SEL_LOCAL_MAGIC    0x2B61676Cu

static int sel_in_range(uint32_t value, uint32_t lo, uint32_t hi)
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
static _Thread_local uint32_t sel_mapped_page; /* 0 = nothing cached */

static int sel_host_readable(uintptr_t addr)
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

static int sel_mapped(uint32_t addr, uint32_t size)
{
    const uint32_t base = addr & ~0xFFFu;
    const uint32_t span = (addr - base) + size;
    uint32_t pages = (span + 0xFFFu) >> 12;

    if (pages == 0u) pages = 1u;
    if (pages > 2u) pages = 2u; /* callers read at most 0x40 bytes */
    if (pages == 1u && base != 0u && base == sel_mapped_page) return 1;
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t host = (uintptr_t)guest_host_ptr(base + (i << 12));
        if (!sel_host_readable(host)) return 0;
    }
    if (pages == 1u) sel_mapped_page = base;
    return 1;
}
#else
/* Only the Android build runs the game.  The host build exists for a syntax
 * check, where the two windows above stay in charge. */
static int sel_mapped(uint32_t addr, uint32_t size)
{
    (void)addr;
    (void)size;
    return 1;
}
#endif

static int sel_stack_word(uint32_t value)
{
    return sel_in_range(value, SEL_GUEST_STACK_LO,
                          SEL_GUEST_STACK_HI) &&
           (value & 3u) == 0u && sel_mapped(value, 4u);
}

static int sel_heap_word(uint32_t value)
{
    return sel_in_range(value, SEL_GUEST_HEAP_LO,
                          SEL_GUEST_HEAP_HI) &&
           (value & 3u) == 0u && sel_mapped(value, 4u);
}

/* Any guest address this diagnostic may dereference: the guest stack or the
 * identity-mapped heap window.  Everything else is printed as a raw value. */
static int sel_readable(uint32_t value)
{
    return sel_stack_word(value) || sel_heap_word(value);
}

/* Word read that turns an unreadable pointer into 0 instead of a second
 * fault; the trace must never be the reason a run dies. */
static uint32_t sel_rd(uint32_t base, uint32_t offset)
{
    return sel_readable(base) && sel_mapped(base + offset, 4u)
               ? rd32(base + offset)
               : 0u;
}

static void sel_trace_object_line(const char *tag, uint32_t object,
                                    unsigned words)
{
    unsigned i;

    fprintf(SEL_OUT, "[select24] %s object=%08X", tag, object);
    for (i = 0; i < words; ++i)
        fprintf(SEL_OUT, " +%02X=%08X", i * 4u,
                sel_rd(object, i * 4u));
    fputc('\n', SEL_OUT);
}

/* The batch record of 0x6F6B98F0 / 0x6F6B9F70 is nine dwords at stride 0x24;
 * the order loop reads +0x00 (unit) and +0x1C (event handle), and the writer
 * fills +0x04, +0x08 and +0x0C..+0x20 as well.  Print all of them, so a record
 * that was never written is distinguishable from one with a stale handle. */
#define SEL_ORDER_RECORD_STRIDE 0x24u
#define SEL_ORDER_RECORD_MAX    SEL_STRUCT_CAP
#define SEL_ORDER_ARRAY_OFF     0xBA0u
#define SEL_ORDER_FRAME_MIN     0xBE8u

static void sel_trace_order_record(const char *tag, uint32_t index,
                                     uint32_t record)
{
    fprintf(SEL_OUT,
            "[select24] %s idx=%u rec=%08X "
            "e00=%08X e04=%08X e08=%08X e0C=%08X e10=%08X "
            "e14=%08X e18=%08X e1C=%08X e20=%08X\n",
            tag, index, record,
            sel_rd(record, 0x00u), sel_rd(record, 0x04u),
            sel_rd(record, 0x08u), sel_rd(record, 0x0Cu),
            sel_rd(record, 0x10u), sel_rd(record, 0x14u),
            sel_rd(record, 0x18u), sel_rd(record, 0x1Cu),
            sel_rd(record, 0x20u));
}

/*
 * Walk the +0x08 chain of the interface objects that carry {handle, serial}
 * at +0x14/+0x18.  The chain is what links the rejected event to the previous
 * interface that still resolved, so the transition from a live generation to
 * the out-of-range one is visible in one place.  The walk is bounded and
 * cycle-safe; it never follows a pointer outside the guest heap range.
 */
static void sel_trace_self_chain(uint32_t self)
{
    uint32_t seen[8];
    uint32_t node = self;
    unsigned count = 0u;

    while (node != 0u && sel_heap_word(node) && count < 8u) {
        uint32_t ref = sel_rd(node, 4u);
        uint32_t prev = sel_rd(node, 8u);
        uint32_t handle = sel_rd(node, 0x14u);
        uint32_t serial = sel_rd(node, 0x18u);
        uint32_t klass = sel_rd(node, 0x10u);
        unsigned i;
        int duplicate = 0;

        for (i = 0u; i < count; ++i)
            if (seen[i] == node)
                duplicate = 1;
        if (duplicate)
            break;
        seen[count] = node;
        fprintf(SEL_OUT,
                "[select24] selfchain i=%u self=%08X handle=%08X serial=%u "
                "ref=%08X prev=%08X class=%08X link2C=%08X link3C=%08X\n",
                count, node, handle, serial, ref, prev, klass,
                sel_rd(node, 0x2Cu), sel_rd(node, 0x3Cu));
        ++count;
        node = prev;
    }
    if (count == 0u)
        fprintf(SEL_OUT, "[select24] selfchain self=%08X not-in-heap\n",
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
static void sel_scan_handle_table(uint32_t self, uint32_t handle,
                                    uint32_t serial, uint32_t ref)
{
    uint32_t root = rd32(GVA(SEL_HANDLE_TABLE));
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

    if (root == 0u || !sel_mapped(root, 0x40u)) {
        fprintf(SEL_OUT, "[select24] scan table=%08X unmapped\n", root);
        return;
    }
    if ((int32_t)handle < 0) {
        count = rd32(root + 0x3Cu);
        base = rd32(root + 0x2Cu);
    } else {
        count = rd32(root + 0x1Cu);
        base = rd32(root + 0x0Cu);
    }
    if (count > SEL_HANDLE_MAX ||
        (count != 0u &&
         (base < SEL_GUEST_HEAP_LO || base >= SEL_GUEST_HEAP_HI ||
          count > (SEL_GUEST_HEAP_HI - base) / 8u))) {
        fprintf(SEL_OUT,
                "[select24] scan root=%08X base=%08X count=%u suspect\n",
                root, base, count);
        return;
    }
    if (ref >= 0x50u)
        unit = ref - 0x50u;
    fprintf(SEL_OUT,
            "[select24] scan root=%08X base=%08X count=%u handle=%08X "
            "serial=%u self=%08X ref=%08X unit=%08X\n",
            root, base, count, handle, serial, self, ref, unit);
    for (i = 0u; i < count; ++i) {
        uint32_t entry = base + i * 8u;
        uint32_t state = sel_rd(entry, 0u);
        uint32_t object;
        uint32_t object_serial;

        if (state != SEL_HANDLE_LIVE)
            continue;
        object = sel_rd(entry, 4u);
        if (!sel_heap_word(object))
            continue;
        object_serial = sel_rd(object, 0x18u);
        if (self != 0u && object == self) {
            ++self_hits;
            if (self_printed < 4u) {
                fprintf(SEL_OUT,
                        "[select24] scan self-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++self_printed;
            }
        }
        if (ref != 0u && object == ref) {
            ++ref_hits;
            if (ref_printed < 4u) {
                fprintf(SEL_OUT,
                        "[select24] scan ref-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++ref_printed;
            }
        }
        if (unit != 0u && object == unit) {
            ++unit_hits;
            if (unit_printed < 4u) {
                fprintf(SEL_OUT,
                        "[select24] scan unit-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++unit_printed;
            }
        }
        if (serial != 0u && object_serial == serial) {
            ++serial_hits;
            if (serial_printed < 4u) {
                fprintf(SEL_OUT,
                        "[select24] scan serial-hit index=%u object=%08X "
                        "serial=%u\n", i, object, object_serial);
                ++serial_printed;
            }
        }
    }
    fprintf(SEL_OUT,
            "[select24] scan done self-hits=%u ref-hits=%u unit-hits=%u "
            "serial-hits=%u\n",
            self_hits, ref_hits, unit_hits, serial_hits);
}

static void sel_trace_event_context(CPU *c, uint32_t self, uint32_t arg,
                                      int ok)
{
    uint32_t ebp = c->ebp;
    unsigned frame;
    int local_seen = 0;

    for (frame = 0; frame < 32u; ++frame) {
        uint32_t next;
        uint32_t ra;

        if (!sel_stack_word(ebp))
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
        if (ebp >= SEL_GUEST_STACK_LO + 0x40u &&
            rd32(ebp - 0x3Cu) == SEL_LOCAL_MAGIC) {
            uint32_t unit = rd32(ebp - 0x30u);
            uint32_t event = rd32(ebp - 0x2Cu);
            uint32_t vtbl = sel_heap_word(unit) ? rd32(unit) : 0u;
            uint32_t u14 = sel_heap_word(unit) ? rd32(unit + 0x14u) : 0u;
            uint32_t u18 = sel_heap_word(unit) ? rd32(unit + 0x18u) : 0u;

            local_seen = 1;
            fprintf(SEL_OUT,
                    "[select24] local f%u ebp=%08X unit=%08X event=%08X "
                    "vtbl=%08X unit14=%08X unit18=%08X self=%08X "
                    "arg0=%08X arg1=%08X arg2=%08X\n",
                    frame, ebp, unit, event, vtbl, u14, u18, self,
                    rd32(ebp + 8u), rd32(ebp + 0xCu), rd32(ebp + 0x10u));
            if (!ok && sel_heap_word(unit)) {
                sel_trace_object_line("unit", unit, 17u);
                fprintf(SEL_OUT,
                        "[select24] unit2 unit=%08X u44=%08X u48=%08X "
                        "u4C=%08X u50=%08X u54=%08X\n",
                        unit, sel_rd(unit, 0x44u), sel_rd(unit, 0x48u),
                        sel_rd(unit, 0x4Cu), sel_rd(unit, 0x50u),
                        sel_rd(unit, 0x54u));
            }
        }

        /*
         * Builder 0x6F6B98F0 keeps the same object at [ebp-0x20C] and
         * [ebp-0x204], its record count at [ebp-0x1C4] and the current index
         * at [ebp-0x208].  The object repeats the count at +0x40, which makes
         * the match specific; the record array was moved to [ebp-0xBA0].
         * [ebp-0x200] in this frame is a pointer to a local handed to the
         * event lookup, so reading it as the index would make the check
         * unsatisfiable.
         */
        if (ebp >= SEL_GUEST_STACK_LO + SEL_ORDER_FRAME_MIN) {
            uint32_t object = rd32(ebp - 0x204u);
            uint32_t alias = rd32(ebp - 0x20Cu);
            uint32_t count = rd32(ebp - 0x1C4u);
            uint32_t index = rd32(ebp - 0x208u);
            int strict = object == alias && count <= 40u && index <= count &&
                         sel_heap_word(object) &&
                         sel_rd(object, 0x40u) == count;

            if (strict) {
                uint32_t array = ebp - SEL_ORDER_ARRAY_OFF;
                uint32_t element = array + index * 0x24u;
                uint32_t i;

                fprintf(SEL_OUT,
                        "[select24] builderB f%u ebp=%08X count=%u index=%u "
                        "object=%08X object14=%08X object18=%08X "
                        "object1C=%08X array=%08X element=%08X\n",
                        frame, ebp, count, index, object,
                        sel_rd(object, 0x14u), sel_rd(object, 0x18u),
                        sel_rd(object, 0x1Cu), array, element);
                if (sel_stack_word(array) &&
                    sel_stack_word(element) &&
                    element + 0x24u <= SEL_GUEST_STACK_HI) {
                    fprintf(SEL_OUT,
                            "[select24] element e00=%08X e04=%08X e08=%08X "
                            "e0C=%08X e10=%08X e14=%08X e18=%08X "
                            "e1C=%08X e20=%08X\n",
                            sel_rd(element, 0x00u),
                            sel_rd(element, 0x04u),
                            sel_rd(element, 0x08u),
                            sel_rd(element, 0x0Cu),
                            sel_rd(element, 0x10u),
                            sel_rd(element, 0x14u),
                            sel_rd(element, 0x18u),
                            sel_rd(element, 0x1Cu),
                            sel_rd(element, 0x20u));
                }
                /*
                 * Dump the whole batch as it stands when the failure is
                 * reported.  The loop trace prints the same array on its first
                 * iteration; this copy covers the case where the guard fires
                 * on a frame the loop trace never reached (a chunk that was
                 * compiled without it, or a batch whose loop already exited).
                 */
                if (sel_stack_word(array)) {
                    uint32_t total = count;

                    if (total > SEL_ORDER_RECORD_MAX)
                        total = SEL_ORDER_RECORD_MAX;
                    fprintf(SEL_OUT,
                            "[select24] builderB array base=%08X count=%u\n",
                            array, count);
                    for (i = 0; i < total; ++i) {
                        uint32_t rec = array + i * SEL_ORDER_RECORD_STRIDE;

                        if (!sel_stack_word(rec) ||
                            rec + SEL_ORDER_RECORD_STRIDE >
                                SEL_GUEST_STACK_HI)
                            break;
                        sel_trace_order_record("builderB", i, rec);
                    }
                }
            } else if (!ok &&
                       (sel_heap_word(object) || sel_heap_word(alias) ||
                        count != 0u || index != 0u)) {
                /*
                 * The strict identity check misses the failing frame when the
                 * producer stores a different alias or the count copy is
                 * already damaged.  Print the raw candidates and the record at
                 * the current index anyway; this is what distinguishes "the
                 * builder frame was not walked" from "the record itself is
                 * wrong".
                 */
                uint32_t object40 = sel_rd(object, 0x40u);

                fprintf(SEL_OUT,
                        "[select24] builderB? f%u ebp=%08X object=%08X "
                        "alias=%08X count=%u index=%u object40=%08X "
                        "strict=%d\n",
                        frame, ebp, object, alias, count, index, object40,
                        strict);
                if (index <= 64u) {
                    uint32_t array = ebp - SEL_ORDER_ARRAY_OFF;
                    uint32_t element = array + index * 0x24u;

                    if (sel_stack_word(array) &&
                        element + 0x24u <= SEL_GUEST_STACK_HI) {
                        fprintf(SEL_OUT,
                                "[select24] builderB? element "
                                "e00=%08X e04=%08X e08=%08X e0C=%08X "
                                "e10=%08X e14=%08X e18=%08X e1C=%08X "
                                "e20=%08X\n",
                                sel_rd(element, 0x00u),
                                sel_rd(element, 0x04u),
                                sel_rd(element, 0x08u),
                                sel_rd(element, 0x0Cu),
                                sel_rd(element, 0x10u),
                                sel_rd(element, 0x14u),
                                sel_rd(element, 0x18u),
                                sel_rd(element, 0x1Cu),
                                sel_rd(element, 0x20u));
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
        if (ra == 0x6F146C3Fu && sel_stack_word(next)) {
            uint32_t source = rd32(next + 8u);

            int source_readable = sel_heap_word(source) ||
                                  sel_stack_word(source);

            fprintf(SEL_OUT,
                    "[select24] source L_6F146C10 arg=%08X%s",
                    source, source_readable ? " " : "\n");
            if (source_readable) {
                fprintf(SEL_OUT,
                        "object=%08X +00=%08X +0C=%08X +10=%08X "
                        "+14=%08X +18=%08X\n",
                        source, sel_rd(source, 0x00u),
                        sel_rd(source, 0x0Cu), sel_rd(source, 0x10u),
                        sel_rd(source, 0x14u), sel_rd(source, 0x18u));
            }
        }

        if (!ok) {
            fprintf(SEL_OUT, "[select24] stack f%u ebp=%08X ra=%08X\n",
                    frame, ebp, ra);
        }

        if (next <= ebp || next - ebp > 0x10000u ||
            !sel_stack_word(next))
            break;
        ebp = next;
    }

    if (!ok && !local_seen) {
        fprintf(SEL_OUT,
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
void wc3_sel_trace_evt2(CPU *c, uint32_t self, uint32_t arg)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    char probe[160];
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    uint32_t ref = 0u;
    uint32_t prev = 0u;
    uint32_t klass = 0u;
    uint32_t ra = 0u;
    int ok;

    sel_save_state(c, &saved);
    if (self != 0u && sel_heap_word(self)) {
        handle = sel_rd(self, 0x14u);
        serial = sel_rd(self, 0x18u);
        ref = sel_rd(self, 4u);
        prev = sel_rd(self, 8u);
        klass = sel_rd(self, 0x10u);
    }
    if (sel_stack_word(c->ebp))
        ra = rd32(c->ebp + 4u);
    sel_describe_handle(handle, serial, probe, sizeof(probe), &ok);
    fprintf(SEL_OUT,
            "[select24] evt2 self=%08X arg=%08X handle=%08X serial=%u "
            "ref=%08X prev=%08X class=%08X ra=%08X %s\n",
            self, arg, handle, serial, ref, prev, klass, ra, probe);
    if (!ok && self != 0u && sel_heap_word(self)) {
        sel_trace_object_line("self", self, 17u);
        if (sel_heap_word(ref))
            sel_trace_object_line("selfref", ref, 17u);
        if (sel_heap_word(arg))
            sel_trace_object_line("arg", arg, 9u);
        sel_trace_self_chain(self);
        sel_scan_handle_table(self, handle, serial, ref);
        /*
         * L_6F146D30 is called from L_6F146C10 with the source interface at
         * [caller_ebp+8].  Scan for that object too: a source hit means the
         * rejected pair came from a registered interface; a miss means the
         * producer was given a temporary or stale wrapper.  The helper has no
         * separate "probe" argument, so pass source+0x50 as ref; it derives
         * unit=ref-0x50 and searches the table for source.
         */
        if (sel_stack_word(c->ebp)) {
            uint32_t caller = rd32(c->ebp);
            uint32_t source = sel_stack_word(caller) ? rd32(caller + 8u) : 0u;

            if (source != 0u && sel_heap_word(source)) {
                uint32_t source_handle = sel_rd(source, 0x14u);

                fprintf(SEL_OUT,
                        "[select24] scan source object=%08X handle=%08X "
                        "serial=%u\n", source, source_handle,
                        sel_rd(source, 0x18u));
                sel_scan_handle_table(0u, source_handle, 0u,
                                        source + 0x50u);
            }
        }
    }
    sel_trace_event_context(c, self, arg, ok);
    sel_restore_state(c, &saved);
}

/*
 * 0x6F6926B0 reads the task payload at [factory+0x54] and silently drops the
 * order for that unit when the field is zero; the two consumers at 0x6F689FC0
 * and 0x6F691E60 then only see the resulting null.  This trace records every
 * task creation, null payload or not: the wrapper, the payload (with its
 * vtable, class tag and lifetime fields), the descriptor at [ebp-0x3C] that
 * 0x6F055D00 built (class tag at +4, resolved class factory from 0x6F055C40 at
 * +0xC, caller argument at +0x10) and the order target from [ebp+8]/[ebp+0xC].
 * `null=` tells the dropped unit apart without a second run.
 */
void wc3_sel_trace_tasknull(CPU *c, uint32_t wrapper)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    uint32_t desc;
    uint32_t payload;

    sel_save_state(c, &saved);
    desc = c->ebp - 0x3Cu;
    payload = sel_rd(wrapper, 0x54u);
    fprintf(SEL_OUT,
            "[select24] task seq=%u cap=%u wrapper=%08X payload=%08X "
            "null=%u vt=%08X class=%08X handle=%08X serial=%u "
            "f50=%08X f54=%08X p24=%08X p28=%08X p2C=%08X p30=%08X "
            "p34=%08X p38=%08X p3C=%08X "
            "unit=%08X arg=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X dpair=%08X/%08X\n",
            g_sel_order_seq, wc3_sel_cap(), wrapper, payload,
            payload == 0u ? 1u : 0u,
            sel_rd(payload, 0x00u), sel_rd(payload, 0x10u),
            sel_rd(payload, 0x14u), sel_rd(payload, 0x18u),
            sel_rd(payload, 0x50u), sel_rd(payload, 0x54u),
            sel_rd(payload, 0x24u), sel_rd(payload, 0x28u),
            sel_rd(payload, 0x2Cu), sel_rd(payload, 0x30u),
            sel_rd(payload, 0x34u), sel_rd(payload, 0x38u),
            sel_rd(payload, 0x3Cu),
            sel_rd(c->ebp, 0x08u), sel_rd(c->ebp, 0x0Cu),
            desc, sel_rd(desc, 0x04u), sel_rd(desc, 0x0Cu),
            sel_rd(desc, 0x10u), sel_rd(desc, 0x14u),
            sel_rd(desc, 0x18u));
    sel_restore_state(c, &saved);
}

/*
 * Inserted at 0x6F154B92, right after both object initialisation slots of
 * 0x6F154B60 ran (the +0x0C ctor and the +0x44 bind).  The birth record of
 * 0x6F154B76 shows the object before the slots ran; this one shows whether a
 * slot (or the virtual factory callback [manager+0x254]) filled the +0x54
 * payload that 0x6F6926B0 later reads.  `object` (edi) and `desc` (ebx) are
 * still live in the caller frame at that point.
 */
void wc3_sel_trace_evinit(CPU *c, uint32_t object, uint32_t desc)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    fprintf(SEL_OUT,
            "[select24] evinit object=%08X vt=%08X class=%08X handle=%08X "
            "serial=%u f50=%08X f54=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X dpair=%08X/%08X d24=%08X d28=%08X\n",
            object, sel_rd(object, 0x00u), sel_rd(object, 0x10u),
            sel_rd(object, 0x14u), sel_rd(object, 0x18u),
            sel_rd(object, 0x50u), sel_rd(object, 0x54u), desc,
            sel_rd(desc, 0x04u), sel_rd(desc, 0x0Cu),
            sel_rd(desc, 0x10u), sel_rd(desc, 0x14u),
            sel_rd(desc, 0x18u), sel_rd(desc, 0x24u),
            sel_rd(desc, 0x28u));
    sel_restore_state(c, &saved);
}

/*
 * Inserted at 0x6F69273F, the second silent drop of 0x6F6926B0: the result of
 * 0x6F04CDD0 (the "instance of class" check) is zero and the function jumps to
 * its shared epilogue without touching the unit.  The caller's frame still
 * holds the target unit at [ebp+8], the order argument at [ebp+0xC], the class
 * tag at [ebp-0x48] and the wrapper object in edi, so the record pairs the
 * rejected unit with the object it was rejected for.
 */
void wc3_sel_trace_orderdrop(CPU *c, uint32_t wrapper, uint32_t tag,
                               uint32_t result)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    fprintf(SEL_OUT,
            "[select24] orderdrop seq=%u cap=%u wrapper=%08X result=%08X "
            "tag=%08X unit=%08X arg=%08X vt=%08X class=%08X handle=%08X "
            "serial=%u f50=%08X f54=%08X\n",
            g_sel_order_seq, wc3_sel_cap(), wrapper, result, tag,
            sel_rd(c->ebp, 0x08u), sel_rd(c->ebp, 0x0Cu),
            sel_rd(wrapper, 0x00u), sel_rd(wrapper, 0x10u),
            sel_rd(wrapper, 0x14u), sel_rd(wrapper, 0x18u),
            sel_rd(wrapper, 0x50u), sel_rd(wrapper, 0x54u));
    sel_restore_state(c, &saved);
}

/*
 * Inserted around the 0x6F054530 call in 0x6F04D9C0: stage 0 carries the
 * {handle, serial} pair, stage 1 the lookup result.  Only the pairs that the
 * replica already rejects and the zero results are printed, so a normal run
 * stays quiet and a failing run shows both halves of the same lookup.
 */
void wc3_sel_trace_lookup(CPU *c, uint32_t stage, uint32_t a, uint32_t b)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    char probe[160];
    int ok;

    sel_save_state(c, &saved);
    if (stage == 0u) {
        sel_describe_handle(a, b, probe, sizeof(probe), &ok);
        if (!ok) {
            fprintf(SEL_OUT, "[select24] lookup handle=%08X serial=%u %s\n",
                    a, b, probe);
        }
    } else if (a == 0u) {
        fprintf(SEL_OUT, "[select24] lookup result=0\n");
    }
    sel_restore_state(c, &saved);
}

/* Index and record-cursor slots of the two order loops.  The array base is
 * [ebp-0xBA0] in both.  The cursor points past the record start by 0x18 in one
 * loop and by 0x1C in the other, which is exactly the offset the loop uses for
 * its unit read; the index slot follows the loop shape, not a builder name:
 *   cursor 0x18 -> index [ebp-0x208]  (site 0x6F6B9E8D)
 *   cursor 0x1C -> index [ebp-0x200]  (site 0x6F6BA4C2)
 * In the 0x18 frame [ebp-0x200] holds a pointer to a local passed to the event
 * lookup, and in the 0x1C frame [ebp-0x208] holds the saved edi, so the two
 * slots must not be mixed.  The batch count is [ebp-0x1C4] in both.
 */
#define SEL_ORDER_COUNT      0xFFFFFE3Cu /* [ebp-0x1C4] */
#define SEL_ORDER_CURSOR_18  0x18u
#define SEL_ORDER_CURSOR_1C  0x1Cu
#define SEL_ORDER_INDEX_18   0xFFFFFDF8u /* [ebp-0x208], cursor 0x18 */
#define SEL_ORDER_INDEX_1C   0xFFFFFE00u /* [ebp-0x200], cursor 0x1C */

/*
 * Inserted after `mov ebx, ecx` in the two point-order handlers
 * (0x6F6B991E for message 0x12 and 0x6F6B9F9E for message 0x13).  ecx/ebx is
 * the request object.  The request layout is:
 *   +0x15 byte  target/order kind
 *   +0x18 word  flags
 *   +0x1C dword command
 *   +0x20/+0x24 second object handle/serial
 *   +0x28/+0x2C ground point
 *   +0x30/+0x34 first object handle/serial
 * This line is emitted once per issued point order and is the header every
 * later formation record refers to by seq.
 */
void wc3_sel_trace_order_begin(CPU *c, uint32_t request, uint32_t source)
{
    Sel24CpuState saved;
    uint32_t seq = ++g_sel_order_seq;
    uint32_t cap = wc3_sel_cap();

    sel_save_state(c, &saved);
    fprintf(SEL_OUT,
            "[select24] orderbegin seq=%u src=%u req=%08X cap=%u "
            "o00=%08X o04=%08X o08=%08X o0C=%08X o10=%08X o14=%08X "
            "t15=%02X flags=%04X cmd=%08X "
            "o20=%08X o24=%08X pt=%08X,%08X o30=%08X o34=%08X "
            "o38=%08X o3C=%08X\n",
            seq, source, request, cap,
            sel_rd(request, 0x00u), sel_rd(request, 0x04u),
            sel_rd(request, 0x08u), sel_rd(request, 0x0Cu),
            sel_rd(request, 0x10u), sel_rd(request, 0x14u),
            (uint32_t)(sel_rd(request, 0x14u) >> 8) & 0xFFu,
            sel_rd(request, 0x18u) & 0xFFFFu,
            sel_rd(request, 0x1Cu),
            sel_rd(request, 0x20u), sel_rd(request, 0x24u),
            sel_rd(request, 0x28u), sel_rd(request, 0x2Cu),
            sel_rd(request, 0x30u), sel_rd(request, 0x34u),
            sel_rd(request, 0x38u), sel_rd(request, 0x3Cu));
    sel_restore_state(c, &saved);
}

/*
 * Inserted on the 0x6F16966D `cmovne eax, esi` line, immediately before the
 * PrClGen pointer write at 0x6F169670.  The insertion routine 0x6F169620 uses
 * a slot index (`edx`) that is -1 when both the existing-slot lookup and the
 * free-slot lookup failed; the original code then writes the pointer array
 * out of bounds.  `list` is the PrClGen object (edi), `index` is edx, `value`
 * is the pointer about to be stored in the widened array at +0x1FC, `unit`
 * is the function argument and `store` is the "store pointer" flag.
 * `slot`/`serial` show the slot's {handle, serial} pair and `used` counts
 * occupied slots across the runtime cap, so a path that still stops at twelve
 * is visible even when the batch has 24 records.
 */
void wc3_sel_trace_listadd(CPU *c, uint32_t list, uint32_t index,
                             uint32_t value, uint32_t unit, uint32_t store)
{
    Sel24CpuState saved;
    uint32_t cap = wc3_sel_cap();
    int32_t idx = (int32_t)index;
    uint32_t ptr = 0u;
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    unsigned used = 0u;
    unsigned i;

    sel_save_state(c, &saved);
    if (idx >= 0 && (uint32_t)idx < cap && sel_readable(list)) {
        ptr = sel_rd(list, SEL_CLUSTER_PTRS + (uint32_t)idx * 4u);
        handle = sel_rd(list, 0x1Cu + (uint32_t)idx * 0xCu);
        serial = sel_rd(list, 0x20u + (uint32_t)idx * 0xCu);
    }
    if (sel_readable(list)) {
        for (i = 0u; i < cap; ++i)
            if (sel_rd(list, 0x1Cu + i * 0xCu) != 0xFFFFFFFFu)
                ++used;
    }
    fprintf(SEL_OUT,
            "[select24] listadd seq=%u list=%08X cap=%u idx=%d unit=%08X "
            "value=%08X store=%u used=%u ptr=%08X slot=%08X/%08X "
            "tgt=%08X/%08X pt=%08X,%08X,%08X flags=%08X\n",
            g_sel_order_seq, list, cap, idx, unit, value, store, used, ptr,
            handle, serial,
            sel_rd(list, SEL_CLUSTER_TAIL),
            sel_rd(list, SEL_CLUSTER_TAIL + 4u),
            sel_rd(list, SEL_CLUSTER_TAIL + 8u),
            sel_rd(list, SEL_CLUSTER_TAIL + 12u),
            sel_rd(list, SEL_CLUSTER_TAIL + 16u),
            sel_rd(list, SEL_CLUSTER_FLAGS));
    sel_restore_state(c, &saved);
}

/*
 * Inserted after 0x6F16B306 (`mov edi, ecx`) at the entry of the ground-order
 * solver 0x6F16B2F0.  ecx/edi is the CPrCluster.  The member vector uses the
 * widened layout:
 *   +0x28 base, +0x30 grow flag, +0x34 capacity, +0x38 count
 * and each member record is 0x2C bytes with the unit pointer at +0x14.  The
 * trace dumps every member up to the runtime cap before the solver touches
 * the index/pair workspace.  A cluster that already stops at twelve is
 * visible here; the same header prints its target point and flags.
 *
 * The solver runs for every active cluster on every frame.  The full dump is
 * kept for groups at or above the ten-member diagnostic floor and only once
 * per (order, cluster, count); repeated identical frames are what turned the
 * first debug run into a 164 MB trace.  A count change (12 -> 13 -> 14) is a
 * new record and is still printed.
 */
static int sel_formation_seen(uint32_t cluster, uint32_t count)
{
    static uint32_t keys[128];
    static unsigned used;
    uint32_t key = cluster ^ (count * 0x9E3779B1u) ^
                   (g_sel_order_seq * 0x85EBCA6Bu);
    unsigned i;

    for (i = 0u; i < used; ++i)
        if (keys[i] == key)
            return 1;
    if (used < 128u)
        keys[used++] = key;
    return 0;
}

void wc3_sel_trace_formation_begin(CPU *c, uint32_t cluster)
{
    Sel24CpuState saved;
    uint32_t cap = wc3_sel_cap();
    uint32_t vcap = 0u;
    uint32_t count = 0u;
    uint32_t base = 0u;
    uint32_t i;

    sel_save_state(c, &saved);
    if (sel_readable(cluster)) {
        vcap = sel_rd(cluster, 0x34u);
        count = sel_rd(cluster, 0x38u);
        base = sel_rd(cluster, 0x28u);
    }
    if (count < 10u || sel_formation_seen(cluster, count)) {
        sel_restore_state(c, &saved);
        return;
    }
    fprintf(SEL_OUT,
            "[select24] formation seq=%u cluster=%08X cap=%u vcap=%u count=%u "
            "grow=%08X base=%08X tgt=%08X/%08X pt=%08X,%08X,%08X "
            "flags=%08X\n",
            g_sel_order_seq, cluster, cap, vcap, count,
            sel_rd(cluster, 0x30u), base,
            sel_rd(cluster, SEL_CLUSTER_TAIL),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 4u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 8u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 12u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 16u),
            sel_rd(cluster, SEL_CLUSTER_FLAGS));
    if (count > cap)
        fprintf(SEL_OUT,
                "[select24] formation seq=%u OVER count=%u cap=%u vcap=%u\n",
                g_sel_order_seq, count, cap, vcap);
    for (i = 0u; i < count && i < cap; ++i) {
        uint32_t rec = base + i * 0x2Cu;
        uint32_t unit;

        if (!sel_readable(base) || !sel_readable(rec))
            break;
        unit = sel_rd(rec, 0x14u);
        fprintf(SEL_OUT,
                "[select24] formationrec seq=%u i=%u rec=%08X unit=%08X "
                "h=%08X s=%08X r28=%08X r2C=%08X\n",
                g_sel_order_seq, i, rec, unit,
                sel_rd(unit, 0x14u), sel_rd(unit, 0x18u),
                sel_rd(rec, 0x28u), sel_rd(rec, 0x2Cu));
    }
    sel_restore_state(c, &saved);
}

/*
 * Inserted at the entry of the member-vector insertion 0x6F16C077.  `vector`
 * is esi (the embedded vector at cluster+0x1C), `add` is edi (number of
 * members the caller wants to append, normally one).  The vector fields are
 *   +0x0C base, +0x14 grow flag, +0x18 capacity, +0x1C count.
 * `over` is the exact branch condition of 0x6F16C083: count+add > capacity.
 * When the grow flag is zero and over is one, the insertion returns zero at
 * 0x6F16C0AA; the caller 0x6F16B7B0 ignores that result and unlinks the unit
 * from PrClGen anyway.  Printing the live fields here is what separates a
 * still-12 cluster from a failure later in the solver.
 */
void wc3_sel_trace_cluster_try(CPU *c, uint32_t vector, uint32_t add)
{
    Sel24CpuState saved;
    uint32_t cap = wc3_sel_cap();
    uint32_t vcap = 0u;
    uint32_t count = 0u;
    uint32_t grow = 0u;
    uint32_t base = 0u;
    uint32_t want = 0u;
    uint32_t cluster = vector >= 0x1Cu ? vector - 0x1Cu : 0u;

    sel_save_state(c, &saved);
    if (sel_readable(vector)) {
        vcap = sel_rd(vector, 0x18u);
        count = sel_rd(vector, 0x1Cu);
        grow = sel_rd(vector, 0x14u);
        base = sel_rd(vector, 0x0Cu);
    }
    want = count + add;
    fprintf(SEL_OUT,
            "[select24] clustertry seq=%u vector=%08X cluster=%08X cv=%08X "
            "cap=%u vcap=%u count=%u add=%u want=%u over=%u grow=%08X "
            "base=%08X tgt=%08X/%08X pt=%08X,%08X,%08X flags=%08X\n",
            g_sel_order_seq, vector, cluster, sel_rd(cluster, 0u), cap,
            vcap, count, add, want, want > vcap ? 1u : 0u, grow, base,
            sel_rd(cluster, SEL_CLUSTER_TAIL),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 4u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 8u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 12u),
            sel_rd(cluster, SEL_CLUSTER_TAIL + 16u),
            sel_rd(cluster, SEL_CLUSTER_FLAGS));
    sel_restore_state(c, &saved);
}

/*
 * Inserted after the 0x6F16C060 member-vector insertion at 0x6F16B7ED.  This
 * is the call whose zero result is ignored by the original code: the unit is
 * then unlinked from PrClGen anyway.  `result` is eax immediately after the
 * call, `unit` is edi (the PrClGen slot pointer), `list` is ebx, `slot` is
 * esi and `cluster` is the CPrCluster argument.  The cluster count/capacity
 * after the insertion tells whether the 13th..24th members were accepted.
 */
void wc3_sel_trace_cluster_insert(CPU *c, uint32_t cluster, uint32_t unit,
                                    uint32_t list, uint32_t slot,
                                    uint32_t result)
{
    Sel24CpuState saved;
    uint32_t cap = wc3_sel_cap();
    int32_t idx = (int32_t)slot;
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    uint32_t count = 0u;
    uint32_t base = 0u;
    uint32_t last = 0u;

    sel_save_state(c, &saved);
    if (sel_readable(cluster)) {
        count = sel_rd(cluster, 0x38u);
        base = sel_rd(cluster, 0x28u);
    }
    if (idx >= 0 && (uint32_t)idx < cap && sel_readable(list)) {
        handle = sel_rd(list, 0x1Cu + (uint32_t)idx * 0xCu);
        serial = sel_rd(list, 0x20u + (uint32_t)idx * 0xCu);
    }
    if (count > 0u && count <= cap)
        last = sel_rd(base + (count - 1u) * 0x2Cu, 0x14u);
    fprintf(SEL_OUT,
            "[select24] clusterins seq=%u cluster=%08X cap=%u unit=%08X "
            "list=%08X slot=%d result=%08X vcap=%u count=%u "
            "slotpair=%08X/%08X base=%08X last=%08X\n",
            g_sel_order_seq, cluster, cap, unit, list, idx, result,
            sel_rd(cluster, 0x34u), count, handle, serial, base, last);
    sel_restore_state(c, &saved);
}

/*
 * Ground-order base trace (diagnostic).  The order loop reads
 * [selection+0x28] as the unit-record base and indexes it by
 * group_index * 0x2C.  The member vector at selection+0x1C has the same base
 * at +0x0C, so printing both separates a stale 12-entry record array from a
 * member vector that grew to 24.  idx is the absolute group index
 * (cursor + loop counter); the group index array is at group+4.
 */
void wc3_sel_trace_order_base(CPU *c, uint32_t sel, uint32_t group,
                                uint32_t cursor, uint32_t idx)
{
    Sel24CpuState saved;
    uint32_t base = 0u;
    uint32_t vec = 0u;
    uint32_t vcap = 0u;
    uint32_t vcnt = 0u;
    uint32_t vgrow = 0u;
    uint32_t gidx = 0u;

    sel_save_state(c, &saved);
    /* The CPrCluster member vector is embedded at sel+0x1C: its vtable is at
     * sel+0x1C, the data base at sel+0x28 (= vector+0x0C), the growth chunk at
     * sel+0x30, capacity at sel+0x34 and count at sel+0x38. */
    vec = sel + 0x1Cu;
    if (sel_readable(sel)) {
        base = sel_rd(sel, 0x28u);
        vcap = sel_rd(vec, 0x18u);
        vcnt = sel_rd(vec, 0x1Cu);
        vgrow = sel_rd(vec, 0x14u);
    }
    if (sel_readable(group) && idx < 64u)
        gidx = sel_rd(group, 4u + idx * 4u);
    fprintf(SEL_OUT,
            "[select24] orderbase seq=%u sel=%08X base=%08X vec=%08X "
            "vcap=%u vcnt=%u vgrow=%08X group=%08X cur=%u idx=%u gidx=%08X\n",
            g_sel_order_seq, sel, base, vec, vcap, vcnt, vgrow,
            group, cursor, idx, gidx);
    sel_restore_state(c, &saved);
}

/*
 * Inserted at the entry of the two batch writers 0x6F6BA590 and 0x6F6BA800
 * after `mov esi, [edi]`.  `object` is the object the writer fills, `self` the
 * handler object it was called with, `pair_off` the offset of the handle/serial
 * pair the writer later pushes: 0x38 for the 0x6F6BA590 site, 0x30 for the
 * 0x6F6BA800 site.  Both pairs are printed, so a writer that reads a stale
 * pair is visible even if the offset assumption is wrong.  The first line also
 * prints the full request fields, including the target pair at +0x30/+0x34
 * and the ground point at +0x28/+0x2C, so a unit-target order is
 * distinguishable from a point order before any per-unit record is emitted.
 */
void wc3_sel_trace_order_unit(CPU *c, uint32_t pair_off, uint32_t object,
                                uint32_t self)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    uint32_t first = 0u;
    uint32_t cap = wc3_sel_cap();

    sel_save_state(c, &saved);
    if (sel_readable(object))
        first = rd32(object);
    fprintf(SEL_OUT,
            "[select24] orderunit seq=%u cap=%u pair=%02X self=%08X "
            "object=%08X first=%08X "
            "o00=%08X o04=%08X o08=%08X o0C=%08X o10=%08X o14=%08X "
            "t15=%02X flags=%04X cmd=%08X "
            "o20=%08X o24=%08X pt=%08X,%08X o30=%08X o34=%08X "
            "o38=%08X o3C=%08X\n",
            g_sel_order_seq, cap, pair_off, self, object, first,
            sel_rd(object, 0x00u), sel_rd(object, 0x04u),
            sel_rd(object, 0x08u), sel_rd(object, 0x0Cu),
            sel_rd(object, 0x10u), sel_rd(object, 0x14u),
            (uint32_t)(sel_rd(object, 0x14u) >> 8) & 0xFFu,
            sel_rd(object, 0x18u) & 0xFFFFu,
            sel_rd(object, 0x1Cu),
            sel_rd(object, 0x20u), sel_rd(object, 0x24u),
            sel_rd(object, 0x28u), sel_rd(object, 0x2Cu),
            sel_rd(object, 0x30u), sel_rd(object, 0x34u),
            sel_rd(object, 0x38u), sel_rd(object, 0x3Cu));
    if (sel_readable(first))
        fprintf(SEL_OUT,
                "[select24] orderunit seq=%u pair=%02X first14=%08X "
                "first18=%08X first1C=%08X\n",
                g_sel_order_seq, pair_off, rd32(first + 0x14u),
                rd32(first + 0x18u), rd32(first + 0x1Cu));
    sel_restore_state(c, &saved);
}

/*
 * Inserted after `mov edx, eax` in the two order loops (0x6F6B9E8D and
 * 0x6F6BA4C2).  eax is the event object the per-unit lookup returned and ecx
 * the target unit; a zero event is the unit that silently drops out of the
 * order.  `cursor_off` is 0x18 or 0x1C and selects the matching index slot.
 * The first iteration also dumps the sorted array, so the log shows whether
 * the writer produced twenty-four records at all or only twelve.
 */
void wc3_sel_trace_order_loop(CPU *c, uint32_t cursor_off, uint32_t cursor,
                                uint32_t unit, uint32_t event)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    uint32_t record = cursor - cursor_off;
    uint32_t index_slot = (cursor_off == SEL_ORDER_CURSOR_18)
                              ? SEL_ORDER_INDEX_18
                              : SEL_ORDER_INDEX_1C;
    int frame_ok = sel_stack_word(c->ebp);
    uint32_t index = frame_ok ? rd32(c->ebp + index_slot) : 0u;
    uint32_t count = frame_ok ? rd32(c->ebp + SEL_ORDER_COUNT) : 0u;
    uint32_t object = c->edi;
    uint32_t o15 = 0u;
    uint32_t o18 = 0u;
    uint32_t i;

    sel_save_state(c, &saved);
    if (sel_readable(object)) {
        o15 = (uint32_t)rd8(object + 0x15u);
        o18 = (uint32_t)rd16(object + 0x18u);
    }
    fprintf(SEL_OUT,
            "[select24] orderloop seq=%u cap=%u cu=%02X idx=%u count=%u "
            "rec=%08X unit=%08X event=%08X object=%08X o14=%08X o15=%02X "
            "o18=%04X o1C=%08X\n",
            g_sel_order_seq, wc3_sel_cap(), cursor_off, index, count,
            record, unit, event, object, sel_rd(object, 0x14u), o15, o18,
            sel_rd(object, 0x1Cu));
    fprintf(SEL_OUT,
            "[select24] orderloopev seq=%u event=%08X "
            "e00=%08X e04=%08X e08=%08X e0C=%08X e10=%08X e14=%08X "
            "e18=%08X e1C=%08X e20=%08X e24=%08X e28=%08X e2C=%08X "
            "e30=%08X e34=%08X e38=%08X e3C=%08X e54=%08X\n",
            g_sel_order_seq, event,
            sel_rd(event, 0x00u), sel_rd(event, 0x04u),
            sel_rd(event, 0x08u), sel_rd(event, 0x0Cu),
            sel_rd(event, 0x10u), sel_rd(event, 0x14u),
            sel_rd(event, 0x18u), sel_rd(event, 0x1Cu),
            sel_rd(event, 0x20u), sel_rd(event, 0x24u),
            sel_rd(event, 0x28u), sel_rd(event, 0x2Cu),
            sel_rd(event, 0x30u), sel_rd(event, 0x34u),
            sel_rd(event, 0x38u), sel_rd(event, 0x3Cu),
            sel_rd(event, 0x54u));
    if (sel_stack_word(record) &&
        record + SEL_ORDER_RECORD_STRIDE <= SEL_GUEST_STACK_HI)
        sel_trace_order_record("orderrec", index, record);

    if (index == 0u && sel_stack_word(record)) {
        uint32_t total = count;

        if (total > SEL_ORDER_RECORD_MAX)
            total = SEL_ORDER_RECORD_MAX;
        fprintf(SEL_OUT,
                "[select24] orderdump cu=%02X count=%u base=%08X max=%u\n",
                cursor_off, count, record, SEL_ORDER_RECORD_MAX);
        for (i = 0; i < total; ++i) {
            uint32_t rec = record + i * SEL_ORDER_RECORD_STRIDE;

            if (!sel_stack_word(rec) ||
                rec + SEL_ORDER_RECORD_STRIDE > SEL_GUEST_STACK_HI)
                break;
            sel_trace_order_record("orderdump", i, rec);
        }
    }
    sel_restore_state(c, &saved);
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
void wc3_sel_trace_order_slots(CPU *c, uint32_t tag, uint32_t list,
                                 uint32_t element)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    uint32_t base = (tag == 3u) ? 0x2Cu : 0x50u;
    uint32_t index = 0u;
    uint32_t command;
    uint32_t unit;

    sel_save_state(c, &saved);
    if (list != 0u && element >= list + base)
        index = (element - list - base) >> 5;
    command = sel_rd(list, 0x00u);
    unit = sel_rd(element, 0x00u);
    fprintf(SEL_OUT,
            "[select24] slots tag=%u list=%08X elem=%08X idx=%u cmd=%08X "
            "t15=%02X o18=%04X c28=%08X c4C=%08X unit=%08X\n",
            tag, list, element, index, command,
            (sel_rd(command, 0x14u) >> 8) & 0xFFu,
            sel_rd(command, 0x18u) & 0xFFFFu,
            sel_rd(list, 0x28u), sel_rd(list, 0x4Cu), unit);
    sel_restore_state(c, &saved);
}

/*
 * Inserted in 0x6F6B93A0 after the calls to 0x6F04CDD0, which are the last
 * points where a per-unit order can still be rejected: the caller returns
 * without touching the unit when the result is zero.  A unit with result=0
 * is a unit that received the order event but did not get the order, which
 * is exactly the class of defect the 12-unit symptom needs.  The function
 * has two delivery phases; phase says which one this result belongs to.
 */
void wc3_sel_trace_order_apply(CPU *c, uint32_t unit, uint32_t event,
                                 uint32_t result, uint32_t phase)
{
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    fprintf(SEL_OUT,
            "[select24] apply seq=%u cap=%u phase=%u unit=%08X event=%08X "
            "result=%08X unit14=%08X unit18=%08X uD8=%08X u1FC=%08X "
            "u200=%08X e14=%08X e18=%08X e24=%08X e28=%08X e2C=%08X "
            "e30=%08X e34=%08X e54=%08X\n",
            g_sel_order_seq, wc3_sel_cap(), phase, unit, event, result,
            sel_rd(unit, 0x14u), sel_rd(unit, 0x18u),
            sel_rd(unit, 0xD8u), sel_rd(unit, 0x1FCu),
            sel_rd(unit, 0x200u),
            sel_rd(event, 0x14u), sel_rd(event, 0x18u),
            sel_rd(event, 0x24u), sel_rd(event, 0x28u),
            sel_rd(event, 0x2Cu), sel_rd(event, 0x30u),
            sel_rd(event, 0x34u), sel_rd(event, 0x54u));
    sel_restore_state(c, &saved);
}

/*
 * Inserted in 0x6F6B93A0 around the virtual call that hands the order event
 * to the unit.  Stage 0 prints the unit, the event object, its vtable and the
 * handler that is about to run; stage 1 prints the handler result.  The
 * handler is the implementation that decides what the unit does with the
 * order, so a handler that fails or returns nothing for a particular unit is
 * the remaining explanation for a unit that ignores an accepted order.
 */
void wc3_sel_trace_order_exec(CPU *c, uint32_t stage, uint32_t unit,
                                uint32_t event, uint32_t vtbl,
                                uint32_t handler, uint32_t result)
{
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    if (stage == 0u) {
        fprintf(SEL_OUT,
                "[select24] exec seq=%u cap=%u unit=%08X event=%08X "
                "vtbl=%08X handler=%08X unit14=%08X unit18=%08X "
                "uD8=%08X u1FC=%08X u200=%08X "
                "e14=%08X e18=%08X e24=%08X e28=%08X e2C=%08X "
                "e30=%08X e34=%08X e54=%08X\n",
                g_sel_order_seq, wc3_sel_cap(), unit, event, vtbl,
                handler, sel_rd(unit, 0x14u), sel_rd(unit, 0x18u),
                sel_rd(unit, 0xD8u), sel_rd(unit, 0x1FCu),
                sel_rd(unit, 0x200u),
                sel_rd(event, 0x14u), sel_rd(event, 0x18u),
                sel_rd(event, 0x24u), sel_rd(event, 0x28u),
                sel_rd(event, 0x2Cu), sel_rd(event, 0x30u),
                sel_rd(event, 0x34u), sel_rd(event, 0x54u));
    } else {
        fprintf(SEL_OUT,
                "[select24] exec seq=%u result unit=%08X event=%08X "
                "result=%08X unit14=%08X unit18=%08X uD8=%08X u1FC=%08X "
                "u200=%08X e14=%08X e18=%08X e24=%08X e28=%08X e2C=%08X "
                "e30=%08X e34=%08X e54=%08X\n",
                g_sel_order_seq, unit, event, result,
                sel_rd(unit, 0x14u), sel_rd(unit, 0x18u),
                sel_rd(unit, 0xD8u), sel_rd(unit, 0x1FCu),
                sel_rd(unit, 0x200u),
                sel_rd(event, 0x14u), sel_rd(event, 0x18u),
                sel_rd(event, 0x24u), sel_rd(event, 0x28u),
                sel_rd(event, 0x2Cu), sel_rd(event, 0x30u),
                sel_rd(event, 0x34u), sel_rd(event, 0x54u));
    }
    sel_restore_state(c, &saved);
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
void wc3_sel_trace_remove(CPU *c, uint32_t phase, uint32_t group,
                            uint32_t unit)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    if (phase == 0u) {
        fprintf(SEL_OUT,
                "[select24] remove entry group=%08X head=%08X count=%u\n",
                group, sel_rd(group, 0x1F4u), sel_rd(group, 0x1F8u));
    } else if (phase == 2u) {
        fprintf(SEL_OUT,
                "[select24] remove node head=%08X node=%08X count=%u\n",
                group, unit, sel_rd(group, 0x1F8u));
    } else {
        fprintf(SEL_OUT,
                "[select24] removeunit group=%08X unit=%08X head=%08X "
                "count=%u\n",
                group, unit, sel_rd(group, 0x1F4u),
                sel_rd(group, 0x1F8u));
    }
    sel_restore_state(c, &saved);
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
void wc3_sel_trace_apply(CPU *c, uint32_t phase, uint32_t a, uint32_t b)
{
    Sel24CpuState saved;

    sel_save_state(c, &saved);
    if (phase == 0u) {
        fprintf(SEL_OUT,
                "[select24] orderapply result=%08X source=%08X\n", a, b);
    } else {
        fprintf(SEL_OUT,
                "[select24] orderev event=%08X source=%08X\n", a, b);
    }
    sel_restore_state(c, &saved);
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
void wc3_sel_trace_evbirth(CPU *c, uint32_t object, uint32_t desc)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    char probe[160];
    uint32_t klass = 0u;
    uint32_t handle = 0u;
    uint32_t serial = 0u;
    int ok;

    sel_save_state(c, &saved);
    if (sel_heap_word(object)) {
        klass = rd32(object + 0x10u);
        handle = rd32(object + 0x14u);
        serial = rd32(object + 0x18u);
    }
    sel_describe_handle(handle, serial, probe, sizeof(probe), &ok);
    fprintf(SEL_OUT,
            "[select24] evbirth object=%08X vt=%08X class=%08X handle=%08X "
            "serial=%u f50=%08X f54=%08X desc=%08X dclass=%08X dfactory=%08X "
            "darg=%08X d24=%08X d28=%08X %s\n",
            object, sel_rd(object, 0x00u), klass, handle, serial,
            sel_rd(object, 0x50u), sel_rd(object, 0x54u), desc,
            sel_rd(desc, 0x04u), sel_rd(desc, 0x0Cu),
            sel_rd(desc, 0x10u), sel_rd(desc, 0x24u),
            sel_rd(desc, 0x28u), probe);
    sel_restore_state(c, &saved);
}

/*
 * 0x6F1CBFB0 releases a slot of the negative handle table.  0x6F1CBFF0 links
 * the slot into the free list with `entry[index].value = *head; *head =
 * index`, so a second release of the same index links it to itself and the
 * next pop moves the head to 0xFFFFFFFE.  `handle` is the signed handle the
 * caller passed (negative for this table), `slotval` is the value stored in
 * the slot right now: 0xFFFFFFFE means the index was already released.
 */
void wc3_sel_trace_evrelease(CPU *c, uint32_t table, uint32_t handle)
{
    if (!sel_verbose_trace())
        return;
    Sel24CpuState saved;
    uint32_t index = ~handle; /* negative table: id is the complement of the slot */
    uint32_t base = 0u;
    uint32_t head = 0u;
    uint32_t count = 0u;
    uint32_t slotval = 0u;
    char probe[96];
    int ok;

    sel_save_state(c, &saved);
    if (sel_readable(table)) {
        base = sel_rd(table, 0x2Cu);
        head = sel_rd(table, 0x44u);
        count = sel_rd(table, 0x4Cu);
        if (sel_readable(base) && index < count)
            slotval = sel_rd(base, index * 8u);
    }
    sel_describe_handle(handle, 0u, probe, sizeof(probe), &ok);
    fprintf(SEL_OUT,
            "[select24] evrelease table=%08X handle=%08X index=%u head=%08X "
            "count=%u slotval=%08X %s\n",
            table, handle, index, head, count, slotval, probe);
    sel_restore_state(c, &saved);
}
#endif

/*
 * Call one lifted guest function.  args[] is in C order (leftmost first) and
 * is pushed right-to-left.  The return value is c->eax after the call.  The
 * caller's full integer state is restored, including esp, so a callee that
 * cleans its arguments (stdcall) and one that does not (cdecl) behave the
 * same way here.
 */
static uint32_t sel_invoke(CPU *c, uint32_t target, uint32_t ecx,
                             uint32_t ret, const uint32_t *args,
                             unsigned count)
{
    Sel24CpuState saved;
    uint32_t result;
    unsigned i;

    sel_save_state(c, &saved);
    c->ecx = ecx;
    for (i = count; i > 0u; --i) {
        push32(c, args[i - 1u]);
    }
    push32(c, ret);
    dispatch(c, target);
    result = c->eax;
    sel_restore_state(c, &saved);
    return result;
}

static uint32_t sel_alloc(CPU *c, uint32_t size, uint32_t line,
                            uint32_t ret)
{
    uint32_t args[4];

    args[0] = size;
    args[1] = SEL_ALLOC_FILE;
    args[2] = line;
    args[3] = 0u;
    return sel_invoke(c, SEL_ALLOC, 0u, ret, args, 4u);
}

static void sel_grid_ctor(CPU *c, uint32_t grid, uint32_t parent)
{
    uint32_t args[1];

    args[0] = parent;
    (void)sel_invoke(c, SEL_GRID_CTOR, grid, SEL_RET_GRID_CTOR,
                       args, 1u);
}

static void sel_grid_resize(CPU *c, uint32_t grid, uint32_t rows,
                              uint32_t cols)
{
    uint32_t args[2];

    args[0] = rows;
    args[1] = cols;
    (void)sel_invoke(c, SEL_GRID_RESIZE, grid, SEL_RET_RESIZE,
                       args, 2u);
}

static void sel_grid_insert(CPU *c, uint32_t grid, uint32_t col,
                              uint32_t row, uint32_t button)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = button;
    args[3] = 1u; /* flag: SetParent was already done by the button ctor */
    (void)sel_invoke(c, SEL_GRID_INSERT, grid, SEL_RET_INSERT,
                       args, 4u);
}

static void sel_grid_width(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel_invoke(c, SEL_GRID_WIDTH, grid, SEL_RET_WIDTH, args, 1u);
}

static void sel_grid_height(CPU *c, uint32_t grid, uint32_t value)
{
    uint32_t args[1];

    args[0] = value;
    (void)sel_invoke(c, SEL_GRID_HEIGHT, grid, SEL_RET_HEIGHT,
                       args, 1u);
}

static void sel_grid_anchor(CPU *c, uint32_t grid, uint32_t child_anchor,
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
    (void)sel_invoke(c, SEL_GRID_ANCHOR, grid, SEL_RET_ANCHOR,
                       args, 6u);
}

static void sel_button_ctor(CPU *c, uint32_t button, uint32_t grid)
{
    uint32_t args[1];

    args[0] = grid;
    (void)sel_invoke(c, SEL_BUTTON_CTOR, button, SEL_RET_BUTTON,
                       args, 1u);
}

static void sel_fill_cell(CPU *c, uint32_t group, uint32_t col,
                            uint32_t row, uint32_t unit, uint32_t flag)
{
    uint32_t args[4];

    args[0] = col;
    args[1] = row;
    args[2] = unit;
    args[3] = flag;
    (void)sel_invoke(c, SEL_FILL_CELL, group, SEL_RET_FILL, args, 4u);
}

static void sel_frame_method(CPU *c, uint32_t frame, uint32_t slot,
                               uint32_t ret, uint32_t visible)
{
    uint32_t vtable;
    uint32_t method;

    if (!frame) {
        return;
    }
    wr32(frame + SEL_FRAME_VISIBLE, visible);
    vtable = rd32(frame);
    if (!vtable) {
        return;
    }
    method = rd32(vtable + slot);
    if (method) {
        (void)sel_invoke(c, method, frame, ret, NULL, 0u);
    }
}

static uint32_t sel_valid_grid(uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t active;
    uint32_t grid40;

    if (!group) {
        return 0u;
    }
    /* Before init the added fields are outside the original 0x134 allocation
     * and may contain garbage; the original grid12 pointer is still valid. */
    if (rd32(group + SEL_GROUP_INIT) != SEL_INIT_MAGIC) {
        return rd32(group + SEL_GROUP_GRID12);
    }
    grid12 = rd32(group + SEL_GROUP_GRID12);
    grid24 = rd32(group + SEL_GROUP_GRID24);
    active = rd32(group + SEL_GROUP_ACTIVE);
    if (active == grid12 || (grid24 && active == grid24)) {
        return active;
    }
    grid40 = rd32(group + SEL_GROUP_GRID40);
    if (grid40 && active == grid40) {
        return active;
    }
    return grid12;
}

/*
 * Hide every real button of one grid without touching the grid frame.  The
 * original group hide-all assumes all 12 cells exist; the 24-cell grid can
 * contain null cells after a partial allocation, so every level is checked.
 */
static void sel_hide_grid(CPU *c, uint32_t grid)
{
    uint32_t rows;
    uint32_t cols;
    uint32_t row_array;
    uint32_t row;
    uint32_t col;

    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL_GRID_ROWS);
    cols = rd32(grid + SEL_GRID_COLS);
    row_array = rd32(grid + SEL_GRID_ROWS_AT);
    if (!row_array || rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }

    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL_ROW_STRIDE;
        uint32_t buttons = rd32(row_desc + SEL_ROW_BUTTONS);
        if (!buttons) {
            continue;
        }
        for (col = 0u; col < cols; ++col) {
            uint32_t button = rd32(buttons + col * 4u);
            if (button) {
                sel_frame_method(c, button, SEL_VT_HIDE, SEL_RET_HIDE,
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
static void sel_ensure_visible(CPU *c, uint32_t grid)
{
    if (!grid) {
        return;
    }
    if (rd32(grid + SEL_FRAME_VISIBLE) == 0u ||
        rd32(grid + SEL_FRAME_REGISTERED) == 0u) {
        sel_hide_grid(c, grid);
        sel_frame_method(c, grid, SEL_VT_SHOW, SEL_RET_SHOW, 1u);
    }
}

/*
 * Check that the resize really produced the requested rows/cols and that
 * every row has a button array.  This keeps the helper fail-closed when the
 * grid or a row array could not be allocated.
 */
static int sel_grid_ready(uint32_t grid, uint32_t rows, uint32_t cols)
{
    uint32_t row_array;
    uint32_t row;

    if (!grid) {
        return 0;
    }
    if (rd32(grid + SEL_GRID_ROWS) != rows ||
        rd32(grid + SEL_GRID_COLS) != cols) {
        return 0;
    }
    row_array = rd32(grid + SEL_GRID_ROWS_AT);
    if (!row_array) {
        return 0;
    }
    for (row = 0u; row < rows; ++row) {
        uint32_t row_desc = row_array + row * SEL_ROW_STRIDE;
        if (!rd32(row_desc + SEL_ROW_BUTTONS)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Allocate, initialise and populate one extra panel grid.  Returns zero when
 * the grid, a row array or a button could not be allocated; the partial grid
 * is then hidden and left as a dormant child of the group, so the 6x2 path
 * stays intact.  A successful grid is also hidden: it is only shown when the
 * update selects it.
 */
static uint32_t sel_create_grid(CPU *c, uint32_t group, uint32_t rows,
                                  uint32_t cols, uint32_t scale,
                                  uint32_t width, uint32_t height,
                                  uint32_t offset_x, uint32_t offset_y)
{
    uint32_t grid;
    uint32_t row;
    uint32_t col;

    grid = sel_alloc(c, SEL_GRID_SIZE, SEL_GRID_LINE,
                       SEL_RET_GRID_ALLOC);
    if (!grid) {
        return 0u;
    }
    sel_grid_ctor(c, grid, group);
    wr32(grid + SEL_GRID_SCALE, scale);
    sel_grid_width(c, grid, width);
    sel_grid_height(c, grid, height);
    sel_grid_anchor(c, grid, 7u, group, 7u, offset_x, offset_y, 1u);
    sel_grid_resize(c, grid, rows, cols);
    if (!sel_grid_ready(grid, rows, cols)) {
        wr32(grid + SEL_GRID_VISIBLE, 0u);
        sel_frame_method(c, grid, SEL_VT_HIDE, SEL_RET_HIDE, 0u);
        sel_hide_grid(c, grid);
        return 0u;
    }
    for (row = 0u; row < rows; ++row) {
        for (col = 0u; col < cols; ++col) {
            uint32_t button = sel_alloc(c, SEL_BUTTON_SIZE,
                                          SEL_BUTTON_LINE,
                                          SEL_RET_BUTTON_ALLOC);

            if (!button) {
                wr32(grid + SEL_GRID_VISIBLE, 0u);
                sel_frame_method(c, grid, SEL_VT_HIDE, SEL_RET_HIDE,
                                   0u);
                sel_hide_grid(c, grid);
                return 0u;
            }
            sel_button_ctor(c, button, grid);
            sel_grid_insert(c, grid, col, row, button);
        }
    }
    wr32(grid + SEL_GRID_VISIBLE, 0u);
    sel_frame_method(c, grid, SEL_VT_HIDE, SEL_RET_HIDE, 0u);
    sel_hide_grid(c, grid);
    return grid;
}

void wc3_sel_grid_init(CPU *c, uint32_t group)
{
    uint32_t grid12;
    uint32_t grid24 = 0u;
    uint32_t grid40 = 0u;

    if (!group) {
        return;
    }
    if (rd32(group + SEL_GROUP_INIT) == SEL_INIT_MAGIC) {
        return;
    }

    grid12 = rd32(group + SEL_GROUP_GRID12);
    wr32(group + SEL_GROUP_INIT, SEL_INIT_MAGIC);
    wr32(group + SEL_GROUP_GRID24, 0u);
    wr32(group + SEL_GROUP_GRID40, 0u);
    wr32(group + SEL_GROUP_ACTIVE, grid12);
    wr32(group + SEL_GROUP_TOTAL, 0u);
    if (wc3_sel_cap() < 24u) {
        /* Runtime switch is off: keep the original 6x2 path and do not
         * allocate the extra grid.  The marker is already set, so note/select/
         * fill resolve to grid12 without touching the missing +0x134 slot. */
#if WC3_DEBUG && WC3_DEBUG_SELECTION
        fprintf(SEL_OUT, "[select24] init group=%08X grid12=%08X mode=12\n",
                group, grid12);
#endif
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] init group=%08X grid12=%08X marker=%08X cap=%u\n",
            group, grid12, (unsigned)SEL_INIT_MAGIC, wc3_sel_cap());
#endif
    if (!grid12) {
        return;
    }

    grid24 = sel_create_grid(c, group, SEL_GRID24_ROWS, SEL_GRID24_COLS,
                               SEL_GRID24_SCALE, SEL_GRID24_WIDTH,
                               SEL_GRID24_HEIGHT, SEL_GRID24_OFFSET_X,
                               SEL_GRID24_OFFSET_Y);
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT, "[select24] init group=%08X grid24=%08X\n",
            group, grid24);
#endif
    wr32(group + SEL_GROUP_GRID24, grid24);

    if (wc3_sel_cap() >= 40u) {
        grid40 = sel_create_grid(c, group, SEL_GRID40_ROWS,
                                   SEL_GRID40_COLS, SEL_GRID40_SCALE,
                                   SEL_GRID40_WIDTH, SEL_GRID40_HEIGHT,
                                   SEL_GRID40_OFFSET_X,
                                   SEL_GRID40_OFFSET_Y);
#if WC3_DEBUG && WC3_DEBUG_SELECTION
        fprintf(SEL_OUT, "[select24] init group=%08X grid40=%08X\n",
                group, grid40);
#endif
        wr32(group + SEL_GROUP_GRID40, grid40);
    }

    wr32(group + SEL_GROUP_ACTIVE, grid12);
    wr32(grid12 + SEL_GRID_VISIBLE, 1u);
}

/*
 * Called from the update prologue with the CSelectionWar3 object.  The full
 * update function and the split entry fragments share the same stack frame,
 * but only the prologue has the selection pointer.  Keep the total count in
 * the group object so the mode choice works in every fragment.
 */
void wc3_sel_panel_note(CPU *c, uint32_t group, uint32_t selection)
{
    uint32_t total = 0u;

    (void)c;
    if (!group) {
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] note group=%08X marker=%08X grid12=%08X "
            "selection=%08X\n",
            group, rd32(group + SEL_GROUP_INIT),
            rd32(group + SEL_GROUP_GRID12), selection);
#endif
    if (rd32(group + SEL_GROUP_INIT) != SEL_INIT_MAGIC) {
        return;
    }
    /* init only writes the marker after it has seen a real grid12; requiring
     * the same pointer here keeps a chance marker match on a short allocation
     * from turning into an out-of-bounds write at +0x140. */
    if (!rd32(group + SEL_GROUP_GRID12)) {
        return;
    }
    if (selection) {
        total = rd32(selection + SEL_SELECTION_TOTAL);
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] note group=%08X selection=%08X total=%u stored=%u "
            "cap=%u\n",
            group, selection, total, (unsigned)SEL_SELECTION_TOTAL,
            wc3_sel_cap());
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
        uint32_t root = rd32(selection + SEL_SELECTION_HEAD);

        if (!sel_in_range(root, SEL_GUEST_HEAP_LO, 0x70000000u) ||
            (root & 3u) != 0u) {
            fprintf(SEL_OUT,
                    "[select24] seldump root=%08X bad-root total=%u\n", root,
                    total);
        } else {
            uint32_t obj = rd32(root + 8u);

            if (!sel_in_range(obj, SEL_GUEST_HEAP_LO, 0x70000000u) ||
                (obj & 3u) != 0u) {
                fprintf(SEL_OUT,
                        "[select24] seldump root=%08X obj=%08X bad-object "
                        "total=%u\n",
                        root, obj, total);
            } else {
                uint32_t handle = rd32(obj + 0x0Cu);
                uint32_t serial = rd32(obj + 0x10u);
                char probe[160];
                int ok = 0;

                sel_describe_handle(handle, serial, probe, sizeof(probe),
                                      &ok);
                fprintf(SEL_OUT,
                        "[select24] seldump root=%08X obj=%08X handle=%08X "
                        "serial=%u total=%u %s\n",
                        root, obj, handle, serial, total, probe);
            }
        }
    }
#endif
    wr32(group + SEL_GROUP_TOTAL, total);
}

void wc3_sel_panel_select(CPU *c, uint32_t group, uint32_t subgroup_units)
{
    uint32_t grid12;
    uint32_t grid24;
    uint32_t grid40 = 0u;
    uint32_t desired;
    uint32_t old;
    uint32_t total;

    if (!group) {
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] select group=%08X marker=%08X subgroup=%u\n",
            group, rd32(group + SEL_GROUP_INIT), subgroup_units);
#endif
    if (rd32(group + SEL_GROUP_INIT) != SEL_INIT_MAGIC) {
        return;
    }
    grid12 = rd32(group + SEL_GROUP_GRID12);
    grid24 = rd32(group + SEL_GROUP_GRID24);
    grid40 = rd32(group + SEL_GROUP_GRID40);
    if (!grid12) {
        return;
    }

    total = rd32(group + SEL_GROUP_TOTAL);
    if (total == 0u || total > SEL_STRUCT_CAP) {
        /* The split-entry fragment may run without the update prologue.  The
         * current subgroup's unit count is a safe lower bound: it never asks
         * for more cells than this fragment can fill. */
        total = subgroup_units;
    }

    if (wc3_sel_cap() >= 40u && grid40 &&
        total > SEL_GRID12_ROWS * SEL_GRID12_COLS &&
        (total > SEL_GRID24_ROWS * SEL_GRID24_COLS || !grid24)) {
        desired = grid40;
    } else if (wc3_sel_cap() >= 24u &&
               total > SEL_GRID12_ROWS * SEL_GRID12_COLS && grid24) {
        desired = grid24;
    } else {
        desired = grid12;
    }

    old = rd32(group + SEL_GROUP_ACTIVE);
    if (old != grid12 && (grid24 == 0u || old != grid24) &&
        (grid40 == 0u || old != grid40)) {
        old = grid12;
        wr32(group + SEL_GROUP_ACTIVE, old);
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] select group=%08X subgroup=%u total=%u grid12=%08X "
            "grid24=%08X grid40=%08X active=%08X desired=%08X\n",
            group, subgroup_units, total, grid12, grid24, grid40, old,
            desired);
#endif
    if (desired == old) {
        /* A parent HideAll can unregister the active grid; ShowAll would not
         * restore it because its own visible flag is zero. */
        sel_ensure_visible(c, desired);
        return;
    }

    /* Remove stale buttons from the target grid first, then swap frames.
     * The fill path that follows shows only the buttons it actually fills. */
    sel_hide_grid(c, desired);
    sel_frame_method(c, old, SEL_VT_HIDE, SEL_RET_HIDE, 0u);
    sel_frame_method(c, desired, SEL_VT_SHOW, SEL_RET_SHOW, 1u);
    wr32(group + SEL_GROUP_ACTIVE, desired);
}

void wc3_sel_panel_hide(CPU *c, uint32_t group)
{
    uint32_t grid = sel_valid_grid(group);

#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT, "[select24] hide group=%08X grid=%08X\n",
            group, grid);
#endif
    if (grid) {
        sel_hide_grid(c, grid);
    }
}

void wc3_sel_panel_fill(CPU *c, uint32_t group, uint32_t index,
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
    grid = sel_valid_grid(group);
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT, "[select24] fill group=%08X index=%u grid=%08X\n",
            group, index, grid);
#endif
    if (!grid) {
        return;
    }
    rows = rd32(grid + SEL_GRID_ROWS);
    cols = rd32(grid + SEL_GRID_COLS);
    if (rows == 0u || cols == 0u || rows > 64u || cols > 64u) {
        return;
    }
    if (index >= rows * cols) {
        return;
    }

    row = index / cols;
    col = index % cols;
    row_array = rd32(grid + SEL_GRID_ROWS_AT);
    if (!row_array) {
        return;
    }
    row_desc = row_array + row * SEL_ROW_STRIDE;
    buttons = rd32(row_desc + SEL_ROW_BUTTONS);
    if (!buttons) {
        return;
    }
    button = rd32(buttons + col * 4u);
    if (!button) {
#if WC3_DEBUG && WC3_DEBUG_SELECTION
        fprintf(SEL_OUT,
                "[select24] fill group=%08X index=%u button=0\n",
                group, index);
#endif
        return;
    }

#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] fill group=%08X index=%u unit=%08X flag=%u grid=%08X "
            "rows=%u cols=%u row=%u col=%u button=%08X\n",
            group, index, unit, flag, grid, rows, cols, row, col, button);
#endif
    sel_fill_cell(c, group, col, row, unit, flag);
}

/*
 * 40-entry ground-order formation workspace.
 *
 * wc3_sel_workspace_init replaces the L_6F169300 -> L_6F169310 zeroing sequence for
 * the widened solver frame: the original zeroes 12 pairs (2 * 0x30 bytes),
 * this one zeroes 40 pairs at count+0xA4.
 */
void wc3_sel_workspace_init(uint32_t pair_base)
{
    uint32_t i;

    for (i = 0u; i < SEL_STRUCT_CAP; ++i) {
        wr32(pair_base + i * 8u, 0u);
        wr32(pair_base + i * 8u + 4u, 0u);
    }
}

/*
 * Selection-group entry snapshot.  The entry is 0x1F4 bytes: count +0,
 * index[40] +4, pair[40] +0xA4 and tail +0x1E4.  The trace deliberately
 * prints the tail on every call and the full index/pair arrays only when the
 * count exceeds the old twelve-entry capacity, so a normal 12-unit order
 * stays one summary line while the failing 14-unit case leaves a complete
 * before/after record.
 */
#if WC3_DEBUG && WC3_DEBUG_SELECTION
static void sel_trace_group_snapshot(const char *stage, uint32_t group)
{
    uint32_t cap = wc3_sel_cap();
    uint32_t count = rd32(group);
    uint32_t n = count < SEL_STRUCT_CAP ? count : SEL_STRUCT_CAP;
    uint32_t chk = count ^ 0x9E3779B9u;
    uint32_t tail[4];
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        uint32_t idx = rd32(group + SEL_GROUP_INDEX + i * 4u);
        uint32_t key = rd32(group + SEL_GROUP_PAIR + i * 8u);
        uint32_t val = rd32(group + SEL_GROUP_PAIR + i * 8u + 4u);

        chk += idx * 0x85EBCA6Bu ^ key * 0xC2B23AE3u ^ val;
        if (count > 12u)
            fprintf(SEL_OUT,
                    "[select24] group %s i=%u idx=%08X key=%08X val=%08X\n",
                    stage, i, idx, key, val);
    }
    for (i = 0u; i < 4u; ++i) {
        tail[i] = rd32(group + SEL_GROUP_TAIL + i * 4u);
        chk += tail[i];
    }
    fprintf(SEL_OUT,
            "[select24] group %s seq=%u ptr=%08X count=%u cap=%u%s "
            "tail=%08X,%08X,%08X,%08X chk=%08X\n",
            stage, g_sel_order_seq, group, count, cap,
            count > cap ? " OVER" : "",
            tail[0], tail[1], tail[2], tail[3], chk);
}
#endif

/*
 * Called by the LiftPass at 0x6F16A612 for each of the sixteen selection
 * groups.  The snapshot is taken before zeroing, so a reused entry shows the
 * tail left behind by an earlier order; the sort snapshots then show whether
 * D270/CB80 installed a fresh one and whether the sort preserved it.
 */
void wc3_sel_group_init(uint32_t pair_base)
{
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    if (pair_base >= SEL_GROUP_PAIR)
        sel_trace_group_snapshot("init", pair_base - SEL_GROUP_PAIR);
#else
    (void)pair_base;
#endif
    wc3_sel_workspace_init(pair_base);
    /* The original L_6F169300 zeroed only the pair array (12 entries); the
     * count at group+0 was left on the stack. On a fresh frame that is
     * zero, but a reused frame (loading a save with >12 selected units)
     * leaves garbage, and L_6F16A6E0 then feeds an uninitialized group to
     * wc3_sel_workspace_sort, which walks the pair array with a huge count and
     * faults. Zero the count so empty groups stay empty. The fill writer
     * (L_6F16CB80) overwrites it for the groups that actually carry units. */
    if (pair_base >= SEL_GROUP_PAIR)
        wr32(pair_base - SEL_GROUP_PAIR, 0u);
}

/*
 * The former formation-cursor carry was removed in PASS_REV 52.  It kept a
 * process-global X coordinate between unrelated orders and let one cluster's
 * point become another unit's move target.  The original base constant and
 * per-cluster local are used again; no global formation state exists here.
 */

/*
 * Formation row length for the selection-group order path.
 *
 * L_6F16D270 (and its split copies L_6F16D286 / L_6F16D2D3) computes
 * index = 13 * flag + count and reads a 32-bit table word; the value becomes
 * min(count, word) and is stored at group+0x12C.  L_6F16A6E0 then passes that
 * value as the batch step of the order loop.  The 1.27 table has four blocks
 * of 13 entries:
 *
 *   A 0x6FA91D20: 0 1 2 3 2 3 3 3 3 3 4 4 4
 *   B 0x6FA91D54: 0 1 2 3 2 3 3 3 3 3 3 3 3
 *   C 0x6FA91D88: 0 1 2 3 4 3 3 4 4 4 4 4 4
 *   D 0x6FA91DBC: 0 1 2 3 3 3 3 3 3 3 3 3 3
 *
 * Base 0x6FA91D20 selects A (flag 0) or B (flag 1); base 0x6FA91D88 selects
 * C or D.  The table was sized for the 12-unit cap, so count 13 with flag 0
 * reads B[0] and count 13 with flag 1 reads C[0]; both are zero.  A zero row
 * length makes the order loop spin forever because its index never advances
 * (observed with a 13-unit group after the Rev 58 shift fix).
 *
 * The table ends at 0x6FA91DF0, immediately before the list-object vtable at
 * 0x6FA91DF4, so it cannot be extended in place.  This helper keeps the
 * original read for count <= 12 (bit-exact baseline) and defines the 13..40
 * range used by the 24/40-unit profile:
 *
 *   flag 1 (B/D, compact mode; 3 from count 6 on) -> 3
 *   flag 0 (A/C, grid mode) -> ceil(count / 3) for 13..24
 *                           -> ceil(count / 4) for 25..40
 *
 * The grid rule continues the table's own three-row shape: 13..15 -> 5,
 * 16..18 -> 6, 19..21 -> 7, 22..24 -> 8, which also matches the 8x3 panel
 * grid used for 24 units, and the four-row shape 25..28 -> 7, 29..32 -> 8,
 * 33..36 -> 9, 37..40 -> 10, which matches the 10x4 panel grid.  Any value
 * >= 1 makes the loop advance; the loop clamps the step to the number of
 * units that are left.
 */
uint32_t wc3_sel_formation_columns(uint32_t index, uint32_t count,
                                     uint32_t pair)
{
    uint32_t flag;
    uint32_t value;

    if (count <= 12u)
        return rd32(index * 4u +
                    GVA(pair ? 0x6FA91D88u : 0x6FA91D20u));

    flag = (index - count) / 13u;
    if (flag != 0u)
        value = 3u;
    else if (count <= 24u)
        value = (count + 2u) / 3u;
    else
        value = (count + 3u) / 4u;
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    fprintf(SEL_OUT,
            "[select24] formation columns idx=%u count=%u pair=%u value=%u\n",
            index, count, pair, value);
#endif
    return value;
}

/* Bit-exact float load for the sort comparison. */
static float sel_f32(uint32_t bits)
{
    union {
        uint32_t u;
        float f;
    } v;

    v.u = bits;
    return v.f;
}

/*
 * wc3_sel_workspace_sort is a transliteration of L_6F16DFD0 with the 40-entry
 * workspace layout: count at count_ptr+0, index at count_ptr+4 (40 dwords)
 * and pair at count_ptr+0xA4 (40 pairs, second dword at +0xA8).  The control
 * flow, comparisons, swap/rotate and index updates are the lifted ones; only
 * the two pair-base displacements differ from the original 12-entry sort.
 * L_6F16DFD0 itself is left in place for reference; the solver calls sort24
 * directly and the selection group goes through wc3_sel_group_sort().
 */
#if WC3_DEBUG && WC3_DEBUG_SELECTION
/*
 * Read-only dump of the formation sort workspace.  The count is the first
 * value the solver reads; a count above the runtime cap explains a hang even
 * before the insertion-sort loop is entered.  The index and pair arrays are
 * printed in workspace order, not sorted order, so the caller can see what
 * the solver received and what it produced.
 */
static void sel_trace_sort_workspace(const char *stage, uint32_t count_ptr,
                                       uint32_t count)
{
    uint32_t cap = wc3_sel_cap();
    uint32_t i;

    if (!sel_verbose_trace())
        return;
    (void)count_ptr;
    fprintf(SEL_OUT,
            "[select24] sort %s seq=%u ptr=%08X count=%u cap=%u%s\n",
            stage, g_sel_order_seq, count_ptr, count, cap,
            count > cap ? " OVER" : "");
    for (i = 0u; i < count && i < cap; ++i)
        fprintf(SEL_OUT,
                "[select24] sort %s i=%u idx=%u pair=%08X key=%08X\n",
                stage, i, sel_rd(count_ptr, SEL_GROUP_INDEX + i * 4u),
                sel_rd(count_ptr, SEL_GROUP_PAIR + i * 8u),
                sel_rd(count_ptr, SEL_GROUP_PAIR + i * 8u + 4u));
}
#endif

void wc3_sel_workspace_sort(uint32_t count_ptr)
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
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    sel_trace_sort_workspace("begin", count_ptr, count);
#endif
    if (count <= 1u)
        return;

    pair_end = count_ptr + SEL_GROUP_PAIR + count * 8u;
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
                if (!(sel_f32(key_bits) > rdss(esi)))
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
                edx = rd32(ecx + ebx * 8u + SEL_GROUP_PAIR);
                ecx = saved_pair;
                wr32(ecx, edx);
                edx = rd32(count_ptr + ebx * 8u + SEL_GROUP_PAIR + 4u);
                wr32(ecx + 4u, edx);
                edx = count_ptr;
                ecx = key_orig;
                wr32(edx + ebx * 4u + 4u, esi);
                wr32(edx + ebx * 8u + SEL_GROUP_PAIR, ecx);
                wr32(edx + ebx * 8u + SEL_GROUP_PAIR + 4u, eax);
            }
        }

        if (ebp_edi <= 1u)
            break;
    }
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    sel_trace_sort_workspace("end", count_ptr, count);
#endif
}

/*
 * Called by the LiftPass at 0x6F16A6EF instead of the 12-entry sort.  The
 * pre/post snapshots are the regression witness: if the old pair stride had
 * remained, pair 12/13 would land on +0x94/+0x9C and the post tail/checksum
 * would differ from the pre record.  With the 0x1F4 layout the tail is at
 * +0x1E4..+0x1F0 and must survive unchanged.
 */
void wc3_sel_group_sort(uint32_t group)
{
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    sel_trace_group_snapshot("sort-pre", group);
#else
    (void)group;
#endif
    wc3_sel_workspace_sort(group);
#if WC3_DEBUG && WC3_DEBUG_SELECTION
    sel_trace_group_snapshot("sort-post", group);
#endif
}
