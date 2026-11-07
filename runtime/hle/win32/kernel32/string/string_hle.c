/*
 * string_hle.c - target-side kernel32 narrow-string subset.
 *
 * lstrlenA, lstrcpynA, GetACP and GetCPInfo are byte-oriented and locale-free.
 * They are moved before the rest of the string/locale group because they can
 * be verified against the Windows contract without guessing an encoding.
 * The ANSI code page stays the fixed 1251 model: the guest's CPINFO describes
 * a single-byte code page, so MaxCharSize is 1 and there are no lead bytes.
 *
 * lstrcpynA always null-terminates when iMaxLength is nonzero, including the
 * truncation case.  A Windows probe confirmed that n=4 on "abcdef" produces
 * "abc\0", not four unterminated bytes; n=1 produces "\0"; n=0 leaves the
 * buffer untouched.
 */
#include "string_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

static void h_lstrlen_a(CPU *c)
{
    uint32_t p = wc3_legacy_arg32(c, 0);
    uint32_t n = 0;
    while (p && wc3_legacy_guest_ok(p, 1) && wc3_legacy_rd8g(p)) {
        n++;
        p++;
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
            if (!src || src > UINT32_MAX - i ||
                !wc3_legacy_guest_ok(src + i, 1))
                break;
            const uint8_t ch = wc3_legacy_rd8g(src + i);
            if (!ch) break;
            wc3_legacy_wr8g(dst + i, ch);
            i++;
        }
        wc3_legacy_wr8g(dst + i, 0);
    }
    c->eax = dst;
}

static void h_get_acp(CPU *c)
{
    c->eax = 1251;
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

int wc3_hle_kernel32_string_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("KERNEL32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
