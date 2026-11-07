/*
 * secure_hle.c - target-side MSVCR80 strcpy_s.
 *
 * The as-built handler treated a null source as an empty string and returned
 * EINVAL for a destination that was too small.  MSVCRT80 separates the two
 * failures: a null source is EINVAL, a short destination is ERANGE, and both
 * leave the destination as an empty string when the destination itself is
 * valid.  The guest only observes the return value and the destination, so
 * this slice follows the CRT contract instead of carrying the simplification
 * forward.  A guest range that is not mapped is refused as EINVAL rather than
 * faulting the host, matching the other target-side memory handlers.
 */
#include "secure_hle.h"

#include <stdint.h>

#include "cpu.h"
#include "legacy_shim.h"

#define CRT_EINVAL 22u
#define CRT_ERANGE 34u
/* MSVCRT80 defines RSIZE_MAX as SIZE_MAX >> 1; the guest is 32-bit. */
#define RSIZE_MAX 0x7FFFFFFFu

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

    if (!dst || cap == 0u || cap > RSIZE_MAX ||
        !wc3_legacy_guest_ok(dst, 1u)) {
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

static const struct {
    const char *name;
    void (*impl)(CPU *c);
} g_overrides[] = {
    { "strcpy_s", h_strcpy_s },
};

int msvcr80_secure_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0]);
}
