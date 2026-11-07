/*
 * menu_hle.c - target-side USER32 menu objects.
 *
 * A menu owns a compact item array.  Item text is ANSI, matching MENUITEMINFOA,
 * and submenus are ordinary target menu handles.  DestroyMenu and DeleteMenu
 * release submenus recursively so the handle table cannot leak across a menu
 * rebuild.
 */
#include "menu_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

#define MENU_HANDLE_BASE 0x00110000u
#define MENU_HANDLE_STRIDE 4u
#define MENU_COUNT 64u
#define MENU_ITEM_MAX 64u
#define MENU_TEXT_MAX 128u
#define MENU_BIND_MAX 32u
#define MENU_BIND_NONE 0xFFFFFFFFu

#define MENUITEMINFO_SIZE 48u
#define MENUITEMINFO_MIN_SIZE 44u

#define MIIM_STATE 0x00000001u
#define MIIM_ID 0x00000002u
#define MIIM_SUBMENU 0x00000004u
#define MIIM_CHECKMARKS 0x00000008u
#define MIIM_TYPE 0x00000010u
#define MIIM_DATA 0x00000020u
#define MIIM_STRING 0x00000040u
#define MIIM_BITMAP 0x00000080u
#define MIIM_FTYPE 0x00000100u

#define MFT_BITMAP 0x00000004u
#define MFT_SEPARATOR 0x00000800u
#define MFT_OWNERDRAW 0x00000100u

#define MFS_CHECKED 0x00000008u

#define MF_BYCOMMAND 0x00000000u
#define MF_BYPOSITION 0x00000400u

typedef struct MenuItemInfo {
    uint32_t cb_size;
    uint32_t mask;
    uint32_t type;
    uint32_t state;
    uint32_t id;
    uint32_t submenu;
    uint32_t bitmap_checked;
    uint32_t bitmap_unchecked;
    uint32_t item_data;
    uint32_t text_address;
    uint32_t text_capacity;
    uint32_t bitmap_item;
} MenuItemInfo;

typedef struct MenuItem {
    uint32_t type;
    uint32_t state;
    uint32_t id;
    uint32_t submenu;
    uint32_t bitmap_checked;
    uint32_t bitmap_unchecked;
    uint32_t item_data;
    uint32_t bitmap_item;
    char text[MENU_TEXT_MAX];
} MenuItem;

typedef struct MenuObject {
    uint8_t used;
    uint32_t handle;
    uint16_t count;
    MenuItem items[MENU_ITEM_MAX];
} MenuObject;

typedef struct MenuBinding {
    uint8_t used;
    uint32_t hwnd;
    uint32_t menu;
} MenuBinding;

static MenuObject g_menus[MENU_COUNT];
static MenuBinding g_bindings[MENU_BIND_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (!dst || capacity == 0u) return;
    if (!src) src = "";
    size_t length = strlen(src);
    if (length >= capacity) length = capacity - 1u;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int read_guest_string(uint32_t address, char *buffer, size_t capacity)
{
    if (!buffer || capacity < 2u) return 0;
    if (!address) {
        buffer[0] = '\0';
        return 1;
    }
    for (size_t i = 0; i + 1u < capacity; i++) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t at = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(at, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(at);
        buffer[i] = ch;
        if (!ch) return 1;
    }
    return 0;
}

static void write_guest_string(uint32_t address, const char *text,
                               uint32_t capacity)
{
    if (!text) text = "";
    const size_t length = strlen(text);
    for (uint32_t i = 0; i < length && i + 1u < capacity; i++) {
        if (!wc3_legacy_guest_ok(address + i, 1u)) return;
        wc3_legacy_wr8g(address + i, (uint8_t)text[i]);
    }
    if (capacity > 0u) {
        const uint32_t at = address + (uint32_t)(length < capacity - 1u
                                                 ? length : capacity - 1u);
        if (wc3_legacy_guest_ok(at, 1u)) wc3_legacy_wr8g(at, 0u);
    }
}

static uint32_t read_u32(uint32_t address)
{
    return wc3_legacy_rd32g(address);
}

static void write_u32(uint32_t address, uint32_t value)
{
    if (address && wc3_legacy_guest_ok(address, 4u))
        wc3_legacy_wr32g(address, value);
}

static int read_menu_item_info(uint32_t address, MenuItemInfo *info)
{
    if (!address || !info) return 0;
    if (!wc3_legacy_guest_ok(address, MENUITEMINFO_MIN_SIZE)) return 0;
    const uint32_t cb_size = read_u32(address);
    if (cb_size < MENUITEMINFO_MIN_SIZE ||
        !wc3_legacy_guest_ok(address, cb_size))
        return 0;
    if (cb_size > MENUITEMINFO_SIZE) return 0;

    memset(info, 0, sizeof *info);
    info->cb_size = cb_size;
    info->mask = read_u32(address + 4u);
    info->type = read_u32(address + 8u);
    info->state = read_u32(address + 12u);
    info->id = read_u32(address + 16u);
    info->submenu = read_u32(address + 20u);
    info->bitmap_checked = read_u32(address + 24u);
    info->bitmap_unchecked = read_u32(address + 28u);
    info->item_data = read_u32(address + 32u);
    info->text_address = read_u32(address + 36u);
    info->text_capacity = read_u32(address + 40u);
    if (cb_size >= MENUITEMINFO_SIZE)
        info->bitmap_item = read_u32(address + 44u);
    return 1;
}

static MenuObject *find_menu(uint32_t handle)
{
    for (unsigned i = 0; i < MENU_COUNT; i++) {
        if (g_menus[i].used && g_menus[i].handle == handle)
            return &g_menus[i];
    }
    return NULL;
}

static MenuObject *new_menu(uint32_t *handle_out)
{
    for (unsigned i = 0; i < MENU_COUNT; i++) {
        if (g_menus[i].used) continue;
        memset(&g_menus[i], 0, sizeof g_menus[i]);
        g_menus[i].used = 1u;
        g_menus[i].handle = MENU_HANDLE_BASE + i * MENU_HANDLE_STRIDE;
        *handle_out = g_menus[i].handle;
        return &g_menus[i];
    }
    return NULL;
}

static void clear_binding(uint32_t menu)
{
    for (unsigned i = 0; i < MENU_BIND_MAX; i++) {
        if (g_bindings[i].used && g_bindings[i].menu == menu)
            memset(&g_bindings[i], 0, sizeof g_bindings[i]);
    }
}

static void destroy_menu_locked(uint32_t handle, unsigned depth)
{
    MenuObject *menu = find_menu(handle);
    if (!menu) return;
    if (depth < 16u) {
        for (unsigned i = 0; i < menu->count; i++) {
            if (menu->items[i].submenu &&
                menu->items[i].submenu != handle)
                destroy_menu_locked(menu->items[i].submenu, depth + 1u);
        }
    }
    clear_binding(handle);
    memset(menu, 0, sizeof *menu);
}

static MenuBinding *find_binding(uint32_t hwnd)
{
    for (unsigned i = 0; i < MENU_BIND_MAX; i++) {
        if (g_bindings[i].used && g_bindings[i].hwnd == hwnd)
            return &g_bindings[i];
    }
    return NULL;
}

static MenuBinding *ensure_binding(uint32_t hwnd)
{
    MenuBinding *binding = find_binding(hwnd);
    if (binding) return binding;
    for (unsigned i = 0; i < MENU_BIND_MAX; i++) {
        if (g_bindings[i].used) continue;
        memset(&g_bindings[i], 0, sizeof g_bindings[i]);
        g_bindings[i].used = 1u;
        g_bindings[i].hwnd = hwnd;
        return &g_bindings[i];
    }
    return NULL;
}

static int item_is_text(const MenuItemInfo *info)
{
    return !(info->type & (MFT_BITMAP | MFT_SEPARATOR | MFT_OWNERDRAW));
}

static int item_text_from_info(const MenuItemInfo *info, char *text,
                               size_t capacity)
{
    text[0] = '\0';
    if (!(info->mask & (MIIM_TYPE | MIIM_STRING))) return 1;
    if (!item_is_text(info)) return 1;
    return read_guest_string(info->text_address, text, capacity);
}

static int apply_item_info(MenuItem *item, const MenuItemInfo *info)
{
    if (info->mask & (MIIM_FTYPE | MIIM_TYPE))
        item->type = info->type;

    if (info->mask & (MIIM_TYPE | MIIM_STRING)) {
        if (item_is_text(info)) {
            char text[MENU_TEXT_MAX];
            if (!item_text_from_info(info, text, sizeof text)) return 0;
            copy_text(item->text, sizeof item->text, text);
        } else {
            item->text[0] = '\0';
            if (info->mask & MIIM_TYPE)
                item->bitmap_item = info->text_address;
        }
    }
    if (info->mask & MIIM_STATE) item->state = info->state;
    if (info->mask & MIIM_ID) item->id = info->id;
    if (info->mask & MIIM_SUBMENU) item->submenu = info->submenu;
    if (info->mask & MIIM_CHECKMARKS) {
        item->bitmap_checked = info->bitmap_checked;
        item->bitmap_unchecked = info->bitmap_unchecked;
    }
    if (info->mask & MIIM_DATA) item->item_data = info->item_data;
    if ((info->mask & MIIM_BITMAP) &&
        info->cb_size >= MENUITEMINFO_SIZE)
        item->bitmap_item = info->bitmap_item;
    return 1;
}

static int item_index_by_position(const MenuObject *menu, uint32_t position)
{
    return position < menu->count ? (int)position : -1;
}

static int item_index_by_command(const MenuObject *menu, uint32_t command)
{
    for (unsigned i = 0; i < menu->count; i++) {
        if (menu->items[i].id == command) return (int)i;
    }
    return -1;
}

static int resolve_item_index(const MenuObject *menu, uint32_t item,
                              uint32_t flags)
{
    if (flags & MF_BYPOSITION)
        return item_index_by_position(menu, item);
    return item_index_by_command(menu, item);
}

static void h_create_menu(CPU *c)
{
    uint32_t handle = 0u;
    pthread_mutex_lock(&g_lock);
    new_menu(&handle);
    pthread_mutex_unlock(&g_lock);
    c->eax = handle;
}

static void h_destroy_menu(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    pthread_mutex_lock(&g_lock);
    const int existed = find_menu(handle) != NULL;
    if (existed) destroy_menu_locked(handle, 0u);
    pthread_mutex_unlock(&g_lock);
    c->eax = existed ? 1u : 0u;
}

static void h_get_menu_item_count(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    pthread_mutex_lock(&g_lock);
    MenuObject *menu = find_menu(handle);
    c->eax = menu ? (uint32_t)menu->count : 0xFFFFFFFFu;
    pthread_mutex_unlock(&g_lock);
}

static void h_get_menu(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    pthread_mutex_lock(&g_lock);
    MenuBinding *binding = find_binding(hwnd);
    c->eax = binding ? binding->menu : 0u;
    pthread_mutex_unlock(&g_lock);
}

static void h_set_menu(CPU *c)
{
    const uint32_t hwnd = wc3_legacy_arg32(c, 0u);
    const uint32_t handle = wc3_legacy_arg32(c, 1u);
    if (!hwnd) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_lock);
    MenuBinding *binding = find_binding(hwnd);
    if (handle && !find_menu(handle)) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    if (!handle) {
        if (binding) memset(binding, 0, sizeof *binding);
        pthread_mutex_unlock(&g_lock);
        c->eax = 1u;
        return;
    }
    if (!binding) binding = ensure_binding(hwnd);
    if (!binding) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    binding->menu = handle;
    pthread_mutex_unlock(&g_lock);
    c->eax = 1u;
}

static void h_draw_menu_bar(CPU *c)
{
    c->eax = wc3_legacy_arg32(c, 0u) ? 1u : 0u;
}

static void h_insert_menu_item(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t item = wc3_legacy_arg32(c, 1u);
    const uint32_t by_position = wc3_legacy_arg32(c, 2u);
    const uint32_t info_address = wc3_legacy_arg32(c, 3u);
    MenuItemInfo info;

    if (!read_menu_item_info(info_address, &info)) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_lock);
    MenuObject *menu = find_menu(handle);
    if (!menu || menu->count >= MENU_ITEM_MAX) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }

    uint32_t position = menu->count;
    if (by_position) {
        position = item == 0xFFFFFFFFu ? menu->count : item;
        if (position > menu->count) position = menu->count;
    } else {
        const int found = item_index_by_command(menu, item);
        if (found >= 0) position = (uint32_t)found;
    }

    MenuItem new_item;
    memset(&new_item, 0, sizeof new_item);
    if (!apply_item_info(&new_item, &info)) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }

    if (position < menu->count) {
        memmove(&menu->items[position + 1u], &menu->items[position],
                (menu->count - position) * sizeof menu->items[0]);
    }
    menu->items[position] = new_item;
    menu->count++;
    pthread_mutex_unlock(&g_lock);
    c->eax = 1u;
}

static void h_set_menu_item_info(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t item = wc3_legacy_arg32(c, 1u);
    const uint32_t by_position = wc3_legacy_arg32(c, 2u);
    const uint32_t info_address = wc3_legacy_arg32(c, 3u);
    MenuItemInfo info;

    if (!read_menu_item_info(info_address, &info)) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_lock);
    MenuObject *menu = find_menu(handle);
    if (!menu) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    const int index = resolve_item_index(menu, item,
                                         by_position ? MF_BYPOSITION
                                                     : MF_BYCOMMAND);
    if (index < 0) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    const int ok = apply_item_info(&menu->items[index], &info);
    pthread_mutex_unlock(&g_lock);
    c->eax = ok ? 1u : 0u;
}

static void h_get_menu_item_info(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t item = wc3_legacy_arg32(c, 1u);
    const uint32_t by_position = wc3_legacy_arg32(c, 2u);
    const uint32_t info_address = wc3_legacy_arg32(c, 3u);
    MenuItemInfo info;

    if (!read_menu_item_info(info_address, &info)) {
        c->eax = 0u;
        return;
    }

    pthread_mutex_lock(&g_lock);
    MenuObject *menu = find_menu(handle);
    if (!menu) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    const int index = resolve_item_index(menu, item,
                                         by_position ? MF_BYPOSITION
                                                     : MF_BYCOMMAND);
    if (index < 0) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    const MenuItem *source = &menu->items[index];

    if (info.mask & (MIIM_FTYPE | MIIM_TYPE))
        write_u32(info_address + 8u, source->type);
    if (info.mask & MIIM_STATE)
        write_u32(info_address + 12u, source->state);
    if (info.mask & MIIM_ID)
        write_u32(info_address + 16u, source->id);
    if (info.mask & MIIM_SUBMENU)
        write_u32(info_address + 20u, source->submenu);
    if (info.mask & MIIM_CHECKMARKS) {
        write_u32(info_address + 24u, source->bitmap_checked);
        write_u32(info_address + 28u, source->bitmap_unchecked);
    }
    if (info.mask & MIIM_DATA)
        write_u32(info_address + 32u, source->item_data);
    if ((info.mask & MIIM_BITMAP) &&
        info.cb_size >= MENUITEMINFO_SIZE)
        write_u32(info_address + 44u, source->bitmap_item);

    int success = 1;
    if (info.mask & (MIIM_TYPE | MIIM_STRING)) {
        if (source->type & MFT_BITMAP) {
            if (info.mask & MIIM_TYPE)
                write_u32(info_address + 36u, source->bitmap_item);
            write_u32(info_address + 40u, 0u);
        } else {
            const uint32_t length = (uint32_t)strlen(source->text);
            const uint32_t required = length + 1u;
            if (!info.text_address) {
                write_u32(info_address + 40u, required);
            } else if (info.text_capacity < required) {
                write_u32(info_address + 40u, required);
                success = 0;
            } else if (!wc3_legacy_guest_ok(info.text_address,
                                            info.text_capacity)) {
                success = 0;
            } else {
                write_guest_string(info.text_address, source->text,
                                   info.text_capacity);
                write_u32(info_address + 40u, length);
            }
        }
    }

    pthread_mutex_unlock(&g_lock);
    c->eax = success ? 1u : 0u;
}

static void h_delete_menu(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t item = wc3_legacy_arg32(c, 1u);
    const uint32_t flags = wc3_legacy_arg32(c, 2u);

    pthread_mutex_lock(&g_lock);
    MenuObject *menu = find_menu(handle);
    if (!menu) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    const int index = resolve_item_index(menu, item, flags);
    if (index < 0) {
        pthread_mutex_unlock(&g_lock);
        c->eax = 0u;
        return;
    }
    if (menu->items[index].submenu)
        destroy_menu_locked(menu->items[index].submenu, 1u);
    if ((unsigned)index + 1u < menu->count) {
        memmove(&menu->items[index], &menu->items[index + 1u],
                (menu->count - (unsigned)index - 1u) * sizeof menu->items[0]);
    }
    menu->count--;
    memset(&menu->items[menu->count], 0, sizeof menu->items[0]);
    pthread_mutex_unlock(&g_lock);
    c->eax = 1u;
}

static void h_track_popup_menu(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    pthread_mutex_lock(&g_lock);
    const int valid = find_menu(handle) != NULL;
    pthread_mutex_unlock(&g_lock);
    (void)valid;
    /* No host pointer can select a guest menu item.  Win32 returns zero for
     * cancellation, which is also what the as-built stub did. */
    c->eax = 0u;
}

typedef struct MenuOverride {
    const char *name;
    void (*impl)(CPU *c);
} MenuOverride;

static const MenuOverride g_overrides[] = {
    { "CreateMenu", h_create_menu },
    { "CreatePopupMenu", h_create_menu },
    { "DestroyMenu", h_destroy_menu },
    { "GetMenuItemCount", h_get_menu_item_count },
    { "GetMenu", h_get_menu },
};

typedef struct MenuAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} MenuAddition;

static const MenuAddition g_additions[] = {
    { "SetMenu", 2u, h_set_menu },
    { "DrawMenuBar", 1u, h_draw_menu_bar },
    { "InsertMenuItemA", 4u, h_insert_menu_item },
    { "SetMenuItemInfoA", 4u, h_set_menu_item_info },
    { "GetMenuItemInfoA", 4u, h_get_menu_item_info },
    { "DeleteMenu", 3u, h_delete_menu },
    { "TrackPopupMenu", 7u, h_track_popup_menu },
};

int wc3_hle_user32_menu_install(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_menus, 0, sizeof g_menus);
    memset(g_bindings, 0, sizeof g_bindings);
    pthread_mutex_unlock(&g_lock);

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("USER32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
