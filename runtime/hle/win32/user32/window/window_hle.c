/*
 * window_hle.c - target-side USER32 window/display facts.
 *
 * GetSystemMetrics is the first USER32 handler to leave the as-built monolith: its
 * only external input is the current display mode, which the shim already
 * exposes.  The as-built default of 800 for unrelated metrics is preserved:
 * the game uses those metrics only as loose layout hints, and changing them
 * without a device measurement would be a guess.
 *
 * Window properties are self-contained too.  They are an atom table keyed by
 * the guest HWND; the value is an opaque 32-bit handle, so the target does not
 * need a real window tree to preserve SetPropA/GetPropA/RemovePropA.  The
 * table is split into shards with one mutex each; a lookup holds only the
 * shard lock that covers the slot it reads.
 */
#include "window_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "cpu.h"
#include "hle_bridge.h"

#define DESKTOP_WINDOW_HANDLE 0x00010000u
#define PROPERTY_TABLE_SIZE 256u
#define PROPERTY_NAME_SIZE 256u
#define PROPERTY_SHARD_SIZE 32u
#define PROPERTY_SHARD_COUNT (PROPERTY_TABLE_SIZE / PROPERTY_SHARD_SIZE)

_Static_assert(PROPERTY_TABLE_SIZE % PROPERTY_SHARD_SIZE == 0u,
               "property shards must cover the table");

static int32_t g_cursor_display_count;

/* Window properties are an atom table in Win32: the name is case-insensitive
 * and the value is an opaque 32-bit handle.  The target has no real HWND tree,
 * so the table keys on the guest handle exactly as the guest passes it; that
 * preserves the property contract without pretending to own a window. */
typedef struct User32Property {
    uint32_t hwnd;
    uint32_t value;
    char name[PROPERTY_NAME_SIZE];
    uint8_t used;
} User32Property;

/* g_property_table_lock serializes writers.  It makes the first-match scan,
 * the first-empty insertion choice, and the shard pin one atomic step, so
 * SetProp cannot create a duplicate key.  Once a slot is pinned, a writer may
 * drop the table lock and finish the slot-local work under its shard lock.
 * g_property_shard_locks[s] owns the structure and value of the
 * PROPERTY_SHARD_SIZE contiguous slots in shard s.  A lookup takes only the
 * shard lock covering the slot it reads and keeps it across the value read, so
 * a concurrent SetProp/RemoveProp cannot clear or reuse that slot underneath
 * it; lookups never take the table lock.  Writers take table -> shard, and no
 * operation takes the table lock while holding a shard lock. */
static User32Property g_properties[PROPERTY_TABLE_SIZE];
static pthread_mutex_t g_property_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_property_shard_locks[PROPERTY_SHARD_COUNT];
static pthread_once_t g_property_shard_locks_once = PTHREAD_ONCE_INIT;

static int read_property_name(uint32_t address, char *buffer)
{
    if (!address || !buffer) return 0;
    for (size_t i = 0; i + 1u < PROPERTY_NAME_SIZE; i++) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t byte_address = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(byte_address, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(byte_address);
        buffer[i] = ch;
        if (!ch) return i != 0u;
    }
    buffer[PROPERTY_NAME_SIZE - 1u] = '\0';
    return 0;
}

static void init_property_shard_locks(void)
{
    for (unsigned i = 0; i < PROPERTY_SHARD_COUNT; i++)
        pthread_mutex_init(&g_property_shard_locks[i], NULL);
}

static void ensure_property_shard_locks(void)
{
    (void)pthread_once(&g_property_shard_locks_once, init_property_shard_locks);
}

static int property_matches(const User32Property *property, uint32_t hwnd,
                            const char *name)
{
    return property->used && property->hwnd == hwnd &&
           strcasecmp(property->name, name) == 0;
}

static unsigned property_shard_of(unsigned index)
{
    return index / PROPERTY_SHARD_SIZE;
}

/* Caller holds g_property_table_lock.  Writers publish used/hwnd/name under
 * the table lock (and the slot's shard), so this scan is stable without taking
 * the shard locks.  RemoveProp clears used before dropping the table lock, so
 * a slot retired later in its shard lock is already invisible here.  Returns
 * the first matching slot, or PROPERTY_TABLE_SIZE when the property is
 * absent. */
static unsigned find_property_locked(uint32_t hwnd, const char *name)
{
    for (unsigned i = 0; i < PROPERTY_TABLE_SIZE; i++) {
        if (property_matches(&g_properties[i], hwnd, name)) return i;
    }
    return PROPERTY_TABLE_SIZE;
}

static void h_get_prop(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    char name[PROPERTY_NAME_SIZE];
    c->eax = 0;
    if (!hwnd || !read_property_name(wc3_legacy_arg32(c, 1u), name))
        return;

    /* Shards are scanned in index order, so the first matching slot is the
     * same slot the former single-lock scan returned. */
    for (unsigned shard = 0; shard < PROPERTY_SHARD_COUNT; shard++) {
        pthread_mutex_lock(&g_property_shard_locks[shard]);
        const unsigned base = shard * PROPERTY_SHARD_SIZE;
        for (unsigned offset = 0; offset < PROPERTY_SHARD_SIZE; offset++) {
            const unsigned index = base + offset;
            if (property_matches(&g_properties[index], hwnd, name)) {
                c->eax = g_properties[index].value;
                pthread_mutex_unlock(&g_property_shard_locks[shard]);
                return;
            }
        }
        pthread_mutex_unlock(&g_property_shard_locks[shard]);
    }
}

static void h_set_prop(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t value = wc3_legacy_arg32(c, 2u);
    char name[PROPERTY_NAME_SIZE];
    c->eax = 0;
    if (!hwnd || !read_property_name(wc3_legacy_arg32(c, 1u), name))
        return;

    pthread_mutex_lock(&g_property_table_lock);
    const unsigned index = find_property_locked(hwnd, name);
    if (index < PROPERTY_TABLE_SIZE) {
        /* The table lock proves the index and pins the shard.  Only the value
         * changes, so the table lock can be dropped before the write. */
        const unsigned shard = property_shard_of(index);
        pthread_mutex_lock(&g_property_shard_locks[shard]);
        pthread_mutex_unlock(&g_property_table_lock);
        g_properties[index].value = value;
        pthread_mutex_unlock(&g_property_shard_locks[shard]);
        c->eax = 1;
        return;
    }
    for (unsigned i = 0; i < PROPERTY_TABLE_SIZE; i++) {
        if (g_properties[i].used) continue;
        /* The table lock keeps the chosen empty slot stable; the shard lock
         * keeps a lookup from observing a half-written slot. */
        const unsigned shard = property_shard_of(i);
        pthread_mutex_lock(&g_property_shard_locks[shard]);
        g_properties[i].hwnd = hwnd;
        g_properties[i].value = value;
        const size_t name_length = strlen(name) + 1u;
        memcpy(g_properties[i].name, name, name_length);
        g_properties[i].used = 1;
        pthread_mutex_unlock(&g_property_shard_locks[shard]);
        c->eax = 1;
        break;
    }
    pthread_mutex_unlock(&g_property_table_lock);
}

static void h_remove_prop(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    char name[PROPERTY_NAME_SIZE];
    c->eax = 0;
    if (!hwnd || !read_property_name(wc3_legacy_arg32(c, 1u), name))
        return;

    pthread_mutex_lock(&g_property_table_lock);
    const unsigned index = find_property_locked(hwnd, name);
    if (index < PROPERTY_TABLE_SIZE) {
        /* Pin the shard while the table still proves the index.  Retire the
         * slot (used is the table-owned structural bit) before dropping the
         * table; a reuser that sees used=0 must still take this shard. */
        const unsigned shard = property_shard_of(index);
        pthread_mutex_lock(&g_property_shard_locks[shard]);
        const uint32_t removed_value = g_properties[index].value;
        g_properties[index].used = 0u;
        pthread_mutex_unlock(&g_property_table_lock);
        g_properties[index].name[0] = '\0';
        pthread_mutex_unlock(&g_property_shard_locks[shard]);
        c->eax = removed_value;
    } else {
        pthread_mutex_unlock(&g_property_table_lock);
    }
}

static void h_get_desktop_window(CPU *c)
{
    c->eax = DESKTOP_WINDOW_HANDLE;
}

static void h_get_system_metrics(CPU *c)
{
    uint32_t width = 0, height = 0;
    if (!wc3_legacy_current_display_mode(&width, &height)) {
        width = 800;
        height = 600;
    }
    switch (wc3_legacy_arg32(c, 0)) {
    case 0:  /* SM_CXSCREEN */
    case 61: /* SM_CXMAXIMIZED */
    case 78: /* SM_CXVIRTUALSCREEN */
        c->eax = width;
        break;
    case 1:  /* SM_CYSCREEN */
    case 62: /* SM_CYMAXIMIZED */
    case 79: /* SM_CYVIRTUALSCREEN */
        c->eax = height;
        break;
    case 80: /* SM_CMONITORS */
        c->eax = 1;
        break;
    default:
        c->eax = 800;
        break;
    }
}

static void h_show_cursor(CPU *c)
{
    /* Win32 keeps a per-window-station display counter, initially 0 when a
     * mouse is installed.  Android has no system cursor, but the counter is
     * still guest-visible and the as-built "always return 1" was wrong after
     * the first call. */
    if (wc3_legacy_arg32(c, 0) != 0)
        g_cursor_display_count++;
    else
        g_cursor_display_count--;
    c->eax = (uint32_t)g_cursor_display_count;
}

static int32_t read_rect_field(uint32_t address)
{
    return (int32_t)wc3_legacy_rd32g(address);
}

static void write_rect_field(uint32_t address, int32_t value)
{
    wc3_legacy_wr32g(address, (uint32_t)value);
}

static void h_intersect_rect(CPU *c)
{
    const uint32_t destination = wc3_legacy_arg32(c, 0u);
    const uint32_t first = wc3_legacy_arg32(c, 1u);
    const uint32_t second = wc3_legacy_arg32(c, 2u);
    c->eax = 0;

    if (!wc3_legacy_guest_ok(destination, 16u) ||
        !wc3_legacy_guest_ok(first, 16u) ||
        !wc3_legacy_guest_ok(second, 16u))
        return;

    const int32_t left = read_rect_field(first) > read_rect_field(second)
                             ? read_rect_field(first)
                             : read_rect_field(second);
    const int32_t top = read_rect_field(first + 4u) >
                                read_rect_field(second + 4u)
                            ? read_rect_field(first + 4u)
                            : read_rect_field(second + 4u);
    const int32_t right = read_rect_field(first + 8u) <
                                  read_rect_field(second + 8u)
                              ? read_rect_field(first + 8u)
                              : read_rect_field(second + 8u);
    const int32_t bottom = read_rect_field(first + 12u) <
                                   read_rect_field(second + 12u)
                               ? read_rect_field(first + 12u)
                               : read_rect_field(second + 12u);

    if (left < right && top < bottom) {
        write_rect_field(destination, left);
        write_rect_field(destination + 4u, top);
        write_rect_field(destination + 8u, right);
        write_rect_field(destination + 12u, bottom);
        c->eax = 1;
    } else {
        write_rect_field(destination, 0);
        write_rect_field(destination + 4u, 0);
        write_rect_field(destination + 8u, 0);
        write_rect_field(destination + 12u, 0);
    }
}

typedef struct User32WindowOverride {
    const char *name;
    void (*impl)(CPU *c);
} User32WindowOverride;

static const User32WindowOverride g_overrides[] = {
    { "GetDesktopWindow", h_get_desktop_window },
    { "GetSystemMetrics", h_get_system_metrics },
    { "ShowCursor", h_show_cursor },
};

typedef struct User32WindowAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} User32WindowAddition;

static const User32WindowAddition g_additions[] = {
    { "GetPropA", 2, h_get_prop },
    { "SetPropA", 3, h_set_prop },
    { "RemovePropA", 2, h_remove_prop },
    { "IntersectRect", 3, h_intersect_rect },
};

int wc3_hle_user32_window_install(void)
{
    g_cursor_display_count = 0;
    ensure_property_shard_locks();

    /* Bootstrap reset.  Holding the table lock and every shard lock gives the
     * clear the same exclusivity the former single lock had. */
    pthread_mutex_lock(&g_property_table_lock);
    for (unsigned i = 0; i < PROPERTY_SHARD_COUNT; i++)
        pthread_mutex_lock(&g_property_shard_locks[i]);
    memset(g_properties, 0, sizeof g_properties);
    for (unsigned i = PROPERTY_SHARD_COUNT; i > 0u; i--)
        pthread_mutex_unlock(&g_property_shard_locks[i - 1u]);
    pthread_mutex_unlock(&g_property_table_lock);

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("USER32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
