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
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_drag_handle;
static int32_t g_drag_index;
static uint32_t g_drag_lock_window;
static uint32_t g_drag_position_packed;
static int g_drag_active;
static int g_drag_visible;

static uint32_t handle_for_index(unsigned index)
{
    return IMAGE_LIST_HANDLE_BASE + index * IMAGE_LIST_HANDLE_STRIDE;
}

static int index_for_handle(uint32_t handle, unsigned *index_out)
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
    c->eax = 0;

    if (width <= 0 || height <= 0) return;

    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < IMAGE_LIST_TABLE_SIZE; i++) {
        if (g_lists[i].used) continue;
        memset(&g_lists[i], 0, sizeof g_lists[i]);
        g_lists[i].used = 1;
        g_lists[i].width = width;
        g_lists[i].height = height;
        g_lists[i].flags = flags;
        g_lists[i].count = 0;
        g_lists[i].capacity = initial;
        g_lists[i].grow = grow > 0 ? grow : 1;
        c->eax = handle_for_index(i);
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_destroy(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    unsigned index = 0;
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (index_for_handle(handle, &index)) {
        memset(&g_lists[index], 0, sizeof g_lists[index]);
        if (g_drag_handle == handle) {
            g_drag_handle = 0;
            g_drag_index = 0;
            g_drag_lock_window = 0;
            g_drag_position_packed = 0;
            g_drag_active = 0;
            g_drag_visible = 0;
        }
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_image_count(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    c->eax = 0;
    pthread_mutex_lock(&g_lock);
    unsigned index = 0;
    if (index_for_handle(handle, &index))
        c->eax = (uint32_t)g_lists[index].count;
    pthread_mutex_unlock(&g_lock);
}

static void h_add(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    c->eax = 0xFFFFFFFFu; /* -1 */

    pthread_mutex_lock(&g_lock);
    unsigned index = 0;
    if (index_for_handle(handle, &index)) {
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
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_replace(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const int32_t index = (int32_t)wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    unsigned list_index = 0;
    if (index_for_handle(handle, &list_index) && index >= 0 &&
        index < g_lists[list_index].count) {
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_begin_drag(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const int32_t index = (int32_t)wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    unsigned list_index = 0;
    if (index_for_handle(handle, &list_index) && index >= 0 &&
        index < g_lists[list_index].count) {
        g_drag_handle = handle;
        g_drag_index = index;
        g_drag_lock_window = 0;
        g_drag_position_packed = 0;
        g_drag_active = 1;
        g_drag_visible = 0;
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_end_drag(CPU *c)
{
    pthread_mutex_lock(&g_lock);
    g_drag_handle = 0;
    g_drag_index = 0;
    g_drag_lock_window = 0;
    g_drag_position_packed = 0;
    g_drag_active = 0;
    g_drag_visible = 0;
    pthread_mutex_unlock(&g_lock);
    c->eax = 0; /* ImageList_EndDrag is a void function. */
}

static void h_drag_enter(CPU *c)
{
    const uint32_t window = wc3_legacy_arg32(c, 0u);
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 1u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 2u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (g_drag_active) {
        g_drag_lock_window = window;
        g_drag_position_packed = (uint32_t)x ^ ((uint32_t)y << 16);
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_drag_leave(CPU *c)
{
    const uint32_t window = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (g_drag_active && (!window || window == g_drag_lock_window)) {
        g_drag_lock_window = 0;
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_drag_move(CPU *c)
{
    const int32_t x = (int32_t)wc3_legacy_arg32(c, 0u);
    const int32_t y = (int32_t)wc3_legacy_arg32(c, 1u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (g_drag_active) {
        g_drag_position_packed = (uint32_t)x ^ ((uint32_t)y << 16);
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_drag_show_nolock(CPU *c)
{
    const uint32_t show = wc3_legacy_arg32(c, 0u);
    c->eax = 0;

    pthread_mutex_lock(&g_lock);
    if (g_drag_active) {
        g_drag_visible = show != 0u;
        c->eax = 1;
    }
    pthread_mutex_unlock(&g_lock);
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
    memset(g_lists, 0, sizeof g_lists);
    g_drag_handle = 0;
    g_drag_index = 0;
    g_drag_lock_window = 0;
    g_drag_position_packed = 0;
    g_drag_active = 0;
    g_drag_visible = 0;

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("COMCTL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
