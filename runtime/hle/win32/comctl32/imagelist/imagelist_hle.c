/*
 * imagelist_hle.c - target-side COMCTL32 image-list handles.
 *
 * The as-built bootstrap did not implement COMCTL32 at all, so every call was
 * an unknown import with guessed arity.  These rows supply the exact arity and
 * the observable handle/count semantics used by the game: Create returns a
 * fresh handle, Add returns the first new index, Replace validates an index,
 * GetImageCount reports the current count, Destroy invalidates the handle, and
 * the drag calls maintain one process-wide drag session.
 *
 * The table has three lock levels.  g_table_lock owns slot identity (the used
 * bit and handle/index validity) and first-free allocation.  g_list_locks[i]
 * owns slot i's payload (geometry, flags, count, capacity, grow).  g_drag_lock
 * owns the process-wide drag session.  A handle is pinned by taking the slot
 * mutex while the table lock is still held, so destroy can neither clear nor
 * reuse the slot before the caller owns it.  Lock order is table -> list ->
 * drag, and no path takes the table lock while holding a list or drag lock.
 * These handlers never touch guest memory or dispatch under a host lock.
 *
 * No imported COMCTL32 entry point exposes pixels: ImageList_Draw and
 * ImageList_GetIcon are absent from the 1.27 imports.  The target therefore
 * stores geometry and count, not a bitmap, and does not claim to render.
 */
#include "imagelist_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define IMAGE_LIST_TABLE_SIZE 64u
#define IMAGE_LIST_HANDLE_BASE 0x000B0000u
#define IMAGE_LIST_HANDLE_STRIDE 4u

typedef struct TargetImageList {
    uint8_t used;
    int32_t width;
    int32_t height;
    uint32_t flags;
    int32_t count;
    int32_t capacity;
    int32_t grow;
} TargetImageList;

static TargetImageList g_lists[IMAGE_LIST_TABLE_SIZE];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_list_locks[IMAGE_LIST_TABLE_SIZE];
static pthread_once_t g_list_locks_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_drag_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_drag_handle;
static int32_t g_drag_index;
static uint32_t g_drag_lock_window;
static uint32_t g_drag_position_packed;
static int g_drag_active;
static int g_drag_visible;

static void init_list_locks(void)
{
    for (unsigned i = 0; i < IMAGE_LIST_TABLE_SIZE; i++)
        pthread_mutex_init(&g_list_locks[i], NULL);
}

static void ensure_list_locks(void)
{
    (void)pthread_once(&g_list_locks_once, init_list_locks);
}

static uint32_t handle_for_index(unsigned index)
{
    return IMAGE_LIST_HANDLE_BASE + index * IMAGE_LIST_HANDLE_STRIDE;
}

/* Caller holds g_table_lock.  The used bit and handle mapping change only
 * under that lock, so a resolved index stays valid while it is held. */
static int index_for_handle_locked(uint32_t handle, unsigned *index_out)
{
    if (!handle || handle < IMAGE_LIST_HANDLE_BASE) return 0;
    const uint32_t offset = handle - IMAGE_LIST_HANDLE_BASE;
    if (offset % IMAGE_LIST_HANDLE_STRIDE != 0u) return 0;
    const uint32_t index = offset / IMAGE_LIST_HANDLE_STRIDE;
    if (index >= IMAGE_LIST_TABLE_SIZE) return 0;
    if (!g_lists[index].used) return 0;
    if (index_out) *index_out = index;
    return 1;
}

/* Caller holds g_table_lock.  Preserves the first-free allocation order. */
static unsigned find_free_index_locked(void)
{
    for (unsigned i = 0; i < IMAGE_LIST_TABLE_SIZE; i++) {
        if (!g_lists[i].used) return i;
    }
    return IMAGE_LIST_TABLE_SIZE;
}

/* Resolve and pin a live handle.  Taking the slot mutex before dropping the
 * table lock keeps destroy from clearing or reusing the slot underneath the
 * caller; only the slot lock is held on return. */
static int pin_handle(uint32_t handle, unsigned *index_out)
{
    pthread_mutex_lock(&g_table_lock);
    unsigned index = 0;
    if (!index_for_handle_locked(handle, &index)) {
        pthread_mutex_unlock(&g_table_lock);
        return 0;
    }
    pthread_mutex_lock(&g_list_locks[index]);
    pthread_mutex_unlock(&g_table_lock);
    *index_out = index;
    return 1;
}

/* Caller holds g_drag_lock. */
static void clear_drag_locked(void)
{
    g_drag_handle = 0;
    g_drag_index = 0;
    g_drag_lock_window = 0;
    g_drag_position_packed = 0;
    g_drag_active = 0;
    g_drag_visible = 0;
}

static int32_t saturating_positive(uint32_t value)
{
    const int32_t signed_value = (int32_t)value;
    return signed_value > 0 ? signed_value : 0;
}

static void h_create(CPU *c)
{
    const int32_t width = (int32_t)wc3_legacy_arg32(c, 0u);
    const int32_t height = (int32_t)wc3_legacy_arg32(c, 1u);
    const uint32_t flags = wc3_legacy_arg32(c, 2u);
    const int32_t initial = saturating_positive(wc3_legacy_arg32(c, 3u));
    const int32_t grow = saturating_positive(wc3_legacy_arg32(c, 4u));
    unsigned index = IMAGE_LIST_TABLE_SIZE;
    c->eax = 0;

    if (width <= 0 || height <= 0) return;

    /* Publish identity, then pin the list while the table is held. */
    pthread_mutex_lock(&g_table_lock);
    index = find_free_index_locked();
    if (index < IMAGE_LIST_TABLE_SIZE) {
        g_lists[index].used = 1;
        pthread_mutex_lock(&g_list_locks[index]);
    }
    pthread_mutex_unlock(&g_table_lock);

    if (index < IMAGE_LIST_TABLE_SIZE) {
        /* Payload is list-owned; initialise it under the list lock. */
        g_lists[index].width = width;
        g_lists[index].height = height;
        g_lists[index].flags = flags;
        g_lists[index].count = 0;
        g_lists[index].capacity = initial;
        g_lists[index].grow = grow > 0 ? grow : 1;
        pthread_mutex_unlock(&g_list_locks[index]);
        c->eax = handle_for_index(index);
    }
}

static void h_destroy(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    unsigned index = 0;
    int found = 0;
    c->eax = 0;

    pthread_mutex_lock(&g_table_lock);
    found = index_for_handle_locked(handle, &index);
    if (found) {
        pthread_mutex_lock(&g_list_locks[index]);
        /* Identity is table-owned; clear it while both locks are held. */
        g_lists[index].used = 0;
    }
    pthread_mutex_unlock(&g_table_lock);

    if (found) {
        /* Payload is list-owned; the table lock is already released. */
        g_lists[index].width = 0;
        g_lists[index].height = 0;
        g_lists[index].flags = 0;
        g_lists[index].count = 0;
        g_lists[index].capacity = 0;
        g_lists[index].grow = 0;
        /* Keep the list through the drag clear so a creator cannot initialise
         * a reused slot before the old drag session is cleared. */
        pthread_mutex_lock(&g_drag_lock);
        if (g_drag_handle == handle) clear_drag_locked();
        pthread_mutex_unlock(&g_drag_lock);
        pthread_mutex_unlock(&g_list_locks[index]);
        c->eax = 1;
    }
}

static void h_get_image_count(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    unsigned index = 0;
    c->eax = 0;
    if (!pin_handle(handle, &index)) return;
    c->eax = (uint32_t)g_lists[index].count;
    pthread_mutex_unlock(&g_list_locks[index]);
}

static void h_add(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    unsigned index = 0;
    c->eax = 0xFFFFFFFFu; /* -1 */

    if (!pin_handle(handle, &index)) return;
    TargetImageList *list = &g_lists[index];
    if (list->count < INT32_MAX) {
        if (list->count >= list->capacity) {
            const int32_t grow = list->grow > 0 ? list->grow : 1;
            if (list->capacity <= INT32_MAX - grow)
                list->capacity += grow;
            else
                list->capacity = INT32_MAX;
        }
        c->eax = (uint32_t)list->count;
        list->count++;
    }
    pthread_mutex_unlock(&g_list_locks[index]);
}

static void h_replace(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const int32_t index = (int32_t)wc3_legacy_arg32(c, 1u);
    unsigned list_index = 0;
    c->eax = 0;

    if (!pin_handle(handle, &list_index)) return;
    if (index >= 0 && index < g_lists[list_index].count) c->eax = 1;
    pthread_mutex_unlock(&g_list_locks[list_index]);
}

static void h_begin_drag(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const int32_t index = (int32_t)wc3_legacy_arg32(c, 1u);
    unsigned list_index = 0;
    c->eax = 0;

    if (!pin_handle(handle, &list_index)) return;
    if (index >= 0 && index < g_lists[list_index].count) {
        /* The slot stays pinned until the drag session is published, so a
         * concurrent destroy cannot invalidate the handle in between. */
        pthread_mutex_lock(&g_drag_lock);
        g_drag_handle = handle;
        g_drag_index = index;
        g_drag_lock_window = 0;
        g_drag_position_packed = 0;
        g_drag_active = 1;
        g_drag_visible = 0;
        c->eax = 1;
        pthread_mutex_unlock(&g_drag_lock);
    }
    pthread_mutex_unlock(&g_list_locks[list_index]);
}

static void h_end_drag(CPU *c)
{
    pthread_mutex_lock(&g_drag_lock);
    clear_drag_locked();
    pthread_mutex_unlock(&g_drag_lock);
    c->eax = 0; /* ImageList_EndDrag is a void function. */
}

static void h_drag_enter(CPU *c)
{
    const uint32_t window = wc3_legacy_arg32(c, 0u);
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 1u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 2u);
    c->eax = 0;

    pthread_mutex_lock(&g_drag_lock);
    if (g_drag_active) {
        g_drag_lock_window = window;
        g_drag_position_packed = (uint32_t)x ^ ((uint32_t)y << 16);
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_drag_lock);
}

static void h_drag_leave(CPU *c)
{
    const uint32_t window = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_drag_lock);
    if (g_drag_active && (!window || window == g_drag_lock_window)) {
        g_drag_lock_window = 0;
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_drag_lock);
}

static void h_drag_move(CPU *c)
{
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 0u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    pthread_mutex_lock(&g_drag_lock);
    if (g_drag_active) {
        g_drag_position_packed = (uint32_t)x ^ ((uint32_t)y << 16);
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_drag_lock);
}

static void h_drag_show_nolock(CPU *c)
{
    const uint32_t show = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_drag_lock);
    if (g_drag_active) {
        g_drag_visible = show != 0u;
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_drag_lock);
}

typedef struct ImageListAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} ImageListAddition;

static const ImageListAddition g_additions[] = {
    { "ImageList_Create", 5u, h_create },
    { "ImageList_Destroy", 1u, h_destroy },
    { "ImageList_GetImageCount", 1u, h_get_image_count },
    { "ImageList_Add", 3u, h_add },
    { "ImageList_Replace", 4u, h_replace },
    { "ImageList_BeginDrag", 4u, h_begin_drag },
    { "ImageList_EndDrag", 0u, h_end_drag },
    { "ImageList_DragEnter", 3u, h_drag_enter },
    { "ImageList_DragLeave", 1u, h_drag_leave },
    { "ImageList_DragMove", 2u, h_drag_move },
    { "ImageList_DragShowNolock", 1u, h_drag_show_nolock },
};

int wc3_hle_comctl32_imagelist_install(void)
{
    ensure_list_locks();

    /* Bootstrap reset takes every level in the documented order so it cannot
     * intersect a pinned slot or a drag update. */
    pthread_mutex_lock(&g_table_lock);
    for (unsigned i = 0; i < IMAGE_LIST_TABLE_SIZE; i++)
        pthread_mutex_lock(&g_list_locks[i]);
    pthread_mutex_lock(&g_drag_lock);
    memset(g_lists, 0, sizeof g_lists);
    clear_drag_locked();
    pthread_mutex_unlock(&g_drag_lock);
    for (unsigned i = 0; i < IMAGE_LIST_TABLE_SIZE; i++)
        pthread_mutex_unlock(&g_list_locks[i]);
    pthread_mutex_unlock(&g_table_lock);

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("COMCTL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
