/*
 * secure_hle.c - target-side MSVCR80 strcpy_s and strncpy_s.
 *
 * The as-built handler treated a null source as an empty string and returned
 * EINVAL for a destination that was too small.  MSVCRT80 separates the two
 * failures: a null source is EINVAL, a short destination is ERANGE, and both
 * leave the destination as an empty string when the destination itself is
 * valid.  strncpy_s follows the same split, adds the count-limited and
 * _TRUNCATE forms, and reports STRUNCATE (80) when _TRUNCATE actually drops
 * source bytes.  The guest only observes the return value and the destination,
 * so this slice follows the CRT contract instead of carrying the simplification
 * forward.  A guest range that is not mapped is refused as EINVAL rather than
 * faulting the host, matching the other target-side memory handlers.
 *
 * The copy loop is deliberately byte oriented: MSVCRT's overlap behavior is a
 * forward copy, so memmove would produce a different result when the
 * destination starts inside the source.
 */
#include "secure_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define CRT_EINVAL 22u
#define CRT_ERANGE 34u
#define CRT_STRUNCATE 80u
#define CRT_TRUNCATE 0xFFFFFFFFu

static void refuse(CPU *c, uint32_t dst, uint32_t code)
{
    if (wc3_legacy_guest_ok(dst, 1u))
        wc3_legacy_wr8g(dst, 0u);
    c->eax = code;
}

static void h_strcpy_s(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t cap = wc3_legacy_arg32(c, 1);
    const uint32_t src = wc3_legacy_arg32(c, 2);

    if (!dst || cap == 0u || !wc3_legacy_guest_ok(dst, 1u)) {
        c->eax = CRT_EINVAL;
        return;
    }
    if (!src) {
        refuse(c, dst, CRT_EINVAL);
        return;
    }

    uint32_t len = 0u;
    for (;;) {
        if (len >= cap) {
            refuse(c, dst, CRT_ERANGE);
            return;
        }
        const uint32_t at = src + len;
        if (at < src || !wc3_legacy_guest_ok(at, 1u)) {
            refuse(c, dst, CRT_EINVAL);
            return;
        }
        if (wc3_legacy_rd8g(at) == 0u)
            break;
        len++;
    }

    const uint32_t total = len + 1u;
    if (dst > UINT32_MAX - total || !wc3_legacy_guest_ok(dst, total)) {
        refuse(c, dst, CRT_EINVAL);
        return;
    }
    wc3_legacy_guest_copy(dst, src, total);
    c->eax = 0u;
}

static void h_strncpy_s(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t cap = wc3_legacy_arg32(c, 1);
    const uint32_t src = wc3_legacy_arg32(c, 2);
    const uint32_t count = wc3_legacy_arg32(c, 3);

    /* A zero count writes an empty string without looking at src.  The CRT
     * also accepts a null destination when the destination size is zero. */
    if (count == 0u) {
        if (!dst) {
            c->eax = cap == 0u ? 0u : CRT_EINVAL;
            return;
        }
        if (cap == 0u || !wc3_legacy_guest_ok(dst, 1u)) {
            c->eax = CRT_EINVAL;
            return;
        }
        wc3_legacy_wr8g(dst, 0u);
        c->eax = 0u;
        return;
    }

    if (!dst || cap == 0u || !wc3_legacy_guest_ok(dst, 1u)) {
        c->eax = CRT_EINVAL;
        return;
    }
    if (!src) {
        refuse(c, dst, CRT_EINVAL);
        return;
    }

    uint32_t i = 0u;
    uint32_t remaining = cap;

    if (count == CRT_TRUNCATE) {
        for (;;) {
            const uint32_t sa = src + i;
            const uint32_t da = dst + i;
            if (sa < src || da < dst ||
                !wc3_legacy_guest_ok(sa, 1u) ||
                !wc3_legacy_guest_ok(da, 1u)) {
                refuse(c, dst, CRT_EINVAL);
                return;
            }
            const uint8_t ch = wc3_legacy_rd8g(sa);
            wc3_legacy_wr8g(da, ch);
            i++;
            if (ch == 0u) {
                c->eax = 0u;
                return;
            }
            if (--remaining == 0u) {
                wc3_legacy_wr8g(da, 0u);
                c->eax = CRT_STRUNCATE;
                return;
            }
        }
    }

    uint32_t left = count;
    for (;;) {
        const uint32_t sa = src + i;
        const uint32_t da = dst + i;
        if (sa < src || da < dst ||
            !wc3_legacy_guest_ok(sa, 1u) ||
            !wc3_legacy_guest_ok(da, 1u)) {
            refuse(c, dst, CRT_EINVAL);
            return;
        }
        const uint8_t ch = wc3_legacy_rd8g(sa);
        wc3_legacy_wr8g(da, ch);
        i++;
        if (ch == 0u) {
            c->eax = 0u;
            return;
        }
        if (--remaining == 0u) {
            refuse(c, dst, CRT_ERANGE);
            return;
        }
        if (--left == 0u) {
            const uint32_t end = dst + i;
            if (end < dst || !wc3_legacy_guest_ok(end, 1u)) {
                refuse(c, dst, CRT_EINVAL);
                return;
            }
            wc3_legacy_wr8g(end, 0u);
            c->eax = 0u;
            return;
        }
    }
}

/* strcpy_s is present in the as-built table and keeps its row.  strncpy_s was
 * never registered there, so it must be appended with its own cdecl arity;
 * calling the override path for it would make install fail before boot. */
static const struct {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} g_additions[] = {
    { "strncpy_s", 4u, h_strncpy_s },
};

int msvcr80_secure_install(void)
{
    if (wc3_legacy_hle_override("MSVCR80.dll", "strcpy_s", h_strcpy_s) != 0)
        return -1;

    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return 1 + (int)(sizeof g_additions / sizeof g_additions[0]);
}
