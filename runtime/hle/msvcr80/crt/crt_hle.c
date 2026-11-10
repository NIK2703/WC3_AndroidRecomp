/*
 * crt_hle.c - target-side MSVCR80 CRT runtime helpers.
 *
 * _vacopy is the x86 va_list copy helper.  On Windows the guest va_list is a
 * 32-bit pointer to the next argument, so the operation is a single 32-bit
 * store: MSVCR120's body is
 *
 *     mov eax,[esp+4]      ; destination va_list *
 *     mov ecx,[esp+8]      ; source va_list
 *     mov [eax],ecx
 *     ret
 *
 * and leaves EAX equal to the destination pointer.  The handler performs the
 * same store through the guest range check.  An unmapped destination is
 * ignored instead of faulting the host; the guest pointer is still left in
 * EAX, matching the register state of the measured body.
 *
 * abort and _wassert are the fatal runtime-control rows.  MSVCR120's default
 * abort path terminates with exit code 3; _wassert reports the failed
 * expression, file and line, then terminates the same way.  The report text is
 * diagnostic-only and guarded by WC3_DEBUG_HLE; the release artifact keeps the
 * termination.
 *
 * setlocale is the C-locale-only implementation: the target has no locale
 * data, so "" and "C"/"POSIX" select the C locale and any other name fails.
 */
#include "crt_hle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "wc3_debug.h"

static void h_vacopy(CPU *c)
{
    const uint32_t destination = wc3_legacy_arg32(c, 0u);
    const uint32_t source = wc3_legacy_arg32(c, 1u);

    if (destination && wc3_legacy_guest_ok(destination, 4u))
        wc3_legacy_wr32g(destination, source);

    c->eax = destination;
}

/* Copy a bounded guest C string.  Returns 0 when the first byte is not mapped;
 * a truncated but mapped string is returned as-is and the caller decides
 * whether it is a supported name. */
static int read_c_string(uint32_t address, char *buffer, size_t capacity)
{
    size_t i = 0;

    if (!buffer || !capacity) return 0;
    buffer[0] = '\0';
    if (!address) return 0;
    while (i + 1u < capacity) {
        if (address > UINT32_MAX - (uint32_t)i) break;
        const uint32_t byte_address = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(byte_address, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(byte_address);
        buffer[i++] = ch;
        if (!ch) return 1;
    }
    buffer[i] = '\0';
    return 1;
}

#if WC3_DEBUG && WC3_DEBUG_HLE
static void copy_wide_debug(uint32_t address, char *buffer, size_t capacity)
{
    size_t i = 0;

    if (!buffer || !capacity) return;
    if (!address) {
        buffer[0] = '\0';
        return;
    }
    while (i + 1u < capacity) {
        const uint32_t unit_address = address + (uint32_t)(i * 2u);
        if (!wc3_legacy_guest_ok(unit_address, 2u)) break;
        const uint32_t unit = wc3_legacy_rd16g(unit_address);
        if (!unit) break;
        buffer[i++] = (unit >= 0x20u && unit < 0x7Fu) ? (char)unit : '?';
    }
    buffer[i] = '\0';
}
#endif

/* MSVCR120 abort() terminates through _exit(3) in the default configuration.
 * The guest cannot unwind further, so neither can the host. */
static void h_abort(CPU *c)
{
    (void)c;
#if WC3_DEBUG && WC3_DEBUG_HLE
    fprintf(stderr, "[hle] abort()\n");
    fflush(stderr);
#endif
    _Exit(3);
}

/* _wassert reports the failed expression, source file and line before the CRT
 * terminates.  The release artifact keeps the fatal outcome; the report text
 * is diagnostic-only and therefore lives behind the HLE tag. */
static void h_wassert(CPU *c)
{
    (void)c;
#if WC3_DEBUG && WC3_DEBUG_HLE
    char expression[128];
    char file[128];

    copy_wide_debug(wc3_legacy_arg32(c, 0u), expression, sizeof expression);
    copy_wide_debug(wc3_legacy_arg32(c, 1u), file, sizeof file);
    fprintf(stderr, "[hle] assertion failed: %s, file %s, line %u\n",
            expression, file, wc3_legacy_arg32(c, 2u));
    fflush(stderr);
#endif
    _Exit(3);
}

enum {
    CRT_LC_ALL = 0,
    CRT_LC_COLLATE = 1,
    CRT_LC_CTYPE = 2,
    CRT_LC_MONETARY = 3,
    CRT_LC_NUMERIC = 4,
    CRT_LC_TIME = 5
};

/* The only locale the target carries is the C locale.  The returned pointer
 * must be a guest address, so allocate the two-byte name once. */
static uint32_t c_locale_name(void)
{
    static uint32_t name;

    if (!name) {
        name = wc3_legacy_guest_alloc0(2u);
        if (name) {
            wc3_legacy_wr8g(name, (uint8_t)'C');
            wc3_legacy_wr8g(name + 1u, 0u);
        }
    }
    return name;
}

/* setlocale(category, locale): NULL queries the current locale, "" selects the
 * environment default (C on the target), and "C"/"POSIX" select the C locale.
 * Any other name has no data in this build and fails with NULL. */
static void h_setlocale(CPU *c)
{
    const uint32_t category = wc3_legacy_arg32(c, 0u);
    const uint32_t locale = wc3_legacy_arg32(c, 1u);

    if (category > CRT_LC_TIME) {
        c->eax = 0;
        return;
    }
    if (!locale) {
        c->eax = c_locale_name();
        return;
    }

    char name[32];
    if (!read_c_string(locale, name, sizeof name)) {
        c->eax = 0;
        return;
    }
    if (name[0] == '\0' || strcmp(name, "C") == 0 ||
        strcmp(name, "POSIX") == 0) {
        c->eax = c_locale_name();
        return;
    }
    c->eax = 0;
}

static const struct {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} g_additions[] = {
    { "_vacopy", 2u, h_vacopy },
    { "abort", 0u, h_abort },
    { "_wassert", 3u, h_wassert },
    { "setlocale", 2u, h_setlocale },
};

int wc3_hle_msvcr80_crt_install(void)
{
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_additions / sizeof g_additions[0]);
}
