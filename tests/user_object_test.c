/*
 * user_object_test.c - T0 contract test for window-station information.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "user_object_hle.h"

#define MAX_ROWS 8u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u

typedef struct AddRow {
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} AddRow;

static AddRow g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[8];
static uint8_t g_memory[GUEST_SIZE];
static uint32_t g_last_error;

static void fail_with_error(uint32_t error)
{
    g_last_error = error;
}

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)dll;
    (void)cdecl_abi;
    if (g_addition_count >= MAX_ROWS) return -1;
    g_additions[g_addition_count].name = name;
    g_additions[g_addition_count].argc = argc;
    g_additions[g_addition_count].impl = impl;
    g_addition_count++;
    return 0;
}

void wc3_legacy_set_last_error(uint32_t code)
{
    fail_with_error(code);
}

uint32_t wc3_legacy_get_last_error(void)
{
    return g_last_error;
}

uint32_t wc3_legacy_arg32(const CPU *c, unsigned index)
{
    (void)c;
    return index < 8u ? g_args[index] : 0u;
}

int wc3_legacy_guest_ok(uint32_t address, uint32_t size)
{
    if (size == 0u) return 1;
    if (address < GUEST_BASE) return 0;
    const uint64_t end = (uint64_t)address + size;
    return end <= (uint64_t)GUEST_BASE + GUEST_SIZE;
}

void wc3_legacy_wr16g(uint32_t address, uint16_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

static uint32_t guest(uint32_t offset)
{
    return GUEST_BASE + offset;
}

static uint32_t get_u32(uint32_t offset)
{
    uint32_t value;
    memcpy(&value, g_memory + offset, sizeof value);
    return value;
}

static uint16_t get_u16(uint32_t offset)
{
    uint16_t value;
    memcpy(&value, g_memory + offset, sizeof value);
    return value;
}

static void (*find_impl(const char *name, int *argc))(CPU *c)
{
    for (unsigned i = 0; i < g_addition_count; i++) {
        if (strcmp(g_additions[i].name, name) == 0) {
            if (argc) *argc = g_additions[i].argc;
            return g_additions[i].impl;
        }
    }
    return NULL;
}

static uint32_t invoke(const char *name, const uint32_t *args, unsigned count)
{
    void (*impl)(CPU *c) = find_impl(name, NULL);
    if (!impl) return 0xFFFFFFFEu;
    for (unsigned i = 0; i < 8u; i++) g_args[i] = 0u;
    for (unsigned i = 0; i < count && i < 8u; i++) g_args[i] = args[i];
    CPU c;
    memset(&c, 0, sizeof c);
    impl(&c);
    return c.eax;
}

static int string_is(uint32_t offset, const char *ascii)
{
    for (size_t i = 0;; i++) {
        if (get_u16(offset + (uint32_t)i * 2u) != (uint16_t)ascii[i])
            return 0;
        if (!ascii[i]) return 1;
    }
}

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_user32_user_object_install();
    CHECK(installed == 2, "install count");
    CHECK(g_addition_count == 2u, "addition count");

    int argc = -1;
    CHECK(find_impl("GetProcessWindowStation", &argc) != NULL,
          "window station import");
    CHECK(argc == 0, "window station argc");
    CHECK(find_impl("GetUserObjectInformationW", &argc) != NULL,
          "user object import");
    CHECK(argc == 5, "user object argc");

    const uint32_t station = 0x00170000u;
    const uint32_t desktop = 0x00170004u;
    const uint32_t needed = guest(0x1000u);
    const uint32_t buffer = guest(0x2000u);

    g_last_error = 0;
    CHECK(invoke("GetProcessWindowStation", NULL, 0u) == station,
          "process window station");
    CHECK(g_last_error == 0u, "station does not change error");

    const uint32_t query_name[5] = {
        station, 2u, 0u, 0u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", query_name, 5u) == 0u,
          "name size query fails");
    CHECK(get_u32(0x1000u) == 16u, "station name required bytes");
    CHECK(g_last_error == 122u, "size query insufficient buffer");

    g_last_error = 0;
    const uint32_t copy_name[5] = {
        station, 2u, buffer, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", copy_name, 5u) == 1u,
          "copy station name");
    CHECK(string_is(0x2000u, "WinSta0"), "station name");
    CHECK(g_last_error == 0u, "success leaves error alone");

    g_last_error = 0;
    const uint32_t short_name[5] = {
        station, 2u, buffer, 15u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", short_name, 5u) == 0u,
          "short station name fails");
    CHECK(get_u32(0x1000u) == 16u, "short name required bytes");
    CHECK(g_last_error == 122u, "short name insufficient buffer");

    g_last_error = 0;
    const uint32_t null_name[5] = {
        station, 2u, 0u, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", null_name, 5u) == 0u,
          "null name buffer fails");
    CHECK(get_u32(0x1000u) == 16u, "null name required bytes");
    CHECK(g_last_error == 122u, "null name insufficient buffer");

    g_last_error = 0;
    const uint32_t query_type[5] = {
        station, 3u, 0u, 0u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", query_type, 5u) == 0u,
          "type size query fails");
    CHECK(get_u32(0x1000u) == 28u, "station type bytes");
    CHECK(g_last_error == 122u, "type query insufficient buffer");

    g_last_error = 0;
    const uint32_t copy_type[5] = {
        station, 3u, buffer, 28u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", copy_type, 5u) == 1u,
          "copy station type");
    CHECK(string_is(0x2000u, "WindowStation"), "station type");

    g_last_error = 0;
    const uint32_t desktop_name[5] = {
        desktop, 2u, buffer, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", desktop_name, 5u) == 1u,
          "copy desktop name");
    CHECK(string_is(0x2000u, "Default"), "desktop name");

    g_last_error = 0;
    const uint32_t flags[5] = {
        station, 1u, buffer, 12u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", flags, 5u) == 1u,
          "station flags");
    CHECK(get_u32(0x2000u) == 0u, "flags fInherit");
    CHECK(get_u32(0x2004u) == 0u, "flags fReserved");
    CHECK(get_u32(0x2008u) == 1u, "flags WSF_VISIBLE");
    CHECK(g_last_error == 0u, "flags success leaves error");

    g_last_error = 0;
    const uint32_t short_flags[5] = {
        station, 1u, buffer, 11u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", short_flags, 5u) == 0u,
          "short flags fail");
    CHECK(get_u32(0x1000u) == 12u, "flags required bytes");
    CHECK(g_last_error == 122u, "flags insufficient buffer");

    g_last_error = 0;
    const uint32_t bad_flags_pointer[5] = {
        station, 1u, guest(0xFFFEu), 12u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", bad_flags_pointer, 5u) == 0u,
          "invalid flags pointer");
    CHECK(g_last_error == 87u, "invalid flags pointer error");

    g_last_error = 0;
    const uint32_t bad_handle[5] = {
        0x00170008u, 2u, buffer, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", bad_handle, 5u) == 0u,
          "invalid user object");
    CHECK(g_last_error == 6u, "invalid handle error");

    g_last_error = 0;
    const uint32_t bad_index[5] = {
        station, 99u, buffer, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", bad_index, 5u) == 0u,
          "invalid information index");
    CHECK(g_last_error == 87u, "invalid index error");

    g_last_error = 0;
    const uint32_t null_needed[5] = {
        station, 2u, buffer, 16u, 0u,
    };
    CHECK(invoke("GetUserObjectInformationW", null_needed, 5u) == 0u,
          "null needed pointer");
    CHECK(g_last_error == 87u, "null needed error");

    g_last_error = 0;
    const uint32_t invalid_needed[5] = {
        station, 2u, buffer, 16u, guest(0xFFFEu),
    };
    CHECK(invoke("GetUserObjectInformationW", invalid_needed, 5u) == 0u,
          "invalid needed pointer");
    CHECK(g_last_error == 87u, "invalid needed error");

    g_last_error = 0;
    const uint32_t heap_size[5] = {
        station, 5u, buffer, 4u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", heap_size, 5u) == 1u,
          "heap size query");
    CHECK(get_u32(0x2000u) == 0u, "heap size value");

    g_last_error = 0;
    const uint32_t io_query[5] = {
        station, 6u, buffer, 4u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", io_query, 5u) == 1u,
          "io query");
    CHECK(get_u32(0x2000u) == 0u, "io value");

    g_last_error = 0;
    const uint32_t user_sid[5] = {
        station, 4u, buffer, 16u, needed,
    };
    CHECK(invoke("GetUserObjectInformationW", user_sid, 5u) == 0u,
          "user sid is unavailable");
    CHECK(g_last_error == 87u, "user sid error");

    printf("user object OK (%d handlers)\n", installed);
    return 0;
}
