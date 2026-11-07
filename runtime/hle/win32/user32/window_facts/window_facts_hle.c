/*
 * window_facts_hle.c - target-side USER32 window-creation facts.
 *
 * Window creation and the input queue are still owned by the quarantine.  A
 * thin chain override calls the as-built handler first and records the handle,
 * class, parent and placement it produced.  The query imports then answer from
 * that table.  No host lock is held while the as-built handler or guest memory
 * is touched.
 */
#include "window_facts_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define DESKTOP_WINDOW_HANDLE 0x00010000u
#define WINDOW_MAX 32u
#define WINDOW_CLASS_MAX 16u
#define WINDOW_TEXT_MAX 128u
#define WINDOW_PLACEMENT_SIZE 44u

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
    char name[WINDOW_TEXT_MAX];
} WindowClassFact;

typedef struct WindowFact {
    uint8_t used;
    uint32_t handle;
    uint32_t parent;
    uint32_t serial;
    uint32_t show_cmd;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    char class_name[WINDOW_TEXT_MAX];
} WindowFact;

static WindowClassFact g_classes[WINDOW_CLASS_MAX];
static WindowFact g_windows[WINDOW_MAX];
static uint32_t g_next_serial = 1u;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_previous_register_class)(CPU *c);
static void (*g_previous_create_window)(CPU *c);
static void (*g_previous_destroy_window)(CPU *c);
static void (*g_previous_show_window)(CPU *c);
static void (*g_previous_set_window_pos)(CPU *c);

static int read_guest_string(uint32_t address, char *buffer, size_t capacity)
{
    if (!address || !buffer || capacity < 2u) return 0;
    for (size_t i = 0; i + 1u < capacity; i++) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t at = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(at, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(at);
        buffer[i] = ch;
        if (!ch) return i != 0u;
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

static WindowFact *find_window(uint32_t handle)
{
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        if (g_windows[i].used && g_windows[i].handle == handle)
            return &g_windows[i];
    }
    return NULL;
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
        if (g_classes[i].used && strcmp(g_classes[i].name, name) == 0)
            return &g_classes[i];
    }
    return NULL;
}

static void record_class(uint32_t atom, const char *name)
{
    if (!atom || !name || !name[0]) return;
    pthread_mutex_lock(&g_lock);
    WindowClassFact *existing = find_class_name(name);
    if (!existing) {
        for (unsigned i = 0; i < WINDOW_CLASS_MAX; i++) {
            if (g_classes[i].used) continue;
            existing = &g_classes[i];
            break;
        }
    }
    if (existing) {
        existing->used = 1u;
        existing->atom = atom;
        copy_text(existing->name, sizeof existing->name, name);
    }
    pthread_mutex_unlock(&g_lock);
}

static void record_window(uint32_t handle, uint32_t parent,
                          const char *class_name, int32_t x, int32_t y,
                          uint32_t width, uint32_t height)
{
    if (!handle) return;
    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
    if (!window) {
        for (unsigned i = 0; i < WINDOW_MAX; i++) {
            if (g_windows[i].used) continue;
            window = &g_windows[i];
            break;
        }
    }
    if (window) {
        memset(window, 0, sizeof *window);
        window->used = 1u;
        window->handle = handle;
        window->parent = parent;
        window->serial = g_next_serial++;
        window->show_cmd = SW_SHOWNORMAL;
        window->x = x;
        window->y = y;
        window->width = width;
        window->height = height;
        copy_text(window->class_name, sizeof window->class_name, class_name);
    }
    pthread_mutex_unlock(&g_lock);
}

static void remove_window(uint32_t handle)
{
    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
    if (window) memset(window, 0, sizeof *window);
    pthread_mutex_unlock(&g_lock);
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

static void h_register_class_ex(CPU *c)
{
    const uint32_t description = wc3_legacy_arg32(c, 0u);
    char name[WINDOW_TEXT_MAX];
    int have_name = 0;
    if (description && wc3_legacy_guest_ok(description, 48u)) {
        have_name = read_guest_string(wc3_legacy_rd32g(description + 40u),
                                      name, sizeof name);
    }

    if (g_previous_register_class)
        g_previous_register_class(c);

    if (c->eax && have_name)
        record_class(c->eax, name);
}

static void h_create_window_ex(CPU *c)
{
    const uint32_t class_arg = wc3_legacy_arg32(c, 1u);
    const uint32_t parent = wc3_legacy_arg32(c, 8u);
    const uint32_t raw_x = wc3_legacy_arg32(c, 4u);
    const uint32_t raw_y = wc3_legacy_arg32(c, 5u);
    const uint32_t raw_width = wc3_legacy_arg32(c, 6u);
    const uint32_t raw_height = wc3_legacy_arg32(c, 7u);
    char class_name[WINDOW_TEXT_MAX];
    class_name[0] = '\0';

    if (class_arg && class_arg < 0x10000u) {
        pthread_mutex_lock(&g_lock);
        WindowClassFact *fact = find_class_atom(class_arg);
        if (fact) copy_text(class_name, sizeof class_name, fact->name);
        pthread_mutex_unlock(&g_lock);
    } else {
        read_guest_string(class_arg, class_name, sizeof class_name);
    }

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

    record_window(handle, parent, class_name, x, y, width, height);
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

    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
    if (window) window->show_cmd = normalize_show_cmd(command);
    pthread_mutex_unlock(&g_lock);
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

    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
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
    }
    pthread_mutex_unlock(&g_lock);
}

static void h_get_top_window(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    uint32_t best_handle = 0u;
    uint32_t best_serial = 0u;

    pthread_mutex_lock(&g_lock);
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
    pthread_mutex_unlock(&g_lock);
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
        pthread_mutex_lock(&g_lock);
        WindowFact *window = find_window(handle);
        if (window) copy_text(name, sizeof name, window->class_name);
        pthread_mutex_unlock(&g_lock);
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
    pthread_mutex_lock(&g_lock);
    c->eax = find_window(handle) ? 1u : 0u;
    pthread_mutex_unlock(&g_lock);
}

static void h_get_window_placement(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t placement = wc3_legacy_arg32(c, 1u);
    if (!placement || !wc3_legacy_guest_ok(placement, WINDOW_PLACEMENT_SIZE)) {
        c->eax = 0u;
        return;
    }

    int32_t x = 0, y = 0;
    uint32_t width = 800u, height = 600u, show_cmd = SW_SHOWNORMAL;
    int found = 0;

    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
    if (window) {
        found = 1;
        x = window->x;
        y = window->y;
        width = window->width;
        height = window->height;
        show_cmd = window->show_cmd;
    }
    pthread_mutex_unlock(&g_lock);

    if (!found) {
        c->eax = 0u;
        return;
    }

    wc3_legacy_guest_zero(placement, WINDOW_PLACEMENT_SIZE);
    wc3_legacy_wr32g(placement + 0u, WINDOW_PLACEMENT_SIZE);
    wc3_legacy_wr32g(placement + 4u, 0u);
    wc3_legacy_wr32g(placement + 8u, show_cmd);
    wc3_legacy_wr32g(placement + 12u, 0xFFFFFFFFu);
    wc3_legacy_wr32g(placement + 16u, 0xFFFFFFFFu);
    wc3_legacy_wr32g(placement + 20u, 0xFFFFFFFFu);
    wc3_legacy_wr32g(placement + 24u, 0xFFFFFFFFu);
    wc3_legacy_wr32g(placement + 28u, (uint32_t)x);
    wc3_legacy_wr32g(placement + 32u, (uint32_t)y);
    wc3_legacy_wr32g(placement + 36u, (uint32_t)(x + (int32_t)width));
    wc3_legacy_wr32g(placement + 40u, (uint32_t)(y + (int32_t)height));
    c->eax = 1u;
}

static void h_is_iconic(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    pthread_mutex_lock(&g_lock);
    WindowFact *window = find_window(handle);
    c->eax = window && window->show_cmd == SW_SHOWMINIMIZED ? 1u : 0u;
    pthread_mutex_unlock(&g_lock);
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
    name[0] = '\0';

    if (!class_arg) {
        c->eax = 0u;
        return;
    }
    if (class_arg < 0x10000u) {
        pthread_mutex_lock(&g_lock);
        WindowClassFact *fact = find_class_atom(class_arg);
        if (fact) copy_text(name, sizeof name, fact->name);
        pthread_mutex_unlock(&g_lock);
    } else {
        read_guest_string(class_arg, name, sizeof name);
    }
    if (!name[0]) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < WINDOW_MAX; i++) {
        if (g_windows[i].used &&
            strcmp(g_windows[i].class_name, name) == 0) {
            pthread_mutex_unlock(&g_lock);
            c->eax = 0u;
            return;
        }
    }
    WindowClassFact *fact = find_class_name(name);
    if (fact) memset(fact, 0, sizeof *fact);
    pthread_mutex_unlock(&g_lock);
    c->eax = fact ? 1u : 0u;
}

typedef struct WindowFactChain {
    const char *name;
    void (*impl)(CPU *c);
    void (**previous)(CPU *c);
} WindowFactChain;

static const WindowFactChain g_chain[] = {
    { "RegisterClassExA", h_register_class_ex, &g_previous_register_class },
    { "CreateWindowExA", h_create_window_ex, &g_previous_create_window },
    { "DestroyWindow", h_destroy_window, &g_previous_destroy_window },
    { "ShowWindow", h_show_window, &g_previous_show_window },
    { "SetWindowPos", h_set_window_pos, &g_previous_set_window_pos },
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
};

int wc3_hle_user32_window_facts_install(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_classes, 0, sizeof g_classes);
    memset(g_windows, 0, sizeof g_windows);
    g_next_serial = 1u;
    pthread_mutex_unlock(&g_lock);

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
