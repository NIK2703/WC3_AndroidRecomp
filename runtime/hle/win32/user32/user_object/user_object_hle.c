/*
 * user_object_hle.c - target-side USER32 window-station and desktop facts.
 *
 * The game asks for the process window station and then queries UOI_NAME with
 * the standard size-query pattern: pvInfo = NULL, nLength = 0, and expects
 * FALSE with ERROR_INSUFFICIENT_BUFFER plus the required byte count.  Keeping
 * the exact WinSta0/Default strings and that failure contract is enough for the
 * Android desktop model; no host window station is created.
 */
#include "user_object_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define WINDOW_STATION_HANDLE 0x00170000u
#define DESKTOP_HANDLE 0x00170004u

#define UOI_FLAGS 1u
#define UOI_NAME 2u
#define UOI_TYPE 3u
#define UOI_USER_SID 4u
#define UOI_HEAPSIZE 5u
#define UOI_IO 6u

#define ERROR_INVALID_HANDLE 6u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u

#define WSF_VISIBLE 0x00000001u

static const uint16_t g_window_station_name[] = {
    'W', 'i', 'n', 'S', 't', 'a', '0', 0,
};

static const uint16_t g_desktop_name[] = {
    'D', 'e', 'f', 'a', 'u', 'l', 't', 0,
};

static const uint16_t g_window_station_type[] = {
    'W', 'i', 'n', 'd', 'o', 'w', 'S', 't', 'a', 't', 'i', 'o', 'n', 0,
};

static const uint16_t g_desktop_type[] = {
    'D', 'e', 's', 'k', 't', 'o', 'p', 0,
};

static uint32_t utf16_bytes(const uint16_t *text)
{
    uint32_t length = 0;
    while (text[length]) length++;
    return (length + 1u) * 2u;
}

static int write_utf16(uint32_t destination, uint32_t capacity,
                       const uint16_t *text)
{
    const uint32_t required = utf16_bytes(text);
    if (capacity < required || !wc3_legacy_guest_ok(destination, required))
        return 0;
    for (uint32_t i = 0; i < required / 2u; i++)
        wc3_legacy_wr16g(destination + i * 2u, text[i]);
    return 1;
}

static int is_user_object(uint32_t handle)
{
    return handle == WINDOW_STATION_HANDLE || handle == DESKTOP_HANDLE;
}

static void fail_with(CPU *c, uint32_t error)
{
    wc3_legacy_set_last_error(error);
    c->eax = 0u;
}

static void h_get_process_window_station(CPU *c)
{
    (void)c;
    c->eax = WINDOW_STATION_HANDLE;
}

static void h_get_user_object_information(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t index = wc3_legacy_arg32(c, 1u);
    const uint32_t information = wc3_legacy_arg32(c, 2u);
    const uint32_t length = wc3_legacy_arg32(c, 3u);
    const uint32_t needed_pointer = wc3_legacy_arg32(c, 4u);

    if (!is_user_object(handle)) {
        fail_with(c, ERROR_INVALID_HANDLE);
        return;
    }
    if (!needed_pointer || !wc3_legacy_guest_ok(needed_pointer, 4u)) {
        fail_with(c, ERROR_INVALID_PARAMETER);
        return;
    }

    const int is_station = handle == WINDOW_STATION_HANDLE;
    const uint16_t *name = is_station ? g_window_station_name
                                      : g_desktop_name;
    const uint16_t *type = is_station ? g_window_station_type
                                      : g_desktop_type;

    if (index == UOI_NAME || index == UOI_TYPE) {
        const uint16_t *text = index == UOI_NAME ? name : type;
        const uint32_t required = utf16_bytes(text);
        if (!information || length < required) {
            wc3_legacy_wr32g(needed_pointer, required);
            fail_with(c, ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        if (!write_utf16(information, length, text)) {
            fail_with(c, ERROR_INVALID_PARAMETER);
            return;
        }
        c->eax = 1u;
        return;
    }

    if (index == UOI_FLAGS) {
        const uint32_t required = 12u;
        if (!information || length < required) {
            wc3_legacy_wr32g(needed_pointer, required);
            fail_with(c, ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        if (!wc3_legacy_guest_ok(information, required)) {
            fail_with(c, ERROR_INVALID_PARAMETER);
            return;
        }
        wc3_legacy_wr32g(information + 0u, 0u);
        wc3_legacy_wr32g(information + 4u, 0u);
        wc3_legacy_wr32g(information + 8u,
                         is_station ? WSF_VISIBLE : 0u);
        c->eax = 1u;
        return;
    }

    if (index == UOI_HEAPSIZE) {
        const uint32_t required = 4u;
        if (!information || length < required) {
            wc3_legacy_wr32g(needed_pointer, required);
            fail_with(c, ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        if (!wc3_legacy_guest_ok(information, required)) {
            fail_with(c, ERROR_INVALID_PARAMETER);
            return;
        }
        wc3_legacy_wr32g(information, 0u);
        c->eax = 1u;
        return;
    }

    if (index == UOI_IO) {
        const uint32_t required = 4u;
        if (!information || length < required) {
            wc3_legacy_wr32g(needed_pointer, required);
            fail_with(c, ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        if (!wc3_legacy_guest_ok(information, required)) {
            fail_with(c, ERROR_INVALID_PARAMETER);
            return;
        }
        wc3_legacy_wr32g(information, 0u);
        c->eax = 1u;
        return;
    }

    if (index == UOI_USER_SID) {
        fail_with(c, ERROR_INVALID_PARAMETER);
        return;
    }

    fail_with(c, ERROR_INVALID_PARAMETER);
}

typedef struct UserObjectAddition {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} UserObjectAddition;

static const UserObjectAddition g_additions[] = {
    { "GetProcessWindowStation", 0, h_get_process_window_station },
    { "GetUserObjectInformationW", 5, h_get_user_object_information },
};

int wc3_hle_user32_user_object_install(void)
{
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("USER32.dll", g_additions[i].name,
                               g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
