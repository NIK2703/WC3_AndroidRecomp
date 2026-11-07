/*
 * system_hle.c - target-side kernel32 version/machine-information subset.
 *
 * Why this module exists
 * ----------------------
 * GetVersionExA is not a free-form buffer: dwOSVersionInfoSize is a promise
 * about the caller's allocation, and a wrong write length crosses the guest's
 * stack cookie.  The as-built handler was fixed to honor that promise; moving
 * it here keeps the fix in target code and gives it a T0 test.  The other four
 * handlers are fixed machine facts and are grouped with it because they share
 * the same SYSTEM_INFO/STARTUPINFO layouts.
 *
 * The values are the Windows XP model the game was built against (5.1, build
 * 2600); they are intentionally not the host's real OS version, because guest
 * feature checks must see the oracle's environment.
 */
#include "system_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "guest_layout.h"
#include "legacy_shim.h"

/* GetVersion packs major/minor into the low word and the build into the high
 * word: 5.1 build 2600. */
#define WINDOWS_XP_VERSION 0x0A280105u
#define OSVERSIONINFOA_SIZE 0x94u
#define OSVERSIONINFOEXA_SIZE 0x9Cu
/* Largest layout the as-built handler accepted; 0x11C is OSVERSIONINFOEXW. */
#define OSVERSIONINFO_MAX_SIZE 0x11Cu

static void h_get_version(CPU *c)
{
    c->eax = WINDOWS_XP_VERSION;
}

static void h_get_version_ex(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    if (!p || !wc3_legacy_guest_ok(p, 4)) {
        c->eax = 0;
        return;
    }
    const uint32_t size = wc3_legacy_rd32g(p);
    if (size < OSVERSIONINFOA_SIZE || size > OSVERSIONINFO_MAX_SIZE ||
        !wc3_legacy_guest_ok(p, size)) {
        c->eax = 0;
        return;
    }

    wc3_legacy_guest_zero(p, size);
    wc3_legacy_wr32g(p + 0, size);
    wc3_legacy_wr32g(p + 4, 5);      /* dwMajorVersion */
    wc3_legacy_wr32g(p + 8, 1);      /* dwMinorVersion */
    wc3_legacy_wr32g(p + 12, 2600);  /* dwBuildNumber */
    wc3_legacy_wr32g(p + 16, 2);     /* VER_PLATFORM_WIN32_NT */
    if (size >= OSVERSIONINFOEXA_SIZE) {
        wc3_legacy_wr16g(p + 148, 0); /* wServicePackMajor */
        wc3_legacy_wr16g(p + 150, 0); /* wServicePackMinor */
        wc3_legacy_wr16g(p + 152, 0); /* wSuiteMask */
        wc3_legacy_wr8g(p + 154, 1);  /* wProductType = VER_NT_WORKSTATION */
        wc3_legacy_wr8g(p + 155, 0);  /* wReserved */
    }
    c->eax = 1;
}

static void h_get_system_info(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    if (p && wc3_legacy_guest_ok(p, 36)) {
        wc3_legacy_guest_zero(p, 36);
        wc3_legacy_wr16g(p + 0, 0);          /* wProcessorArchitecture = x86 */
        wc3_legacy_wr32g(p + 4, 0x1000);     /* dwPageSize */
        wc3_legacy_wr32g(p + 8, 0x00400000); /* lpMinimumApplicationAddress */
        wc3_legacy_wr32g(p + 12, 0x7FFEFFFF);/* lpMaximumApplicationAddress */
        wc3_legacy_wr32g(p + 16, 3);         /* dwActiveProcessorMask */
        wc3_legacy_wr32g(p + 20, 4);         /* dwNumberOfProcessors */
        wc3_legacy_wr32g(p + 24, 1);         /* dwProcessorType = 586 */
        wc3_legacy_wr32g(p + 28, 0x10000);   /* dwAllocationGranularity */
        wc3_legacy_wr16g(p + 32, 6);         /* wProcessorLevel */
        wc3_legacy_wr16g(p + 34, 0x3A09);    /* wProcessorRevision */
    }
    c->eax = 0;
}

static void h_global_memory_status(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    if (p && wc3_legacy_guest_ok(p, 32)) {
        wc3_legacy_guest_zero(p, 32);
        /* MEMORYSTATUS.dwLength is the size of the structure (32), not 2048.
         * The as-built value made the size field disagree with the buffer it
         * had just filled. */
        wc3_legacy_wr32g(p + 0, 32);         /* dwLength */
        wc3_legacy_wr32g(p + 4, 55);         /* dwMemoryLoad */
        wc3_legacy_wr32g(p + 8, 0x7FFFFFFF); /* dwTotalPhys */
        wc3_legacy_wr32g(p + 12, 0x3FFFFFFF);/* dwAvailPhys */
        wc3_legacy_wr32g(p + 16, 0xFFFFFFFF);/* dwTotalPageFile */
        wc3_legacy_wr32g(p + 20, 0x7FFFFFFF);/* dwAvailPageFile */
        wc3_legacy_wr32g(p + 24, 0x7FFFFFFF);/* dwTotalVirtual */
        wc3_legacy_wr32g(p + 28, 0x3FFFFFFF);/* dwAvailVirtual */
    }
    c->eax = 0;
}

static void h_get_startup_info(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    if (p && wc3_legacy_guest_ok(p, 68)) wc3_legacy_guest_zero(p, 68);
    c->eax = 0;
}

static int wide_unit_ok(uint32_t base, uint32_t index)
{
    if (index > UINT32_MAX / 2u) return 0;
    const uint32_t off = index * 2u;
    return base <= UINT32_MAX - off &&
           wc3_legacy_guest_ok(base + off, 2);
}

static uint16_t wide_unit(uint32_t base, uint32_t index)
{
    return wide_unit_ok(base, index) ? wc3_legacy_rd16g(base + index * 2u) : 0;
}

static int wide_name_equals(uint32_t name, const char *ascii)
{
    for (uint32_t i = 0;; i++) {
        if (!wide_unit_ok(name, i)) return 0;
        uint16_t w = wide_unit(name, i);
        uint8_t a = (uint8_t)ascii[i];
        if (w >= 'A' && w <= 'Z') w = (uint16_t)(w + 32u);
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32u);
        if (w != a) return 0;
        if (!w) return 1;
    }
}

static uint32_t wide_basename(uint32_t name)
{
    uint32_t base = name;
    for (uint32_t i = 0; wide_unit_ok(name, i); i++) {
        const uint16_t ch = wide_unit(name, i);
        if (!ch) break;
        if (ch == '\\' || ch == '/') base = name + (i + 1u) * 2u;
    }
    return base;
}

static void h_mul_div(CPU *c)
{
    const int32_t number = (int32_t)wc3_legacy_arg32(c, 0);
    const int32_t numerator = (int32_t)wc3_legacy_arg32(c, 1);
    const int32_t denominator = (int32_t)wc3_legacy_arg32(c, 2);

    if (!denominator) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    const int64_t product = (int64_t)number * (int64_t)numerator;
    const int64_t half = (int64_t)denominator / 2;
    const int64_t result =
        product >= 0 ? (product + half) / denominator
                     : (product - half) / denominator;
    if (result > INT32_MAX || result < INT32_MIN) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    c->eax = (uint32_t)(int32_t)result;
}

static void h_get_module_handle_w(CPU *c)
{
    const uint32_t name = wc3_legacy_arg32(c, 0);

    if (!name) {
        c->eax = GUEST_IMAGE_WAR3;
        return;
    }
    if (!wide_unit_ok(name, 0)) {
        c->eax = 0;
        return;
    }

    const uint32_t base = wide_basename(name);
    uint32_t module = 0;
    if (wide_name_equals(base, "war3.exe"))
        module = GUEST_IMAGE_WAR3;
    else if (wide_name_equals(base, "game.dll"))
        module = GUEST_IMAGE_GAME;
    else if (wide_name_equals(base, "storm.dll"))
        module = GUEST_IMAGE_STORM;
    else if (wide_name_equals(base, "mss32.dll"))
        module = GUEST_IMAGE_MSS;
    else if (wide_name_equals(base, "mssfast.dll"))
        module = GUEST_IMAGE_MSSFAST;
    else if (wide_name_equals(base, "mp3dec.asi"))
        module = GUEST_IMAGE_MP3DEC;
    else if (wide_name_equals(base, "ijl15.dll"))
        module = GUEST_IMAGE_IJL;
    c->eax = module;
}

typedef struct SystemOverride {
    const char *name;
    void (*impl)(CPU *c);
} SystemOverride;

static const SystemOverride g_overrides[] = {
    { "GetVersion", h_get_version },
    { "GetVersionExA", h_get_version_ex },
    { "GetSystemInfo", h_get_system_info },
    { "GetNativeSystemInfo", h_get_system_info },
    { "GlobalMemoryStatus", h_global_memory_status },
    { "GetStartupInfoA", h_get_startup_info },
};

typedef struct SystemAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} SystemAddition;

static const SystemAddition g_additions[] = {
    { "GetModuleHandleW", 1, h_get_module_handle_w },
    { "MulDiv", 3, h_mul_div },
};

int wc3_hle_kernel32_system_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("KERNEL32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("KERNEL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
