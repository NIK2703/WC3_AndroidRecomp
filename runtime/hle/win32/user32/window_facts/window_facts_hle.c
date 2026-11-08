/*
 * window_facts_hle.c - target-side USER32 window-creation facts.
 *
 * Window creation and the input queue are still owned by the quarantine.  A
 * thin chain override calls the as-built handler first and records the handle,
 * class, parent and placement it produced.  The query imports then answer from
 * that table.  No host lock is held while the as-built handler or guest memory
 * is touched.
 *
 * Class facts and window facts use separate table locks.  Each window slot also
 * has its own mutex: the table lock only establishes slot identity (used,
 * handle, parent, serial, menu and the fields written once by record_window),
 * while the slot mutex protects the mutable placement, show command and title.
 * A lookup pins a slot by taking its mutex before dropping the table lock, so
 * DestroyWindow cannot clear or reuse the slot while the caller still owns the
 * fact pointer.  Lock order is g_class_lock -> g_window_lock -> slot mutex;
 * no operation takes a table lock while holding a slot mutex.
 */
#include "window_facts_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "platform.h"

#define DESKTOP_WINDOW_HANDLE 0x00010000u
#define MONITOR_HANDLE 0x00190000u
#define WINDOW_MAX 32u
#define WINDOW_CLASS_MAX 16u
/* Edit controls are the longest strings the window-facts table stores.  The
 * game reads them with GetDlgItemTextA(..., 0xFF), so keep the model at the
 * same 255-character limit instead of truncating below the caller's buffer. */
#define WINDOW_TEXT_MAX 256u
#define WINDOW_PLACEMENT_SIZE 44u
#define WNDCLASSA_SIZE 40u
#define WNDCLASSEXA_SIZE 48u
#define MONITORINFO_SIZE 40u
#define MONITORINFOEXA_SIZE 72u
#define MONITORINFOF_PRIMARY 1u
#define MONITOR_DEVICE_NAME "\\\\.\\DISPLAY1"

#define MONITOR_DEFAULTTONULL 0u
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u

#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CONTROL_ID_NOT_FOUND 1421u
/* Win32 assigns 1410 to ERROR_CLASS_ALREADY_EXISTS and 1411 to
 * ERROR_CLASS_DOES_NOT_EXIST; GetClassInfoA reports the latter. */
#define ERROR_CLASS_DOES_NOT_EXIST 1411u
#define ERROR_INVALID_MONITOR_HANDLE 1461u

#define WPF_SETMINPOSITION 0x0001u

#define SW_HIDE 0u
#define SW_SHOWNORMAL 1u
#define SW_SHOWMINIMIZED 2u
#define SW_SHOWMAXIMIZED 3u
#define SW_SHOW 5u
#define SW_MINIMIZE 6u
#define SW_SHOWMINNOACTIVE 7u
#define SW_RESTORE 9u
#define SW_SHOWDEFAULT 10u

#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u

typedef struct WindowClassFact {
    uint8_t used;
    uint32_t atom;
    uint32_t style;
    uint32_t wnd_proc;
    int32_t cls_extra;
    int32_t wnd_extra;
    uint32_t instance;
    uint32_t icon;
    uint32_t cursor;
    uint32_t background;
    uint32_t menu_name;
    uint32_t class_name_ptr;
    uint32_t icon_sm;
    char name[WINDOW_TEXT_MAX];
} WindowClassFact;

typedef struct BuiltinClassFact {
    const char *name;
    uint32_t style;
    int32_t cls_extra;
    int32_t wnd_extra;
} BuiltinClassFact;

typedef struct WindowFact {
    uint8_t used;
    uint32_t handle;
    uint32_t parent;
    uint32_t serial;
    uint32_t show_cmd;
    uint32_t thread_id;
    uint32_t process_id;
    uint32_t style;
    uint32_t ex_style;
    uint32_t menu;
    uint32_t placement_flags;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int32_t min_x;
    int32_t min_y;
    int32_t max_x;
    int32_t max_y;
    char class_name[WINDOW_TEXT_MAX];
    char title[WINDOW_TEXT_MAX];
} WindowFact;

static WindowClassFact g_classes[WINDOW_CLASS_MAX];
static WindowFact g_windows[WINDOW_MAX];
static uint32_t g_next_serial = 1u;

/* g_class_lock owns g_classes.  g_window_lock owns the window table structure:
 * used/handle/parent/serial/menu/class_name/style/ex_style/thread and process
 * ids, all written at runtime only by record_window() and remove_window(),
 * plus g_next_serial.  g_window_locks[i] owns window i's mutable placement,
 * show command and title.  The only nesting is class -> window -> slot, and
 * slot locks are never held while taking a table lock. */
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_window_locks[WINDOW_MAX];
static pthread_once_t g_window_locks_once = PTHREAD_ONCE_INIT;

static const Wc3ThreadApi *g_thread;
static uint32_t g_process_id;

static void (*g_previous_register_class)(CPU *c);
static void (*g_previous_register_class_a)(CPU *c);
static void (*g_previous_create_window)(CPU *c);
static void (*g_previous_destroy_window)(CPU *c);
static void (*g_previous_show_window)(CPU *c);
static void (*g_previous_set_window_pos)(CPU *c);
static void (*g_previous_set_window_text)(CPU *c);

/* USER32's process-wide system classes.  GetClassInfoA answers these even
 * when the caller passes an hInstance; Windows ignores the instance filter
 * for system classes.  The guest consumes the style and the extra-byte
 * counts, so those are modeled.  The system wndproc, cursor and background
 * are host-owned objects with no guest handle, so they stay zero. */
static const BuiltinClassFact g_builtin_classes[] = {
    { "Static",    0x00000088u, 0,  8 },
    { "Button",    0x0000008Bu, 0,  8 },
    { "Edit",      0x00000088u, 0,  8 },
    { "ComboBox",  0x0000008Bu, 0,  8 },
    { "ListBox",   0x00000088u, 0,  8 },
    { "ScrollBar", 0x0000008Bu, 0, 80 },
    { "#32769",    0x00000008u, 0,  0 },
    { "MDICLIENT", 0x00000000u, 0,  0 },
};

static int read_guest_string(uint32_t address, char *buffer, size_t capacity)
{
    if (!address || !buffer || capacity < 2u) return 0;
    buffer[0] = '\0';
    for (size_t i = 0; i + 1u < capacity; i++) {
        if (address > UINT32_MAX - (uint32_t)i) {
            buffer[i] = '\0';
            return 0;
        }
        const uint32_t at = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(at, 1u)) {
            buffer[i] = '\0';
            return 0;
        }
        const char ch = (char)wc3_legacy_rd8g(at);
        buffer[i] = ch;
        if (!ch) return 1;
    }
    buffer[capacity - 1u] = '\0';
    return 0;
}

static void copy_text(char *dst, size_t capacity, const char *source)
{
    if (!dst || capacity == 0u) return;
    if (!source) source = "";
    size_t length = strlen(source);
    if (length >= capacity) length = capacity - 1u;
    memcpy(dst, source, length);
    dst[length] = '\0';
}

static int text_equal_ci(const char *left, const char *right)
{
    if (!left || !right) return 0;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *left == *right;
}

static uint32_t current_thread_id(void)
{
    if (!g_thread || !g_thread->self || !g_thread->id) return 0u;
    const Wc3ThreadHandle self = g_thread->self(g_thread->ctx);
    return (uint32_t)g_thread->id(g_thread->ctx, self);
}

static void init_window_locks(void)
{
    for (unsigned i = 0; i < WINDOW_MAX; i++)
        pthread_mutex_init(&g_window_locks[i], NULL);
}

static void ensure_window_locks(void)
{
    (void)pthread_once(&g_window_locks_once, init_window_locks);
}

/* Caller holds g_window_lock.  Slot identity changes only under that lock, so
 * a resolved index can be pinned with g_window_locks[index] while the table
 * lock is still held. */
static unsigned find_window_index_locked(uint32_t handle)
{
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        if (g_windows[i].used && g_windows[i].handle == handle)
            return i;
    }
    return WINDOW_MAX;
}

/* Resolve a handle to a pinned slot.  The table lock is dropped before the
 * caller sees the pointer; the slot mutex stays held, so remove_window() and
 * record_window() cannot clear or reuse the slot underneath the caller. */
static WindowFact *lock_window(uint32_t handle)
{
    pthread_mutex_lock(&g_window_lock);
    const unsigned index = find_window_index_locked(handle);
    if (index >= WINDOW_MAX) {
        pthread_mutex_unlock(&g_window_lock);
        return NULL;
    }
    pthread_mutex_lock(&g_window_locks[index]);
    pthread_mutex_unlock(&g_window_lock);
    return &g_windows[index];
}

static void unlock_window(WindowFact *window)
{
    pthread_mutex_unlock(&g_window_locks[(unsigned)(window - g_windows)]);
}

/* Caller holds g_window_lock.  Dialog item IDs are only unique among the
 * children of one parent; the newest child wins when a dialog reuses a control
 * ID.  Only table-owned fields are read, so no slot lock is needed. */
static unsigned find_dlg_item_index_locked(uint32_t parent, uint32_t id)
{
    unsigned best = WINDOW_MAX;
    uint32_t best_serial = 0u;
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        const WindowFact *window = &g_windows[i];
        if (!window->used) continue;
        if (window->parent != parent) continue;
        if (window->menu != id) continue;
        if (best >= WINDOW_MAX || window->serial > best_serial) {
            best = i;
            best_serial = window->serial;
        }
    }
    return best;
}

static WindowClassFact *find_class_atom(uint32_t atom)
{
    for (unsigned i = 0; i < WINDOW_CLASS_MAX; i++) {
        if (g_classes[i].used && g_classes[i].atom == atom)
            return &g_classes[i];
    }
    return NULL;
}

static WindowClassFact *find_class_name(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < WINDOW_CLASS_MAX; i++) {
        if (g_classes[i].used && text_equal_ci(g_classes[i].name, name))
            return &g_classes[i];
    }
    return NULL;
}

static const BuiltinClassFact *find_builtin_class(const char *name)
{
    if (!name) return NULL;
    const unsigned count = (unsigned)(sizeof g_builtin_classes /
                                      sizeof g_builtin_classes[0]);
    for (unsigned i = 0; i < count; i++) {
        if (text_equal_ci(g_builtin_classes[i].name, name))
            return &g_builtin_classes[i];
    }
    return NULL;
}

static void record_class(uint32_t atom, const char *name, uint32_t description,
                         int is_ex)
{
    if (!atom || !name || !name[0]) return;
    const uint32_t size = is_ex ? WNDCLASSEXA_SIZE : WNDCLASSA_SIZE;
    const uint32_t base = is_ex ? 4u : 0u;
    const int have_description =
        description && wc3_legacy_guest_ok(description, size);

    WindowClassFact facts;
    memset(&facts, 0, sizeof facts);
    if (have_description) {
        facts.style = wc3_legacy_rd32g(description + base + 0u);
        facts.wnd_proc = wc3_legacy_rd32g(description + base + 4u);
        facts.cls_extra =
            (int32_t)wc3_legacy_rd32g(description + base + 8u);
        facts.wnd_extra =
            (int32_t)wc3_legacy_rd32g(description + base + 12u);
        facts.instance = wc3_legacy_rd32g(description + base + 16u);
        facts.icon = wc3_legacy_rd32g(description + base + 20u);
        facts.cursor = wc3_legacy_rd32g(description + base + 24u);
        facts.background = wc3_legacy_rd32g(description + base + 28u);
        facts.menu_name = wc3_legacy_rd32g(description + base + 32u);
        facts.class_name_ptr = wc3_legacy_rd32g(description + base + 36u);
        facts.icon_sm = is_ex ? wc3_legacy_rd32g(description + 44u) : 0u;
    }

    pthread_mutex_lock(&g_class_lock);
    WindowClassFact *existing = find_class_name(name);
    if (!existing) {
        for (unsigned i = 0; i < WINDOW_CLASS_MAX; i++) {
            if (g_classes[i].used) continue;
            existing = &g_classes[i];
            break;
        }
    }
    if (existing) {
        memset(existing, 0, sizeof *existing);
        existing->used = 1u;
        existing->atom = atom;
        existing->style = facts.style;
        existing->wnd_proc = facts.wnd_proc;
        existing->cls_extra = facts.cls_extra;
        existing->wnd_extra = facts.wnd_extra;
        existing->instance = facts.instance;
        existing->icon = facts.icon;
        existing->cursor = facts.cursor;
        existing->background = facts.background;
        existing->menu_name = facts.menu_name;
        existing->class_name_ptr = facts.class_name_ptr;
        existing->icon_sm = facts.icon_sm;
        copy_text(existing->name, sizeof existing->name, name);
    }
    pthread_mutex_unlock(&g_class_lock);
}

static void record_window(uint32_t handle, uint32_t parent,
                          const char *class_name, const char *title,
                          uint32_t style, uint32_t ex_style, uint32_t menu,
                          int32_t x, int32_t y, uint32_t width, uint32_t height,
                          uint32_t thread_id, uint32_t process_id)
{
    if (!handle) return;
    pthread_mutex_lock(&g_window_lock);
    unsigned index = find_window_index_locked(handle);
    if (index >= WINDOW_MAX) {
        for (unsigned i = 0; i < WINDOW_MAX; i++) {
            if (g_windows[i].used) continue;
            index = i;
            break;
        }
    }
    if (index < WINDOW_MAX) {
        WindowFact *window = &g_windows[index];
        pthread_mutex_lock(&g_window_locks[index]);
        memset(window, 0, sizeof *window);
        window->used = 1u;
        window->handle = handle;
        window->parent = parent;
        window->serial = g_next_serial++;
        window->show_cmd = SW_SHOWNORMAL;
        window->thread_id = thread_id;
        window->process_id = process_id;
        window->style = style;
        window->ex_style = ex_style;
        window->menu = menu;
        window->x = x;
        window->y = y;
        window->width = width;
        window->height = height;
        window->min_x = -1;
        window->min_y = -1;
        window->max_x = -1;
        window->max_y = -1;
        copy_text(window->class_name, sizeof window->class_name, class_name);
        copy_text(window->title, sizeof window->title, title);
        pthread_mutex_unlock(&g_window_locks[index]);
    }
    pthread_mutex_unlock(&g_window_lock);
}

static void remove_window(uint32_t handle)
{
    pthread_mutex_lock(&g_window_lock);
    const unsigned index = find_window_index_locked(handle);
    if (index < WINDOW_MAX) {
        pthread_mutex_lock(&g_window_locks[index]);
        memset(&g_windows[index], 0, sizeof g_windows[index]);
        pthread_mutex_unlock(&g_window_locks[index]);
    }
    pthread_mutex_unlock(&g_window_lock);
}

static uint32_t normalize_show_cmd(uint32_t command)
{
    switch (command) {
    case SW_HIDE:
        return 0u;
    case SW_SHOWMINIMIZED:
    case SW_MINIMIZE:
    case SW_SHOWMINNOACTIVE:
        return SW_SHOWMINIMIZED;
    case SW_SHOWMAXIMIZED:
        return SW_SHOWMAXIMIZED;
    default:
        return SW_SHOWNORMAL;
    }
}

static int is_top_parent(uint32_t parent)
{
    return parent == 0u || parent == DESKTOP_WINDOW_HANDLE;
}

static int monitor_size(uint32_t *width, uint32_t *height)
{
    if (!width || !height) return 0;
    if (wc3_legacy_current_display_mode(width, height)) return 1;
    return wc3_legacy_default_display_mode(width, height);
}

static int window_intersects_monitor(const WindowFact *window,
                                     uint32_t monitor_width,
                                     uint32_t monitor_height)
{
    const int64_t left = window->x;
    const int64_t top = window->y;
    const int64_t right = left + (int64_t)window->width;
    const int64_t bottom = top + (int64_t)window->height;
    return right > 0 && bottom > 0 &&
           left < (int64_t)monitor_width &&
           top < (int64_t)monitor_height;
}

static void h_register_class_common(CPU *c, int is_ex)
{
    const uint32_t description = wc3_legacy_arg32(c, 0u);
    const uint32_t size = is_ex ? WNDCLASSEXA_SIZE : WNDCLASSA_SIZE;
    const uint32_t name_offset = is_ex ? 40u : 36u;
    char name[WINDOW_TEXT_MAX];
    name[0] = '\0';
    int have_name = 0;
    if (description && wc3_legacy_guest_ok(description, size)) {
        have_name = read_guest_string(
            wc3_legacy_rd32g(description + name_offset), name, sizeof name);
    }

    if (is_ex) {
        if (g_previous_register_class)
            g_previous_register_class(c);
    } else {
        if (g_previous_register_class_a)
            g_previous_register_class_a(c);
    }

    if (c->eax && have_name)
        record_class(c->eax, name, description, is_ex);
}

static void h_register_class_ex(CPU *c)
{
    h_register_class_common(c, 1);
}

static void h_register_class_a(CPU *c)
{
    h_register_class_common(c, 0);
}

static void h_create_window_ex(CPU *c)
{
    const uint32_t ex_style = wc3_legacy_arg32(c, 0u);
    const uint32_t class_arg = wc3_legacy_arg32(c, 1u);
    const uint32_t title_ptr = wc3_legacy_arg32(c, 2u);
    const uint32_t style = wc3_legacy_arg32(c, 3u);
    const uint32_t parent = wc3_legacy_arg32(c, 8u);
    const uint32_t menu = wc3_legacy_arg32(c, 9u);
    const uint32_t raw_x = wc3_legacy_arg32(c, 4u);
    const uint32_t raw_y = wc3_legacy_arg32(c, 5u);
    const uint32_t raw_width = wc3_legacy_arg32(c, 6u);
    const uint32_t raw_height = wc3_legacy_arg32(c, 7u);
    char class_name[WINDOW_TEXT_MAX];
    char title[WINDOW_TEXT_MAX];
    class_name[0] = '\0';
    title[0] = '\0';

    if (class_arg && class_arg < 0x10000u) {
        pthread_mutex_lock(&g_class_lock);
        WindowClassFact *fact = find_class_atom(class_arg);
        if (fact) copy_text(class_name, sizeof class_name, fact->name);
        pthread_mutex_unlock(&g_class_lock);
    } else {
        read_guest_string(class_arg, class_name, sizeof class_name);
    }
    if (title_ptr)
        read_guest_string(title_ptr, title, sizeof title);

    if (g_previous_create_window)
        g_previous_create_window(c);

    const uint32_t handle = c->eax;
    if (!handle) return;

    int32_t x = (int32_t)raw_x;
    int32_t y = (int32_t)raw_y;
    uint32_t width = raw_width;
    uint32_t height = raw_height;
    if (raw_x == 0x80000000u) x = 0;
    if (raw_y == 0x80000000u) y = 0;
    if (raw_width == 0u || raw_width == 0x80000000u) width = 800u;
    if (raw_height == 0u || raw_height == 0x80000000u) height = 600u;

    record_window(handle, parent, class_name, title, style, ex_style, menu,
                  x, y, width, height, current_thread_id(), g_process_id);
}

static void h_destroy_window(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    if (g_previous_destroy_window)
        g_previous_destroy_window(c);
    if (c->eax)
        remove_window(handle);
}

static void h_show_window(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t command = wc3_legacy_arg32(c, 1u);
    if (g_previous_show_window)
        g_previous_show_window(c);

    WindowFact *window = lock_window(handle);
    if (window) {
        window->show_cmd = normalize_show_cmd(command);
        unlock_window(window);
    }
}

static void h_set_window_pos(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t raw_x = wc3_legacy_arg32(c, 2u);
    const uint32_t raw_y = wc3_legacy_arg32(c, 3u);
    const uint32_t raw_width = wc3_legacy_arg32(c, 4u);
    const uint32_t raw_height = wc3_legacy_arg32(c, 5u);
    const uint32_t flags = wc3_legacy_arg32(c, 6u);
    if (g_previous_set_window_pos)
        g_previous_set_window_pos(c);

    WindowFact *window = lock_window(handle);
    if (window) {
        if (!(flags & SWP_NOMOVE)) {
            window->x = (int32_t)raw_x;
            window->y = (int32_t)raw_y;
        }
        if (!(flags & SWP_NOSIZE) && (int32_t)raw_width > 0 &&
            (int32_t)raw_height > 0) {
            window->width = raw_width;
            window->height = raw_height;
        }
        unlock_window(window);
    }
}

static void h_set_window_text(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t text = wc3_legacy_arg32(c, 1u);
    char title[WINDOW_TEXT_MAX];
    title[0] = '\0';
    if (text)
        (void)read_guest_string(text, title, sizeof title);

    if (g_previous_set_window_text)
        g_previous_set_window_text(c);
    if (!c->eax) return;

    WindowFact *window = lock_window(handle);
    if (window) {
        copy_text(window->title, sizeof window->title, title);
        unlock_window(window);
    }
}

static void h_get_top_window(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    uint32_t best_handle = 0u;
    uint32_t best_serial = 0u;

    pthread_mutex_lock(&g_window_lock);
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        const WindowFact *window = &g_windows[i];
        if (!window->used) continue;
        const int matches = is_top_parent(parent)
            ? is_top_parent(window->parent)
            : window->parent == parent;
        if (!matches) continue;
        if (window->serial >= best_serial) {
            best_serial = window->serial;
            best_handle = window->handle;
        }
    }
    pthread_mutex_unlock(&g_window_lock);
    c->eax = best_handle;
}

static void h_get_class_name(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t buffer = wc3_legacy_arg32(c, 1u);
    const uint32_t capacity = wc3_legacy_arg32(c, 2u);
    char name[WINDOW_TEXT_MAX];
    name[0] = '\0';

    if (handle == DESKTOP_WINDOW_HANDLE) {
        copy_text(name, sizeof name, "#32769");
    } else {
        /* class_name is written only by record_window() under the table
         * lock, so the table lock alone is enough to copy it. */
        pthread_mutex_lock(&g_window_lock);
        const unsigned index = find_window_index_locked(handle);
        if (index < WINDOW_MAX)
            copy_text(name, sizeof name, g_windows[index].class_name);
        pthread_mutex_unlock(&g_window_lock);
    }

    if (!name[0] || !buffer || capacity == 0u ||
        !wc3_legacy_guest_ok(buffer, capacity)) {
        c->eax = 0u;
        return;
    }

    const size_t length = strlen(name);
    const size_t copied = length < capacity - 1u ? length : capacity - 1u;
    for (size_t i = 0; i < copied; i++)
        wc3_legacy_wr8g(buffer + (uint32_t)i, (uint8_t)name[i]);
    wc3_legacy_wr8g(buffer + (uint32_t)copied, 0u);
    c->eax = (uint32_t)copied;
}

static void h_is_window(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    if (handle == DESKTOP_WINDOW_HANDLE) {
        c->eax = 1u;
        return;
    }
    pthread_mutex_lock(&g_window_lock);
    c->eax = find_window_index_locked(handle) < WINDOW_MAX ? 1u : 0u;
    pthread_mutex_unlock(&g_window_lock);
}

static void h_get_window_thread_process_id(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t process_id = wc3_legacy_arg32(c, 1u);

    if (process_id && !wc3_legacy_guest_ok(process_id, 4u)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    if (handle == DESKTOP_WINDOW_HANDLE) {
        if (process_id)
            wc3_legacy_wr32g(process_id, g_process_id);
        c->eax = current_thread_id();
        return;
    }

    uint32_t thread_id = 0u;
    uint32_t owner_process = 0u;
    int found = 0;
    /* The owner ids are written once by record_window() under the table
     * lock, so no slot lock is needed to read them. */
    pthread_mutex_lock(&g_window_lock);
    const unsigned index = find_window_index_locked(handle);
    if (index < WINDOW_MAX) {
        found = 1;
        thread_id = g_windows[index].thread_id;
        owner_process = g_windows[index].process_id;
    }
    pthread_mutex_unlock(&g_window_lock);

    if (!found) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }
    if (process_id)
        wc3_legacy_wr32g(process_id, owner_process);
    c->eax = thread_id;
}

static void h_find_window_ex(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t child_after = wc3_legacy_arg32(c, 1u);
    const uint32_t class_arg = wc3_legacy_arg32(c, 2u);
    const uint32_t title_arg = wc3_legacy_arg32(c, 3u);
    char class_name[WINDOW_TEXT_MAX];
    char title[WINDOW_TEXT_MAX];
    int class_filter = 0;
    int title_filter = 0;
    int class_locked = 0;
    class_name[0] = '\0';
    title[0] = '\0';

    /* Both filters are guest strings and reading them has no side effects, so
     * read both before taking the class lock for an atom lookup. */
    if (title_arg) {
        if (!read_guest_string(title_arg, title, sizeof title)) {
            c->eax = 0u;
            return;
        }
        title_filter = 1;
    }
    const int class_from_guest = class_arg >= 0x10000u;
    if (class_from_guest &&
        !read_guest_string(class_arg, class_name, sizeof class_name)) {
        c->eax = 0u;
        return;
    }
    if (class_arg) {
        if (!class_from_guest) {
            pthread_mutex_lock(&g_class_lock);
            class_locked = 1;
            const WindowClassFact *fact = find_class_atom(class_arg);
            if (fact) copy_text(class_name, sizeof class_name, fact->name);
            if (!class_name[0]) {
                pthread_mutex_unlock(&g_class_lock);
                c->eax = 0u;
                return;
            }
        }
        class_filter = 1;
    }

    uint32_t best_handle = 0u;
    uint32_t best_serial = 0u;
    /* The scan keeps the table lock so the parent/child_after validation and
     * the visible used/handle/serial set stay one snapshot.  Only the mutable
     * title needs the slot lock; class_name is table-owned.  An atom filter
     * keeps g_class_lock across the scan so class removal cannot split the
     * lookup from the scan, matching the former single-lock behaviour. */
    pthread_mutex_lock(&g_window_lock);

    if (parent != 0u && parent != DESKTOP_WINDOW_HANDLE &&
        find_window_index_locked(parent) >= WINDOW_MAX) {
        pthread_mutex_unlock(&g_window_lock);
        if (class_locked) pthread_mutex_unlock(&g_class_lock);
        c->eax = 0u;
        return;
    }
    const unsigned after_index = child_after
        ? find_window_index_locked(child_after)
        : WINDOW_MAX;
    if (child_after && after_index >= WINDOW_MAX) {
        pthread_mutex_unlock(&g_window_lock);
        if (class_locked) pthread_mutex_unlock(&g_class_lock);
        c->eax = 0u;
        return;
    }
    if (after_index < WINDOW_MAX) {
        const int top_scope = is_top_parent(parent);
        if ((top_scope && !is_top_parent(g_windows[after_index].parent)) ||
            (!top_scope && g_windows[after_index].parent != parent)) {
            pthread_mutex_unlock(&g_window_lock);
            if (class_locked) pthread_mutex_unlock(&g_class_lock);
            c->eax = 0u;
            return;
        }
    }
    const uint32_t after_serial = after_index < WINDOW_MAX
        ? g_windows[after_index].serial
        : UINT32_MAX;

    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        const WindowFact *window = &g_windows[i];
        if (!window->used) continue;
        const int parent_match = is_top_parent(parent)
            ? is_top_parent(window->parent)
            : window->parent == parent;
        if (!parent_match) continue;
        if (window->serial >= after_serial) continue;
        if (class_filter && !text_equal_ci(window->class_name, class_name))
            continue;
        if (title_filter) {
            int title_match;
            pthread_mutex_lock(&g_window_locks[i]);
            title_match = text_equal_ci(window->title, title);
            pthread_mutex_unlock(&g_window_locks[i]);
            if (!title_match) continue;
        }
        if (window->serial > best_serial) {
            best_serial = window->serial;
            best_handle = window->handle;
        }
    }
    pthread_mutex_unlock(&g_window_lock);
    if (class_locked) pthread_mutex_unlock(&g_class_lock);
    c->eax = best_handle;
}

static void h_get_class_info(CPU *c)
{
    const uint32_t instance = wc3_legacy_arg32(c, 0u);
    const uint32_t class_arg = wc3_legacy_arg32(c, 1u);
    const uint32_t out = wc3_legacy_arg32(c, 2u);

    if (!out || !wc3_legacy_guest_ok(out, WNDCLASSA_SIZE)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }
    if (!class_arg) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    WindowClassFact snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    char name[WINDOW_TEXT_MAX];
    int have_name = 0;
    int found = 0;
    int builtin = 0;

    if (class_arg >= 0x10000u) {
        if (!read_guest_string(class_arg, name, sizeof name)) {
            wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
            c->eax = 0u;
            return;
        }
        have_name = 1;
    }

    pthread_mutex_lock(&g_class_lock);
    const WindowClassFact *fact = class_arg < 0x10000u
        ? find_class_atom(class_arg)
        : find_class_name(name);
    if (fact) {
        snapshot = *fact;
        found = 1;
    }
    pthread_mutex_unlock(&g_class_lock);

    if (!found && have_name) {
        const BuiltinClassFact *builtin_fact = find_builtin_class(name);
        if (builtin_fact) {
            copy_text(snapshot.name, sizeof snapshot.name, builtin_fact->name);
            snapshot.style = builtin_fact->style;
            snapshot.cls_extra = builtin_fact->cls_extra;
            snapshot.wnd_extra = builtin_fact->wnd_extra;
            snapshot.instance = instance;
            found = 1;
            builtin = 1;
        }
    }

    if (!found) {
        wc3_legacy_set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        c->eax = 0u;
        return;
    }
    if (!builtin && instance && snapshot.instance &&
        instance != snapshot.instance) {
        wc3_legacy_set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        c->eax = 0u;
        return;
    }

    uint32_t class_name_ptr = snapshot.class_name_ptr;
    if (!class_name_ptr && class_arg >= 0x10000u)
        class_name_ptr = class_arg;

    wc3_legacy_guest_zero(out, WNDCLASSA_SIZE);
    wc3_legacy_wr32g(out + 0u, snapshot.style);
    wc3_legacy_wr32g(out + 4u, snapshot.wnd_proc);
    wc3_legacy_wr32g(out + 8u, (uint32_t)snapshot.cls_extra);
    wc3_legacy_wr32g(out + 12u, (uint32_t)snapshot.wnd_extra);
    wc3_legacy_wr32g(out + 16u, snapshot.instance);
    wc3_legacy_wr32g(out + 20u, snapshot.icon);
    wc3_legacy_wr32g(out + 24u, snapshot.cursor);
    wc3_legacy_wr32g(out + 28u, snapshot.background);
    wc3_legacy_wr32g(out + 32u, snapshot.menu_name);
    wc3_legacy_wr32g(out + 36u, class_name_ptr);
    c->eax = 1u;
}

static void h_get_update_rgn(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t region = wc3_legacy_arg32(c, 1u);
    (void)wc3_legacy_arg32(c, 2u);

    if (!region) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }
    if (handle == DESKTOP_WINDOW_HANDLE) {
        c->eax = 1u;
        return;
    }

    pthread_mutex_lock(&g_window_lock);
    const int found = find_window_index_locked(handle) < WINDOW_MAX;
    pthread_mutex_unlock(&g_window_lock);
    if (!found) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }
    /* The target has no invalidated-region queue: InvalidateRect is still a
     * legacy no-op, so every window's update region is empty (NULLREGION). */
    c->eax = 1u;
}

static void h_set_window_placement(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t placement = wc3_legacy_arg32(c, 1u);

    if (!placement ||
        !wc3_legacy_guest_ok(placement, WINDOW_PLACEMENT_SIZE)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }
    if (wc3_legacy_rd32g(placement + 0u) != WINDOW_PLACEMENT_SIZE) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    const uint32_t flags = wc3_legacy_rd32g(placement + 4u);
    const uint32_t show_cmd =
        normalize_show_cmd(wc3_legacy_rd32g(placement + 8u));
    const int32_t min_x = (int32_t)wc3_legacy_rd32g(placement + 12u);
    const int32_t min_y = (int32_t)wc3_legacy_rd32g(placement + 16u);
    const int32_t max_x = (int32_t)wc3_legacy_rd32g(placement + 20u);
    const int32_t max_y = (int32_t)wc3_legacy_rd32g(placement + 24u);
    const int32_t left = (int32_t)wc3_legacy_rd32g(placement + 28u);
    const int32_t top = (int32_t)wc3_legacy_rd32g(placement + 32u);
    const int32_t right = (int32_t)wc3_legacy_rd32g(placement + 36u);
    const int32_t bottom = (int32_t)wc3_legacy_rd32g(placement + 40u);

    if (right < left || bottom < top) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    WindowFact *window = lock_window(handle);
    if (!window) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }
    window->show_cmd = show_cmd;
    window->placement_flags = flags;
    if (flags & WPF_SETMINPOSITION) {
        window->min_x = min_x;
        window->min_y = min_y;
    } else {
        window->min_x = -1;
        window->min_y = -1;
    }
    window->max_x = max_x;
    window->max_y = max_y;
    window->x = left;
    window->y = top;
    window->width = (uint32_t)(right - left);
    window->height = (uint32_t)(bottom - top);
    unlock_window(window);
    c->eax = 1u;
}

static void h_get_dlg_ctrl_id(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    uint32_t id = 0u;
    pthread_mutex_lock(&g_window_lock);
    const unsigned index = find_window_index_locked(handle);
    if (index < WINDOW_MAX && !is_top_parent(g_windows[index].parent))
        id = g_windows[index].menu;
    pthread_mutex_unlock(&g_window_lock);
    c->eax = id;
}

static void h_get_dlg_item(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t id = wc3_legacy_arg32(c, 1u);

    uint32_t handle = 0u;
    int parent_invalid = 0;
    unsigned item_index = WINDOW_MAX;
    pthread_mutex_lock(&g_window_lock);
    parent_invalid = parent != DESKTOP_WINDOW_HANDLE &&
        find_window_index_locked(parent) >= WINDOW_MAX;
    if (!parent_invalid)
        item_index = find_dlg_item_index_locked(parent, id);
    if (item_index < WINDOW_MAX) handle = g_windows[item_index].handle;
    pthread_mutex_unlock(&g_window_lock);

    if (parent_invalid) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }
    if (!handle) {
        wc3_legacy_set_last_error(ERROR_CONTROL_ID_NOT_FOUND);
        c->eax = 0u;
        return;
    }
    c->eax = handle;
}

static void h_get_dlg_item_text(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t id = wc3_legacy_arg32(c, 1u);
    const uint32_t buffer = wc3_legacy_arg32(c, 2u);
    const uint32_t capacity = wc3_legacy_arg32(c, 3u);

    /* Win32 returns zero without touching the buffer when cchMax is zero or
     * the buffer is null, and preserves the caller's last error. */
    if (!buffer || capacity == 0u) {
        c->eax = 0u;
        return;
    }
    if (!wc3_legacy_guest_ok(buffer, capacity)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    char title[WINDOW_TEXT_MAX];
    title[0] = '\0';
    int found = 0;
    int parent_invalid = 0;
    unsigned item_index = WINDOW_MAX;
    pthread_mutex_lock(&g_window_lock);
    parent_invalid = parent != DESKTOP_WINDOW_HANDLE &&
        find_window_index_locked(parent) >= WINDOW_MAX;
    if (!parent_invalid) {
        item_index = find_dlg_item_index_locked(parent, id);
        if (item_index < WINDOW_MAX) {
            pthread_mutex_lock(&g_window_locks[item_index]);
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_window_lock);

    if (found) {
        copy_text(title, sizeof title, g_windows[item_index].title);
        pthread_mutex_unlock(&g_window_locks[item_index]);
    }
    if (parent_invalid) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }

    if (!found) {
        wc3_legacy_set_last_error(ERROR_CONTROL_ID_NOT_FOUND);
        c->eax = 0u;
        return;
    }

    const size_t length = strlen(title);
    const size_t copied = length < capacity - 1u ? length : capacity - 1u;
    for (size_t i = 0; i < copied; i++)
        wc3_legacy_wr8g(buffer + (uint32_t)i, (uint8_t)title[i]);
    wc3_legacy_wr8g(buffer + (uint32_t)copied, 0u);
    c->eax = (uint32_t)copied;
}

static void h_set_dlg_item_text(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t id = wc3_legacy_arg32(c, 1u);
    const uint32_t text = wc3_legacy_arg32(c, 2u);
    char title[WINDOW_TEXT_MAX];
    title[0] = '\0';

    if (text) {
        if (!wc3_legacy_guest_ok(text, 1u)) {
            wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
            c->eax = 0u;
            return;
        }
        (void)read_guest_string(text, title, sizeof title);
    }

    int found = 0;
    int parent_invalid = 0;
    unsigned item_index = WINDOW_MAX;
    pthread_mutex_lock(&g_window_lock);
    parent_invalid = parent != DESKTOP_WINDOW_HANDLE &&
        find_window_index_locked(parent) >= WINDOW_MAX;
    if (!parent_invalid) {
        item_index = find_dlg_item_index_locked(parent, id);
        if (item_index < WINDOW_MAX) {
            pthread_mutex_lock(&g_window_locks[item_index]);
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_window_lock);

    if (found) {
        copy_text(g_windows[item_index].title,
                  sizeof g_windows[item_index].title, title);
        pthread_mutex_unlock(&g_window_locks[item_index]);
    }
    if (parent_invalid) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }

    if (!found) {
        wc3_legacy_set_last_error(ERROR_CONTROL_ID_NOT_FOUND);
        c->eax = 0u;
        return;
    }
    c->eax = 1u;
}

static void h_get_window_placement(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t placement = wc3_legacy_arg32(c, 1u);
    if (!placement || !wc3_legacy_guest_ok(placement, WINDOW_PLACEMENT_SIZE)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    int32_t x = 0, y = 0;
    int32_t min_x = -1, min_y = -1, max_x = -1, max_y = -1;
    uint32_t width = 800u, height = 600u, show_cmd = SW_SHOWNORMAL;
    uint32_t flags = 0u;
    int found = 0;

    WindowFact *window = lock_window(handle);
    if (window) {
        found = 1;
        x = window->x;
        y = window->y;
        width = window->width;
        height = window->height;
        show_cmd = window->show_cmd;
        flags = window->placement_flags;
        min_x = window->min_x;
        min_y = window->min_y;
        max_x = window->max_x;
        max_y = window->max_y;
        unlock_window(window);
    }

    if (!found) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }

    wc3_legacy_guest_zero(placement, WINDOW_PLACEMENT_SIZE);
    wc3_legacy_wr32g(placement + 0u, WINDOW_PLACEMENT_SIZE);
    wc3_legacy_wr32g(placement + 4u, flags);
    wc3_legacy_wr32g(placement + 8u, show_cmd);
    wc3_legacy_wr32g(placement + 12u, (uint32_t)min_x);
    wc3_legacy_wr32g(placement + 16u, (uint32_t)min_y);
    wc3_legacy_wr32g(placement + 20u, (uint32_t)max_x);
    wc3_legacy_wr32g(placement + 24u, (uint32_t)max_y);
    wc3_legacy_wr32g(placement + 28u, (uint32_t)x);
    wc3_legacy_wr32g(placement + 32u, (uint32_t)y);
    wc3_legacy_wr32g(placement + 36u, (uint32_t)(x + (int32_t)width));
    wc3_legacy_wr32g(placement + 40u, (uint32_t)(y + (int32_t)height));
    c->eax = 1u;
}

static void h_is_iconic(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    c->eax = 0u;
    WindowFact *window = lock_window(handle);
    if (window) {
        c->eax = window->show_cmd == SW_SHOWMINIMIZED ? 1u : 0u;
        unlock_window(window);
    }
}

static void h_adjust_window_rect_ex(CPU *c)
{
    const uint32_t rect = wc3_legacy_arg32(c, 0u);
    (void)wc3_legacy_arg32(c, 1u);
    (void)wc3_legacy_arg32(c, 2u);
    (void)wc3_legacy_arg32(c, 3u);
    /* The Android target window has no system frame or caption.  Its client
     * rect and window rect are the same rectangle, so the adjustment is
     * identity after validating the guest RECT. */
    c->eax = rect && wc3_legacy_guest_ok(rect, 16u) ? 1u : 0u;
}

static void h_unregister_class(CPU *c)
{
    const uint32_t class_arg = wc3_legacy_arg32(c, 0u);
    char name[WINDOW_TEXT_MAX];
    int class_locked = 0;
    name[0] = '\0';

    if (!class_arg) {
        c->eax = 0u;
        return;
    }
    if (class_arg >= 0x10000u) {
        read_guest_string(class_arg, name, sizeof name);
    } else {
        pthread_mutex_lock(&g_class_lock);
        class_locked = 1;
        WindowClassFact *fact = find_class_atom(class_arg);
        if (fact) copy_text(name, sizeof name, fact->name);
    }
    if (!name[0]) {
        if (class_locked) pthread_mutex_unlock(&g_class_lock);
        c->eax = 0u;
        return;
    }

    if (!class_locked) pthread_mutex_lock(&g_class_lock);
    /* The window scan and the class removal stay under one class lock, as in
     * the former single-lock version, so a concurrent RegisterClassA cannot
     * slip a same-named class in between.  Only table-owned class_name is
     * read, so the window scan needs no slot lock. */
    pthread_mutex_lock(&g_window_lock);
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        if (g_windows[i].used &&
            text_equal_ci(g_windows[i].class_name, name)) {
            pthread_mutex_unlock(&g_window_lock);
            pthread_mutex_unlock(&g_class_lock);
            c->eax = 0u;
            return;
        }
    }
    WindowClassFact *fact = find_class_name(name);
    if (fact) memset(fact, 0, sizeof *fact);
    pthread_mutex_unlock(&g_window_lock);
    pthread_mutex_unlock(&g_class_lock);
    c->eax = fact ? 1u : 0u;
}

static int window_origin(uint32_t handle, int32_t *x, int32_t *y)
{
    if (handle == 0u || handle == DESKTOP_WINDOW_HANDLE) {
        *x = 0;
        *y = 0;
        return 1;
    }

    WindowFact *window = lock_window(handle);
    if (!window) return 0;
    *x = window->x;
    *y = window->y;
    unlock_window(window);
    return 1;
}

static void h_map_window_points(CPU *c)
{
    const uint32_t from = wc3_legacy_arg32(c, 0u);
    const uint32_t to = wc3_legacy_arg32(c, 1u);
    const uint32_t points = wc3_legacy_arg32(c, 2u);
    const uint32_t count = wc3_legacy_arg32(c, 3u);

    if (count == 0u) {
        c->eax = 0u;
        return;
    }
    if (!points || count > UINT32_MAX / 8u ||
        !wc3_legacy_guest_ok(points, count * 8u)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    int32_t from_x = 0, from_y = 0, to_x = 0, to_y = 0;
    if (!window_origin(from, &from_x, &from_y) ||
        !window_origin(to, &to_x, &to_y)) {
        wc3_legacy_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        c->eax = 0u;
        return;
    }

    const int32_t dx = from_x - to_x;
    const int32_t dy = from_y - to_y;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = points + i * 8u;
        const int32_t x = (int32_t)wc3_legacy_rd32g(at);
        const int32_t y = (int32_t)wc3_legacy_rd32g(at + 4u);
        wc3_legacy_wr32g(at, (uint32_t)(x + dx));
        wc3_legacy_wr32g(at + 4u, (uint32_t)(y + dy));
    }
    c->eax = count;
}

static void h_is_dialog_message(CPU *c)
{
    /* The target creates no dialog windows (CreateDialog*Param* is not part of
     * this slice), so no message can belong to a dialog.  Win32 returns FALSE
     * for a non-dialog window, and the caller then dispatches the message. */
    (void)wc3_legacy_arg32(c, 0u);
    (void)wc3_legacy_arg32(c, 1u);
    c->eax = 0u;
}

static void h_monitor_from_window(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t flags = wc3_legacy_arg32(c, 1u);
    uint32_t width = 0, height = 0;
    int intersects = 0;

    if (!monitor_size(&width, &height)) {
        c->eax = 0u;
        return;
    }

    if (handle == DESKTOP_WINDOW_HANDLE) {
        intersects = 1;
    } else if (handle) {
        WindowFact *window = lock_window(handle);
        if (window) {
            intersects = window_intersects_monitor(window, width, height);
            unlock_window(window);
        }
    }

    /* Android exposes one monitor.  A valid window on that monitor maps to it;
     * the default flags also select the primary monitor for an unknown or
     * off-screen window, while MONITOR_DEFAULTTONULL reports no monitor. */
    if (intersects || flags != MONITOR_DEFAULTTONULL)
        c->eax = MONITOR_HANDLE;
    else
        c->eax = 0u;
}

static void h_get_monitor_info(CPU *c)
{
    const uint32_t monitor = wc3_legacy_arg32(c, 0u);
    const uint32_t info = wc3_legacy_arg32(c, 1u);
    uint32_t width = 0, height = 0;

    if (monitor != MONITOR_HANDLE) {
        wc3_legacy_set_last_error(ERROR_INVALID_MONITOR_HANDLE);
        c->eax = 0u;
        return;
    }
    if (!info || !wc3_legacy_guest_ok(info, 4u)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    const uint32_t size = wc3_legacy_rd32g(info);
    if ((size != MONITORINFO_SIZE && size != MONITORINFOEXA_SIZE) ||
        !wc3_legacy_guest_ok(info, size)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }
    if (!monitor_size(&width, &height)) {
        wc3_legacy_set_last_error(ERROR_INVALID_PARAMETER);
        c->eax = 0u;
        return;
    }

    wc3_legacy_guest_zero(info, size);
    wc3_legacy_wr32g(info + 0u, size);
    wc3_legacy_wr32g(info + 4u, 0u);
    wc3_legacy_wr32g(info + 8u, 0u);
    wc3_legacy_wr32g(info + 12u, width);
    wc3_legacy_wr32g(info + 16u, height);
    /* Android has no taskbar or separate work area; the game window covers the
     * full display. */
    wc3_legacy_wr32g(info + 20u, 0u);
    wc3_legacy_wr32g(info + 24u, 0u);
    wc3_legacy_wr32g(info + 28u, width);
    wc3_legacy_wr32g(info + 32u, height);
    wc3_legacy_wr32g(info + 36u, MONITORINFOF_PRIMARY);
    if (size == MONITORINFOEXA_SIZE) {
        for (unsigned i = 0; i < sizeof MONITOR_DEVICE_NAME; i++)
            wc3_legacy_wr8g(info + 40u + i,
                            (uint8_t)MONITOR_DEVICE_NAME[i]);
    }
    c->eax = 1u;
}

typedef struct WindowFactChain {
    const char *name;
    void (*impl)(CPU *c);
    void (**previous)(CPU *c);
} WindowFactChain;

static const WindowFactChain g_chain[] = {
    { "RegisterClassExA", h_register_class_ex, &g_previous_register_class },
    { "RegisterClassA", h_register_class_a, &g_previous_register_class_a },
    { "CreateWindowExA", h_create_window_ex, &g_previous_create_window },
    { "DestroyWindow", h_destroy_window, &g_previous_destroy_window },
    { "ShowWindow", h_show_window, &g_previous_show_window },
    { "SetWindowPos", h_set_window_pos, &g_previous_set_window_pos },
    { "SetWindowTextA", h_set_window_text, &g_previous_set_window_text },
};

typedef struct WindowFactAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} WindowFactAddition;

static const WindowFactAddition g_additions[] = {
    { "GetTopWindow", 1, h_get_top_window },
    { "GetClassNameA", 3, h_get_class_name },
    { "IsWindow", 1, h_is_window },
    { "GetWindowPlacement", 2, h_get_window_placement },
    { "IsIconic", 1, h_is_iconic },
    { "AdjustWindowRectEx", 4, h_adjust_window_rect_ex },
    { "UnregisterClassA", 2, h_unregister_class },
    { "IsDialogMessageA", 2, h_is_dialog_message },
    { "MapWindowPoints", 4, h_map_window_points },
    { "MonitorFromWindow", 2, h_monitor_from_window },
    { "GetMonitorInfoA", 2, h_get_monitor_info },
    { "FindWindowExA", 4, h_find_window_ex },
    { "GetClassInfoA", 3, h_get_class_info },
    { "GetWindowThreadProcessId", 2, h_get_window_thread_process_id },
    { "GetUpdateRgn", 3, h_get_update_rgn },
    { "SetWindowPlacement", 2, h_set_window_placement },
    { "GetDlgCtrlID", 1, h_get_dlg_ctrl_id },
    { "GetDlgItem", 2, h_get_dlg_item },
    { "GetDlgItemTextA", 4, h_get_dlg_item_text },
    { "SetDlgItemTextA", 3, h_set_dlg_item_text },
};

int wc3_hle_user32_window_facts_install(void)
{
    const Wc3Platform *platform = wc3_platform();
    if (!platform || !platform->thread.self || !platform->thread.id)
        return -1;
    g_thread = &platform->thread;
    g_process_id = (uint32_t)g_thread->id(g_thread->ctx,
                                          g_thread->self(g_thread->ctx));

    ensure_window_locks();

    /* Install is a bootstrap step.  Holding both table locks and every slot
     * lock gives the reset the same exclusivity the former single lock had,
     * and follows the class -> window -> slot order used at runtime. */
    pthread_mutex_lock(&g_class_lock);
    pthread_mutex_lock(&g_window_lock);
    for (unsigned i = 0; i < WINDOW_MAX; i++)
        pthread_mutex_lock(&g_window_locks[i]);
    memset(g_classes, 0, sizeof g_classes);
    memset(g_windows, 0, sizeof g_windows);
    g_next_serial = 1u;
    for (unsigned i = 0; i < WINDOW_MAX; i++)
        pthread_mutex_unlock(&g_window_locks[i]);
    pthread_mutex_unlock(&g_window_lock);
    pthread_mutex_unlock(&g_class_lock);

    for (unsigned i = 0; i < sizeof g_chain / sizeof g_chain[0]; i++) {
        if (wc3_legacy_hle_chain("USER32.dll", g_chain[i].name,
                                 g_chain[i].impl,
                                 g_chain[i].previous) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_chain / sizeof g_chain[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
