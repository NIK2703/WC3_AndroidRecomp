/*
 * gdi_device_test.c - T0 contract test for GDI gamma ramps and device caps.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gdi_device_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 16u
#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x10000u

typedef struct Row {
    const char *dll;
    const char *name;
    int argc;
    void (*impl)(CPU *c);
} Row;

static Row g_additions[MAX_ROWS];
static unsigned g_addition_count;
static uint32_t g_args[8];
static uint8_t g_memory[GUEST_SIZE];

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)cdecl_abi;
    if (g_addition_count >= MAX_ROWS) return -1;
    g_additions[g_addition_count].dll = dll;
    g_additions[g_addition_count].name = name;
    g_additions[g_addition_count].argc = argc;
    g_additions[g_addition_count].impl = impl;
    g_addition_count++;
    return 0;
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

uint8_t wc3_legacy_rd8g(uint32_t address)
{
    return g_memory[address - GUEST_BASE];
}

uint16_t wc3_legacy_rd16g(uint32_t address)
{
    uint16_t value;
    memcpy(&value, g_memory + address - GUEST_BASE, sizeof value);
    return value;
}

uint32_t wc3_legacy_rd32g(uint32_t address)
{
    uint32_t value;
    memcpy(&value, g_memory + address - GUEST_BASE, sizeof value);
    return value;
}

void wc3_legacy_wr8g(uint32_t address, uint8_t value)
{
    g_memory[address - GUEST_BASE] = value;
}

void wc3_legacy_wr16g(uint32_t address, uint16_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    memcpy(g_memory + address - GUEST_BASE, &value, sizeof value);
}

int wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (width) *width = 1280u;
    if (height) *height = 720u;
    return 1;
}

static uint32_t guest(uint32_t offset)
{
    return GUEST_BASE + offset;
}

static void put_u16(uint32_t offset, uint16_t value)
{
    memcpy(g_memory + offset, &value, sizeof value);
}

static uint16_t get_u16(uint32_t offset)
{
    uint16_t value;
    memcpy(&value, g_memory + offset, sizeof value);
    return value;
}

static void fill_ramp(uint32_t offset, uint16_t seed)
{
    for (uint32_t channel = 0; channel < 3u; channel++) {
        for (uint32_t i = 0; i < 256u; i++) {
            put_u16(offset + (channel * 256u + i) * 2u,
                    (uint16_t)(seed + channel * 1000u + i));
        }
    }
}

static void fill_linear(uint32_t offset)
{
    for (uint32_t channel = 0; channel < 3u; channel++) {
        for (uint32_t i = 0; i < 256u; i++)
            put_u16(offset + (channel * 256u + i) * 2u,
                    (uint16_t)(i * 257u));
    }
}

static int ramps_equal(uint32_t a, uint32_t b)
{
    for (uint32_t i = 0; i < 768u; i++) {
        if (get_u16(a + i * 2u) != get_u16(b + i * 2u)) return 0;
    }
    return 1;
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

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    const int installed = wc3_hle_gdi32_device_install();
    CHECK(installed == 3, "install count");
    CHECK(g_addition_count == 3u, "addition count");

    const char *names[] = {
        "SetDeviceGammaRamp", "GetDeviceGammaRamp", "GetDeviceCaps",
    };
    const int argc[] = { 2, 2, 2 };
    for (unsigned i = 0; i < 3u; i++) {
        int actual = -1;
        CHECK(find_impl(names[i], &actual) != NULL, names[i]);
        CHECK(actual == argc[i], "argc");
    }

    const uint32_t hdc1 = 0x00030000u;
    const uint32_t hdc2 = 0x00030004u;
    const uint32_t ramp1 = guest(0x2000u);
    const uint32_t ramp2 = guest(0x4000u);
    const uint32_t output = guest(0x6000u);

    const uint32_t set_bad_hdc[2] = { 0u, ramp1 };
    CHECK(invoke("SetDeviceGammaRamp", set_bad_hdc, 2u) == 0u,
          "set null hdc");
    const uint32_t set_bad_ramp[2] = { hdc1, 0u };
    CHECK(invoke("SetDeviceGammaRamp", set_bad_ramp, 2u) == 0u,
          "set null ramp");
    const uint32_t set_invalid_ramp[2] = { hdc1, guest(0x10000u) };
    CHECK(invoke("SetDeviceGammaRamp", set_invalid_ramp, 2u) == 0u,
          "set invalid ramp");

    fill_ramp(0x2000u, 0x0100u);
    const uint32_t set1[2] = { hdc1, ramp1 };
    CHECK(invoke("SetDeviceGammaRamp", set1, 2u) == 1u, "set ramp");

    const uint32_t get1[2] = { hdc1, output };
    CHECK(invoke("GetDeviceGammaRamp", get1, 2u) == 1u, "get ramp");
    CHECK(ramps_equal(0x2000u, 0x6000u), "round trip");

    const uint32_t get2[2] = { hdc2, output };
    CHECK(invoke("GetDeviceGammaRamp", get2, 2u) == 1u, "get default ramp");
    fill_linear(0x8000u);
    CHECK(ramps_equal(0x8000u, 0x6000u), "default ramp is linear");

    fill_ramp(0x4000u, 0x3000u);
    const uint32_t set2[2] = { hdc1, ramp2 };
    CHECK(invoke("SetDeviceGammaRamp", set2, 2u) == 1u, "overwrite ramp");
    CHECK(invoke("GetDeviceGammaRamp", get1, 2u) == 1u, "get updated ramp");
    CHECK(ramps_equal(0x4000u, 0x6000u), "updated ramp round trip");

    const uint32_t get_bad_hdc[2] = { 0u, output };
    CHECK(invoke("GetDeviceGammaRamp", get_bad_hdc, 2u) == 0u,
          "get null hdc");
    const uint32_t get_bad_ramp[2] = { hdc1, 0u };
    CHECK(invoke("GetDeviceGammaRamp", get_bad_ramp, 2u) == 0u,
          "get null ramp");

    const uint32_t caps_hdc[2] = { hdc1, 8u };
    CHECK(invoke("GetDeviceCaps", caps_hdc, 2u) == 1280u, "horzres");
    const uint32_t caps_height[2] = { hdc1, 10u };
    CHECK(invoke("GetDeviceCaps", caps_height, 2u) == 720u, "vertres");
    const uint32_t caps_bits[2] = { hdc1, 12u };
    CHECK(invoke("GetDeviceCaps", caps_bits, 2u) == 32u, "bitspixel");
    const uint32_t caps_planes[2] = { hdc1, 14u };
    CHECK(invoke("GetDeviceCaps", caps_planes, 2u) == 1u, "planes");
    const uint32_t caps_tech[2] = { hdc1, 2u };
    CHECK(invoke("GetDeviceCaps", caps_tech, 2u) == 1u, "technology");
    const uint32_t caps_colors[2] = { hdc1, 24u };
    CHECK(invoke("GetDeviceCaps", caps_colors, 2u) == 0xFFFFFFFFu,
          "numcolors");
    const uint32_t caps_dpi[2] = { hdc1, 88u };
    CHECK(invoke("GetDeviceCaps", caps_dpi, 2u) == 96u, "logpixelsx");
    const uint32_t caps_dpi_y[2] = { hdc1, 90u };
    CHECK(invoke("GetDeviceCaps", caps_dpi_y, 2u) == 96u, "logpixelsy");
    const uint32_t caps_unknown[2] = { hdc1, 999u };
    CHECK(invoke("GetDeviceCaps", caps_unknown, 2u) == 0u, "unknown cap");
    const uint32_t caps_zero[2] = { 0u, 12u };
    CHECK(invoke("GetDeviceCaps", caps_zero, 2u) == 0u, "null hdc cap");

    printf("gdi device OK (%d handlers)\n", installed);
    return 0;
}
