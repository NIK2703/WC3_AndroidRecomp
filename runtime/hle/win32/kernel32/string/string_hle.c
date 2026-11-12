/*
 * string_hle.c - target-side kernel32 narrow-string subset.
 *
 * lstrlenA, lstrcpynA, GetACP, GetOEMCP and GetCPInfo are byte-oriented and
 * locale-free.  They are moved before the rest of the string/locale group
 * because they can be verified against the Windows contract without guessing
 * an encoding.  The ANSI/OEM code pages stay the fixed 1251/866 model: the
 * guest's CPINFO describes a single-byte code page, so MaxCharSize is 1 and
 * there are no lead bytes.
 *
 * lstrcpynA always null-terminates when iMaxLength is nonzero, including the
 * truncation case.  A Windows probe confirmed that n=4 on "abcdef" produces
 * "abc\0", not four unterminated bytes; n=1 produces "\0"; n=0 leaves the
 * buffer untouched.
 *
 * lstrcatA finds the destination terminator first, then copies the source
 * including its terminator.  The as-built table did not define it, so the
 * resolver left a guessed-arity stub that returned zero without appending
 * anything.
 */
#include "string_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "hle_bridge.h"

/* Largest chunk to validate with one guest_ok() call.  The chunk stays inside
 * the 32-bit address space and never exceeds the caller's byte budget. */
static uint32_t guest_string_chunk(uint32_t address, uint32_t remaining)
{
    uint32_t chunk = remaining < 64u ? remaining : 64u;
    const uint64_t room = 0x100000000ull - address;
    if ((uint64_t)chunk > room) chunk = (uint32_t)room;
    return chunk;
}

static void h_lstrlen_a(CPU *c)
{
    uint32_t p = wc3_legacy_arg32(c, 0);
    uint32_t n = 0;

    while (p) {
        const uint32_t chunk = guest_string_chunk(p, UINT32_MAX);
        if (wc3_legacy_guest_ok(p, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_rd8g(p + j)) {
                    c->eax = n;
                    return;
                }
                n++;
            }
            p += chunk;
        } else {
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_guest_ok(p, 1u) || !wc3_legacy_rd8g(p)) {
                    c->eax = n;
                    return;
                }
                n++;
                p++;
                if (!p) {
                    c->eax = n;
                    return;
                }
            }
        }
    }
    c->eax = n;
}

static void h_lstrcpyn_a(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);
    const uint32_t max = wc3_legacy_arg32(c, 2);

    if (dst && max && wc3_legacy_guest_ok(dst, max)) {
        uint32_t i = 0;
        while (i + 1u < max) {
            if (!src || src > UINT32_MAX - i) break;
            const uint32_t at = src + i;
            const uint32_t chunk = guest_string_chunk(at, max - 1u - i);
            int stop = 0;

            if (wc3_legacy_guest_ok(at, chunk)) {
                for (uint32_t j = 0; j < chunk; j++) {
                    const uint8_t ch = wc3_legacy_rd8g(at + j);
                    if (!ch) {
                        stop = 1;
                        break;
                    }
                    wc3_legacy_wr8g(dst + i, ch);
                    i++;
                }
            } else {
                for (uint32_t j = 0; j < chunk; j++) {
                    const uint32_t byte = at + j;
                    if (!wc3_legacy_guest_ok(byte, 1u)) {
                        stop = 1;
                        break;
                    }
                    const uint8_t ch = wc3_legacy_rd8g(byte);
                    if (!ch) {
                        stop = 1;
                        break;
                    }
                    wc3_legacy_wr8g(dst + i, ch);
                    i++;
                }
            }
            if (stop) break;
        }
        wc3_legacy_wr8g(dst + i, 0);
    }
    c->eax = dst;
}

static void h_lstrcat_a(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);

    if (!dst || !src || !wc3_legacy_guest_ok(dst, 1)) {
        c->eax = dst;
        return;
    }

    uint32_t end = dst;
    int found = 0;
    while (!found) {
        const uint32_t chunk = guest_string_chunk(end, UINT32_MAX);
        if (wc3_legacy_guest_ok(end, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_rd8g(end)) {
                    found = 1;
                    break;
                }
                if (end == UINT32_MAX) {
                    c->eax = dst;
                    return;
                }
                end++;
            }
        } else {
            int bad = 0;
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_guest_ok(end, 1u)) {
                    bad = 1;
                    break;
                }
                if (!wc3_legacy_rd8g(end)) {
                    found = 1;
                    break;
                }
                if (end == UINT32_MAX) {
                    c->eax = dst;
                    return;
                }
                end++;
            }
            if (bad) {
                c->eax = dst;
                return;
            }
        }
    }
    if (!wc3_legacy_guest_ok(end, 1)) {
        c->eax = dst;
        return;
    }

    uint32_t i = 0;
    while (src <= UINT32_MAX - i && end <= UINT32_MAX - i) {
        const uint32_t src_at = src + i;
        const uint32_t dst_at = end + i;
        uint32_t chunk = guest_string_chunk(src_at, UINT32_MAX);
        const uint32_t dst_chunk = guest_string_chunk(dst_at, UINT32_MAX);
        if (chunk > dst_chunk) chunk = dst_chunk;

        if (wc3_legacy_guest_ok(src_at, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                const uint8_t ch = wc3_legacy_rd8g(src_at + j);
                if (!wc3_legacy_guest_ok(dst_at + j, 1u)) {
                    c->eax = dst;
                    return;
                }
                wc3_legacy_wr8g(dst_at + j, ch);
                if (!ch) {
                    c->eax = dst;
                    return;
                }
            }
        } else {
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_guest_ok(src_at + j, 1u)) {
                    wc3_legacy_wr8g(dst_at + j, 0);
                    c->eax = dst;
                    return;
                }
                const uint8_t ch = wc3_legacy_rd8g(src_at + j);
                if (!wc3_legacy_guest_ok(dst_at + j, 1u)) {
                    c->eax = dst;
                    return;
                }
                wc3_legacy_wr8g(dst_at + j, ch);
                if (!ch) {
                    c->eax = dst;
                    return;
                }
            }
        }
        i += chunk;
    }
    c->eax = dst;
}

static void h_is_bad_string_ptr_a(CPU *c)
{
    const uint32_t p = wc3_legacy_arg32(c, 0);
    const uint32_t max = wc3_legacy_arg32(c, 1);

    if (!p) {
        c->eax = 1;
        return;
    }
    if (!max) {
        c->eax = 0;
        return;
    }
    for (uint32_t i = 0; i < max; ) {
        if (p > UINT32_MAX - i) {
            c->eax = 1;
            return;
        }
        const uint32_t at = p + i;
        const uint32_t chunk = guest_string_chunk(at, max - i);

        if (wc3_legacy_guest_ok(at, chunk)) {
            for (uint32_t j = 0; j < chunk; j++) {
                if (!wc3_legacy_rd8g(at + j)) {
                    c->eax = 0;
                    return;
                }
            }
        } else {
            /* Fall back to the per-byte form when the chunk crosses an
             * unmapped page; the result stays all-or-nothing. */
            for (uint32_t j = 0; j < chunk; j++) {
                const uint32_t byte = at + j;
                if (!wc3_legacy_guest_ok(byte, 1u)) {
                    c->eax = 1;
                    return;
                }
                if (!wc3_legacy_rd8g(byte)) {
                    c->eax = 0;
                    return;
                }
            }
        }
        i += chunk;
    }
    c->eax = 0;
}

static void h_get_acp(CPU *c)
{
    c->eax = 1251;
}

static void h_get_oemcp(CPU *c)
{
    /* The fixed 1251 ANSI model pairs with the Russian OEM code page.  Keep
     * the two answers in one place until the locale layer owns both. */
    c->eax = 866;
}

static void h_get_cp_info(CPU *c)
{
    const uint32_t cp = wc3_legacy_arg32(c, 0);
    const uint32_t out = wc3_legacy_arg32(c, 1);
    if (!out || !wc3_legacy_guest_ok(out, 20)) {
        c->eax = 0;
        return;
    }
    wc3_legacy_guest_zero(out, 20);
    const uint32_t max_char_size =
        (cp == 65001u) ? 4u : (cp == 1200u || cp == 1201u) ? 2u : 1u;
    wc3_legacy_wr32g(out, max_char_size);
    wc3_legacy_wr8g(out + 4, '?');
    c->eax = 1;
}

typedef struct StringOverride {
    const char *name;
    void (*impl)(CPU *c);
} StringOverride;

static const StringOverride g_overrides[] = {
    { "lstrlenA", h_lstrlen_a },
    { "lstrcpynA", h_lstrcpyn_a },
    { "GetACP", h_get_acp },
    { "GetCPInfo", h_get_cp_info },
};

typedef struct StringAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} StringAddition;

static const StringAddition g_additions[] = {
    { "lstrcatA", 2, h_lstrcat_a },
    { "IsBadStringPtrA", 2, h_is_bad_string_ptr_a },
    { "GetOEMCP", 0, h_get_oemcp },
};

int wc3_hle_kernel32_string_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("KERNEL32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("KERNEL32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0, /* __stdcall */
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
