/*
 * user32_display_test.c - T0 contract test for the USER32 display slice.
 *
 * The test links display_hle.c against a fake shim and a small guest mapping.
 * It checks the DEVMODEA/DISPLAY_DEVICEA writes, the current/registry
 * sentinels, the device-relative mode table, and the distinct A/Ex argument
 * layouts of ChangeDisplaySettings.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "display_hle.h"
#include "legacy_shim.h"

#define MAX_ROWS 8u
#define GUEST_BASE 0x20000u
#define GUEST_SIZE 0x10000u
#define ARG_ESP (GUEST_BASE + 0x100u)
#define DM_ADDR (GUEST_BASE + 0x1000u)
#define DEV_ADDR (GUEST_BASE + 0x2000u)

#define DEVMODEA_SIZE 156u
#define DM_FIELDS_OFFSET 40u
#define DM_BPP_OFFSET 104u
#define DM_WIDTH_OFFSET 108u
#define DM_HEIGHT_OFFSET 112u
#define DM_FREQ_OFFSET 120u
#define DM_FIELDS (0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u)
#define DISP_CHANGE_BADMODE 0xFFFFFFFEu

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
} Row;

typedef struct Mode {
    uint32_t width;
    uint32_t height;
} Mode;

static Row g_rows[MAX_ROWS];
static unsigned g_row_count;
static uint8_t g_mem[GUEST_SIZE];

static const Mode g_modes[] = {
    { 640, 480 },
    { 800, 600 },
    { 1024, 768 },
};

static uint32_t g_refresh_hz = 60;
static int g_current_ok = 1;
static uint32_t g_current_w = 1024, g_current_h = 768;
static uint32_t g_default_w = 800, g_default_h = 600;
static unsigned g_select_count;
static uint32_t g_selected_w, g_selected_h;

static void *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    const uint32_t off = addr - GUEST_BASE;
    if (off > GUEST_SIZE || size > GUEST_SIZE - off) return NULL;
    return g_mem + off;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

void wc3_legacy_guest_zero(uint32_t addr, uint32_t size)
{
    void *p = guest_ptr(addr, size);
    if (p) memset(p, 0, size);
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint32_t value = 0;
    const void *p = guest_ptr(addr, 4);
    if (p) memcpy(&value, p, 4);
    return value;
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    void *p = guest_ptr(addr, 4);
    if (p) memcpy(p, &value, 4);
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    void *p = guest_ptr(addr, 2);
    if (p) memcpy(p, &value, 2);
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    void *p = guest_ptr(addr, 1);
    if (p) memcpy(p, &value, 1);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

int wc3_legacy_hle_override(const char *dll, const char *name,
                            void (*impl)(CPU *c))
{
    if (!dll || !name || !impl || g_row_count >= MAX_ROWS) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_row_count++;
    return 0;
}

int display_mode_at(uint32_t index, uint32_t *width, uint32_t *height)
{
    if (index >= sizeof g_modes / sizeof g_modes[0]) return 0;
    if (width) *width = g_modes[index].width;
    if (height) *height = g_modes[index].height;
    return 1;
}

int display_mode_supported(uint32_t width, uint32_t height)
{
    for (unsigned i = 0; i < sizeof g_modes / sizeof g_modes[0]; i++)
        if (g_modes[i].width == width && g_modes[i].height == height)
            return 1;
    return 0;
}

uint32_t display_refresh_hz(void)
{
    return g_refresh_hz;
}

int wc3_legacy_current_display_mode(uint32_t *width, uint32_t *height)
{
    if (!g_current_ok) return 0;
    if (width) *width = g_current_w;
    if (height) *height = g_current_h;
    return 1;
}

int wc3_legacy_default_display_mode(uint32_t *width, uint32_t *height)
{
    if (width) *width = g_default_w;
    if (height) *height = g_default_h;
    return 1;
}

int wc3_legacy_select_display_mode(uint32_t width, uint32_t height)
{
    g_select_count++;
    g_selected_w = width;
    g_selected_h = height;
    return display_mode_supported(width, height);
}

static void (*find_impl(const char *name))(CPU *c)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].name, name) == 0) return g_rows[i].impl;
    return NULL;
}

static uint32_t invoke(const char *name, const uint32_t *args, unsigned count)
{
    void (*impl)(CPU *c) = find_impl(name);
    if (!impl) {
        printf("missing handler: %s\n", name);
        return 0xFFFFFFFFu;
    }
    for (unsigned i = 0; i < count; i++)
        wc3_legacy_wr32g(ARG_ESP + 4u + i * 4u, args[i]);
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = ARG_ESP;
    impl(&c);
    return c.eax;
}

static void set_mode(uint32_t width, uint32_t height)
{
    wc3_legacy_wr32g(DM_ADDR + DM_WIDTH_OFFSET, width);
    wc3_legacy_wr32g(DM_ADDR + DM_HEIGHT_OFFSET, height);
}

static int32_t as_i32(uint32_t value) { return (int32_t)value; }

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            printf("FAIL: %s\n", message);                                \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int check_devmode(uint32_t width, uint32_t height)
{
    CHECK((uint16_t)wc3_legacy_rd32g(DM_ADDR + 36u) == DEVMODEA_SIZE,
          "dmSize");
    CHECK(wc3_legacy_rd32g(DM_ADDR + DM_FIELDS_OFFSET) == DM_FIELDS,
          "dmFields");
    CHECK(wc3_legacy_rd32g(DM_ADDR + DM_BPP_OFFSET) == 32u, "bits per pel");
    CHECK(wc3_legacy_rd32g(DM_ADDR + DM_WIDTH_OFFSET) == width,
          "dmPelsWidth");
    CHECK(wc3_legacy_rd32g(DM_ADDR + DM_HEIGHT_OFFSET) == height,
          "dmPelsHeight");
    CHECK(wc3_legacy_rd32g(DM_ADDR + DM_FREQ_OFFSET) == g_refresh_hz,
          "dmDisplayFrequency");
    return 0;
}

int main(void)
{
    const int installed = user32_display_install();
    CHECK(installed == 5, "install count");
    CHECK(g_row_count == 5u, "row count");
    CHECK(find_impl("EnumDisplaySettingsA") != NULL,
          "EnumDisplaySettingsA row");
    CHECK(find_impl("EnumDisplaySettingsExA") != NULL,
          "EnumDisplaySettingsExA row");
    CHECK(find_impl("EnumDisplayDevicesA") != NULL,
          "EnumDisplayDevicesA row");
    CHECK(find_impl("ChangeDisplaySettingsA") != NULL,
          "ChangeDisplaySettingsA row");
    CHECK(find_impl("ChangeDisplaySettingsExA") != NULL,
          "ChangeDisplaySettingsExA row");
    for (unsigned i = 0; i < g_row_count; i++)
        CHECK(strcmp(g_rows[i].dll, "USER32.dll") == 0, "DLL name");

    /* ENUM_CURRENT_SETTINGS reports the selected mode. */
    memset(guest_ptr(DM_ADDR, DEVMODEA_SIZE), 0xAA, DEVMODEA_SIZE);
    {
        const uint32_t args[] = { 0, 0xFFFFFFFFu, DM_ADDR };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 1u,
              "settings current");
    }
    CHECK(check_devmode(1024, 768) == 0, "current DEVMODEA");
    CHECK(wc3_legacy_rd32g(DM_ADDR) == 0u, "dmDeviceName zeroed");
    CHECK(wc3_legacy_rd32g(DM_ADDR + 44u) == 0u, "dmPosition zeroed");

    /* ENUM_REGISTRY_SETTINGS and a current-mode failure use the default. */
    g_current_ok = 0;
    {
        const uint32_t args[] = { 0, 0xFFFFFFFEu, DM_ADDR };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 1u,
              "settings registry");
    }
    CHECK(check_devmode(800, 600) == 0, "registry DEVMODEA");
    g_current_ok = 1;

    /* Indexed enumeration reads the device-relative table. */
    {
        const uint32_t args[] = { 0, 1u, DM_ADDR };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 1u,
              "settings index 1");
    }
    CHECK(check_devmode(800, 600) == 0, "indexed DEVMODEA");

    /* The Ex form keeps mode and DEVMODE in the same positions. */
    g_refresh_hz = 75;
    {
        const uint32_t args[] = { 0, 0xFFFFFFFFu, DM_ADDR, 1u };
        CHECK(invoke("EnumDisplaySettingsExA", args, 4) == 1u,
              "settings Ex current");
    }
    CHECK(check_devmode(1024, 768) == 0, "Ex DEVMODEA");
    g_refresh_hz = 60;

    /* An unknown index fails without touching the caller's buffer. */
    wc3_legacy_wr32g(DM_ADDR, 0x5A5A5A5Au);
    {
        const uint32_t args[] = { 0, 7u, DM_ADDR };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 0u,
              "settings unknown index");
    }
    CHECK(wc3_legacy_rd32g(DM_ADDR) == 0x5A5A5A5Au,
          "unknown index preserves buffer");

    {
        const uint32_t args[] = { 0, 0u, 0u };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 0u,
              "settings null DEVMODE");
    }
    {
        const uint32_t args[] = { 0, 0u, GUEST_BASE + GUEST_SIZE - 8u };
        CHECK(invoke("EnumDisplaySettingsA", args, 3) == 0u,
              "settings short DEVMODE");
    }

    /* Adapter enumeration: only index 0 exists, name at the as-built offset. */
    memset(guest_ptr(DEV_ADDR, 32), 0xCC, 32);
    {
        const uint32_t args[] = { 0, 0u, DEV_ADDR, 0u };
        CHECK(invoke("EnumDisplayDevicesA", args, 4) == 1u,
              "devices index 0");
    }
    CHECK(memcmp(guest_ptr(DEV_ADDR, 13), "\\\\.\\DISPLAY1", 13) == 0,
          "device name");
    for (unsigned i = 13; i < 32; i++)
        CHECK(((const uint8_t *)guest_ptr(DEV_ADDR, 32))[i] == 0u,
              "device tail");
    {
        const uint32_t args[] = { 0, 1u, DEV_ADDR, 0u };
        CHECK(invoke("EnumDisplayDevicesA", args, 4) == 0u,
              "devices index 1");
    }
    {
        const uint32_t args[] = { 0, 0u, GUEST_BASE + GUEST_SIZE - 8u, 0u };
        CHECK(invoke("EnumDisplayDevicesA", args, 4) == 0u,
              "devices short buffer");
    }

    /* ChangeDisplaySettingsA: apply, test-only, and rejection paths. */
    set_mode(800, 600);
    {
        const uint32_t args[] = { DM_ADDR, 0u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) == 0u,
              "change A apply");
    }
    CHECK(g_select_count == 1u && g_selected_w == 800u &&
              g_selected_h == 600u,
          "change A selected mode");

    {
        const uint32_t args[] = { DM_ADDR, 2u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) == 0u,
              "change A test");
    }
    CHECK(g_select_count == 1u, "CDS_TEST does not select");

    set_mode(1280, 720);
    {
        const uint32_t args[] = { DM_ADDR, 0u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) ==
                  DISP_CHANGE_BADMODE,
              "change A unsupported");
    }
    CHECK(g_select_count == 1u, "unsupported does not select");

    set_mode(0, 768);
    {
        const uint32_t args[] = { DM_ADDR, 0u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) == 0u,
              "change A zero width");
    }
    CHECK(g_select_count == 1u, "zero width does not select");

    {
        const uint32_t args[] = { 0u, 0u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) == 0u,
              "change A null DEVMODE");
    }
    {
        const uint32_t args[] = { GUEST_BASE + GUEST_SIZE - 8u, 0u };
        CHECK(invoke("ChangeDisplaySettingsA", args, 2) ==
                  DISP_CHANGE_BADMODE,
              "change A short DEVMODE");
    }
    CHECK(g_select_count == 1u, "invalid DEVMODE does not select");

    /* ChangeDisplaySettingsExA reads lpDevMode at arg 1 and dwFlags at arg 3.
     * The A-shaped as-built handler read arg 0/1 and could never succeed. */
    set_mode(1024, 768);
    {
        const uint32_t args[] = { 0xDEADBEEFu, DM_ADDR, 0u, 0u, 0u };
        CHECK(invoke("ChangeDisplaySettingsExA", args, 5) == 0u,
              "change Ex apply");
    }
    CHECK(g_select_count == 2u && g_selected_w == 1024u &&
              g_selected_h == 768u,
          "change Ex selected mode");

    set_mode(800, 600);
    {
        const uint32_t args[] = { 0u, DM_ADDR, 0u, 2u, 0u };
        CHECK(invoke("ChangeDisplaySettingsExA", args, 5) == 0u,
              "change Ex test");
    }
    CHECK(g_select_count == 2u, "Ex CDS_TEST does not select");

    {
        const uint32_t args[] = { DM_ADDR, GUEST_BASE + GUEST_SIZE - 8u, 0u,
                                  0u, 0u };
        CHECK(invoke("ChangeDisplaySettingsExA", args, 5) ==
                  DISP_CHANGE_BADMODE,
              "change Ex arg 1 is DEVMODE");
    }
    CHECK(g_select_count == 2u, "Ex invalid DEVMODE does not select");

    CHECK(as_i32(invoke("ChangeDisplaySettingsA", (const uint32_t[]){ 0u, 0u },
                        2)) == 0,
          "change A null is success");

    printf("user32 display OK (%d handlers)\n", installed);
    return 0;
}
