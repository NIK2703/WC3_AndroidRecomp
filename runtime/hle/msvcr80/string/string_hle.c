/*
 * string_hle.c - target-side MSVCR80 string/memory/ctype/numeric subset.
 *
 * The as-built handlers were host-libc wrappers around guest pointers.  Most
 * were correct, but two contracts were not:
 *
 *   - strncpy must pad the destination with zeros through n when the source is
 *     shorter; the old loop stopped at the first NUL;
 *   - strstr("", "") must return the haystack, not NULL.
 *
 * Both are fixed here and covered by T0.  The character classifiers are
 * explicit ASCII predicates, not host-locale libc calls: CP1251 bytes above
 * 0x7F are neither letters nor whitespace in the CRT's C locale.  _mbsstr is
 * the byte search because the guest code page has no lead bytes.
 */
#include "crt_string_hle.h"

#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"

static int add_ok(uint32_t base, uint32_t off)
{
    return base <= UINT32_MAX - off;
}

static int byte_ok(uint32_t base, uint32_t off)
{
    return add_ok(base, off) && wc3_legacy_guest_ok(base + off, 1);
}

static void h_memcpy(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    wc3_legacy_guest_copy(dst, src, n);
    c->eax = dst;
}

static void h_memmove(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    wc3_legacy_guest_move(dst, src, n);
    c->eax = dst;
}

static void h_memset(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint8_t value = (uint8_t)wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    wc3_legacy_guest_fill(dst, value, n);
    c->eax = dst;
}

static void h_strncpy(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    if (n && dst && wc3_legacy_guest_ok(dst, n)) {
        uint32_t i = 0;
        for (; i < n; i++) {
            const uint8_t ch = (src && byte_ok(src, i))
                                   ? wc3_legacy_rd8g(src + i)
                                   : 0;
            wc3_legacy_wr8g(dst + i, ch);
            if (!ch) {
                i++;
                break;
            }
        }
        for (; i < n; i++) wc3_legacy_wr8g(dst + i, 0);
    }
    c->eax = dst;
}

static void h_strncmp(CPU *c)
{
    const uint32_t a = wc3_legacy_arg32(c, 0);
    const uint32_t b = wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t ca = (a && byte_ok(a, i)) ? wc3_legacy_rd8g(a + i) : 0;
        const uint8_t cb = (b && byte_ok(b, i)) ? wc3_legacy_rd8g(b + i) : 0;
        if (ca != cb) {
            r = (int)ca - (int)cb;
            break;
        }
        if (!ca) break;
    }
    c->eax = (uint32_t)(int32_t)r;
}

static uint8_t ascii_upper(uint8_t ch)
{
    if (ch >= 'a' && ch <= 'z') return (uint8_t)(ch - ('a' - 'A'));
    return ch;
}

static void h_strnicmp(CPU *c)
{
    const uint32_t a = wc3_legacy_arg32(c, 0);
    const uint32_t b = wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    int r = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t ca =
            ascii_upper((a && byte_ok(a, i)) ? wc3_legacy_rd8g(a + i) : 0);
        const uint8_t cb =
            ascii_upper((b && byte_ok(b, i)) ? wc3_legacy_rd8g(b + i) : 0);
        if (ca != cb) {
            r = (int)ca - (int)cb;
            break;
        }
        if (!ca) break;
    }
    c->eax = (uint32_t)(int32_t)r;
}

static void h_strchr(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 1);
    c->eax = 0;
    if (!s) return;
    for (uint32_t i = 0;; i++) {
        if (!byte_ok(s, i)) return;
        const uint8_t cur = wc3_legacy_rd8g(s + i);
        if (cur == ch) {
            c->eax = s + i;
            return;
        }
        if (!cur) return;
    }
}

static void h_strrchr(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 1);
    uint32_t found = 0;
    if (!s) {
        c->eax = 0;
        return;
    }
    for (uint32_t i = 0;; i++) {
        if (!byte_ok(s, i)) break;
        const uint8_t cur = wc3_legacy_rd8g(s + i);
        if (cur == ch) found = s + i;
        if (!cur) break;
    }
    c->eax = found;
}

static int match_at(uint32_t hay, uint32_t needle, uint32_t offset)
{
    for (uint32_t j = 0;; j++) {
        if (j > UINT32_MAX - offset) return 0;
        const uint32_t ho = offset + j;
        if (!byte_ok(hay, ho)) return 0;
        const uint8_t hc = wc3_legacy_rd8g(hay + ho);

        if (!byte_ok(needle, j)) return 0;
        const uint8_t nc = wc3_legacy_rd8g(needle + j);
        if (!nc) return 1;
        if (hc != nc) return 0;
    }
}

static void h_strstr(CPU *c)
{
    const uint32_t hay = wc3_legacy_arg32(c, 0);
    const uint32_t needle = wc3_legacy_arg32(c, 1);
    c->eax = 0;
    if (!hay) return;
    if (!needle || !wc3_legacy_guest_ok(needle, 1) ||
        !wc3_legacy_rd8g(needle)) {
        c->eax = hay; /* empty needle, including the empty haystack */
        return;
    }
    for (uint32_t i = 0;; i++) {
        if (!byte_ok(hay, i)) return;
        if (match_at(hay, needle, i)) {
            c->eax = hay + i;
            return;
        }
        if (!wc3_legacy_rd8g(hay + i)) return;
    }
}

static int accept_has(uint32_t set, uint8_t ch)
{
    if (!set) return 0;
    for (uint32_t i = 0;; i++) {
        if (!byte_ok(set, i)) return 0;
        const uint8_t cur = wc3_legacy_rd8g(set + i);
        if (!cur) return 0;
        if (cur == ch) return 1;
    }
}

static void h_strpbrk(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint32_t accept = wc3_legacy_arg32(c, 1);
    c->eax = 0;
    if (!s || !accept) return;
    for (uint32_t i = 0;; i++) {
        if (!byte_ok(s, i)) return;
        const uint8_t cur = wc3_legacy_rd8g(s + i);
        if (!cur) return;
        if (accept_has(accept, cur)) {
            c->eax = s + i;
            return;
        }
    }
}

static void h_strspn(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint32_t accept = wc3_legacy_arg32(c, 1);
    uint32_t n = 0;
    if (!s || !accept) {
        c->eax = 0;
        return;
    }
    for (;; n++) {
        if (!byte_ok(s, n)) break;
        const uint8_t cur = wc3_legacy_rd8g(s + n);
        if (!cur || !accept_has(accept, cur)) break;
    }
    c->eax = n;
}

static void h_strcspn(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint32_t reject = wc3_legacy_arg32(c, 1);
    uint32_t n = 0;
    if (!s) {
        c->eax = 0;
        return;
    }
    for (;; n++) {
        if (!byte_ok(s, n)) break;
        const uint8_t cur = wc3_legacy_rd8g(s + n);
        if (!cur || accept_has(reject, cur)) break;
    }
    c->eax = n;
}

static void h_strnlen(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint32_t max = wc3_legacy_arg32(c, 1);
    uint32_t n = 0;
    if (s) {
        for (; n < max; n++) {
            if (!byte_ok(s, n) || !wc3_legacy_rd8g(s + n)) break;
        }
    }
    c->eax = n;
}

static void h_strupr(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    if (s) {
        for (uint32_t i = 0;; i++) {
            if (!byte_ok(s, i)) break;
            const uint8_t cur = wc3_legacy_rd8g(s + i);
            if (!cur) break;
            wc3_legacy_wr8g(s + i, ascii_upper(cur));
        }
    }
    c->eax = s;
}

static void h_strlwr(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    if (s) {
        for (uint32_t i = 0;; i++) {
            if (!byte_ok(s, i)) break;
            uint8_t cur = wc3_legacy_rd8g(s + i);
            if (!cur) break;
            if (cur >= 'A' && cur <= 'Z') cur = (uint8_t)(cur + ('a' - 'A'));
            wc3_legacy_wr8g(s + i, cur);
        }
    }
    c->eax = s;
}

static void h_isdigit(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch >= '0' && ch <= '9') ? 1u : 0u;
}

static void h_isalpha(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) ? 1u : 0u;
}

static void h_isspace(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
              ch == '\f' || ch == '\r')
                 ? 1u
                 : 0u;
}

static void h_iswspace(CPU *c)
{
    const uint32_t ch = wc3_legacy_arg32(c, 0);
    c->eax = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
              ch == '\f' || ch == '\r')
                 ? 1u
                 : 0u;
}

static void h_toupper(CPU *c)
{
    c->eax = ascii_upper((uint8_t)wc3_legacy_arg32(c, 0));
}

static void h_tolower(CPU *c)
{
    uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + ('a' - 'A'));
    c->eax = ch;
}

static void h_strdup(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    c->eax = 0;
    if (!s) return;

    uint32_t len = 0;
    for (;; len++) {
        if (!byte_ok(s, len)) return;
        if (!wc3_legacy_rd8g(s + len)) break;
    }
    if (len == UINT32_MAX) return;

    const uint32_t p = wc3_legacy_guest_alloc(len + 1u);
    if (!p) return;
    for (uint32_t i = 0; i <= len; i++)
        wc3_legacy_wr8g(p + i, wc3_legacy_rd8g(s + i));
    c->eax = p;
}

static int32_t parse_decimal(uint32_t s)
{
    uint32_t i = 0;
    while (byte_ok(s, i)) {
        const uint8_t ch = wc3_legacy_rd8g(s + i);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' ||
            ch == '\f' || ch == '\r') {
            i++;
            continue;
        }
        break;
    }

    int negative = 0;
    if (byte_ok(s, i)) {
        const uint8_t ch = wc3_legacy_rd8g(s + i);
        if (ch == '+' || ch == '-') {
            negative = ch == '-';
            i++;
        }
    }

    uint64_t magnitude = 0;
    int any = 0;
    while (byte_ok(s, i)) {
        const uint8_t ch = wc3_legacy_rd8g(s + i);
        if (ch < '0' || ch > '9') break;
        any = 1;
        if (magnitude <= 2147483648ULL) {
            magnitude = magnitude * 10u + (uint32_t)(ch - '0');
            if (magnitude > 2147483648ULL) magnitude = 2147483648ULL;
        }
        i++;
    }
    if (!any) return 0;
    if (negative)
        return magnitude >= 2147483648ULL ? INT32_MIN : -(int32_t)magnitude;
    return magnitude > 2147483647ULL ? INT32_MAX : (int32_t)magnitude;
}

static void h_atoi(CPU *c)
{
    c->eax = (uint32_t)parse_decimal(wc3_legacy_arg32(c, 0));
}

static void h_atol(CPU *c)
{
    c->eax = (uint32_t)parse_decimal(wc3_legacy_arg32(c, 0));
}

static void h_itoa(CPU *c)
{
    const int32_t value = (int32_t)wc3_legacy_arg32(c, 0);
    const uint32_t dst = wc3_legacy_arg32(c, 1);
    const int radix = (int)wc3_legacy_arg32(c, 2);
    if (radix < 2 || radix > 36 || !dst) {
        c->eax = 0;
        return;
    }

    char tmp[40];
    char *p = tmp + sizeof tmp;
    *--p = 0;
    uint32_t magnitude =
        value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    do {
        const int digit = (int)(magnitude % (uint32_t)radix);
        *--p = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        magnitude /= (uint32_t)radix;
    } while (magnitude);
    if (value < 0) *--p = '-';

    const uint32_t len = (uint32_t)(tmp + sizeof tmp - p);
    if (!wc3_legacy_guest_ok(dst, len)) {
        c->eax = 0;
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        wc3_legacy_wr8g(dst + i, (uint8_t)p[i]);
    c->eax = dst;
}

static void h_strcmp(CPU *c)
{
    const uint32_t a = wc3_legacy_arg32(c, 0);
    const uint32_t b = wc3_legacy_arg32(c, 1);
    int r = 0;
    for (uint32_t i = 0;; i++) {
        const uint8_t ca = (a && byte_ok(a, i)) ? wc3_legacy_rd8g(a + i) : 0;
        const uint8_t cb = (b && byte_ok(b, i)) ? wc3_legacy_rd8g(b + i) : 0;
        if (ca != cb) {
            r = (int)ca - (int)cb;
            break;
        }
        if (!ca) break;
    }
    c->eax = (uint32_t)(int32_t)r;
}

static void h_stricmp(CPU *c)
{
    const uint32_t a = wc3_legacy_arg32(c, 0);
    const uint32_t b = wc3_legacy_arg32(c, 1);
    int r = 0;
    for (uint32_t i = 0;; i++) {
        const uint8_t ca =
            ascii_upper((a && byte_ok(a, i)) ? wc3_legacy_rd8g(a + i) : 0);
        const uint8_t cb =
            ascii_upper((b && byte_ok(b, i)) ? wc3_legacy_rd8g(b + i) : 0);
        if (ca != cb) {
            r = (int)ca - (int)cb;
            break;
        }
        if (!ca) break;
    }
    c->eax = (uint32_t)(int32_t)r;
}

static void h_memchr(CPU *c)
{
    const uint32_t s = wc3_legacy_arg32(c, 0);
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 1);
    const uint32_t n = wc3_legacy_arg32(c, 2);
    c->eax = 0;
    if (!s || !n || !wc3_legacy_guest_ok(s, n)) return;
    for (uint32_t i = 0; i < n; i++) {
        if (wc3_legacy_rd8g(s + i) == ch) {
            c->eax = s + i;
            return;
        }
    }
}

static void h_isalnum(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z'))
                 ? 1u
                 : 0u;
}

static void h_isgraph(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch >= 0x21u && ch <= 0x7Eu) ? 1u : 0u;
}

static void h_islower(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch >= 'a' && ch <= 'z') ? 1u : 0u;
}

static void h_isprint(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch >= 0x20u && ch <= 0x7Eu) ? 1u : 0u;
}

static void h_isupper(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = (ch >= 'A' && ch <= 'Z') ? 1u : 0u;
}

static void h_isxdigit(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
              (ch >= 'a' && ch <= 'f'))
                 ? 1u
                 : 0u;
}

/* Guest wchar_t is the 16-bit Windows unit, not the host's 32-bit wchar_t. */
static int wide_unit_ok(uint32_t base, uint32_t index)
{
    if (index > UINT32_MAX / 2u) return 0;
    const uint32_t off = index * 2u;
    return add_ok(base, off) && wc3_legacy_guest_ok(base + off, 2);
}

static uint16_t wide_unit(uint32_t base, uint32_t index)
{
    return wide_unit_ok(base, index) ? wc3_legacy_rd16g(base + index * 2u) : 0;
}

static void h_wcsrchr(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint16_t ch = (uint16_t)wc3_legacy_arg32(c, 1);
    uint32_t found = 0;

    for (uint32_t i = 0; wide_unit_ok(text, i); i++) {
        const uint16_t unit = wide_unit(text, i);
        if (unit == ch) found = text + i * 2u;
        if (!unit) break;
    }
    c->eax = found;
}

static int wide_prefix_at(uint32_t text, uint32_t needle, uint32_t offset)
{
    for (uint32_t i = 0;; i++) {
        if (!wide_unit_ok(needle, i)) return 0;
        if (offset > UINT32_MAX - i) return 0;
        if (!wide_unit_ok(text, offset + i)) return 0;
        const uint16_t want = wide_unit(needle, i);
        const uint16_t got = wide_unit(text, offset + i);
        if (got != want) return 0;
        if (!want) return 1;
    }
}

static void h_wcsstr(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t needle = wc3_legacy_arg32(c, 1);

    if (!text || !needle || !wide_unit_ok(text, 0) ||
        !wide_unit_ok(needle, 0)) {
        c->eax = 0;
        return;
    }
    if (wide_unit(needle, 0) == 0) {
        c->eax = text;
        return;
    }

    for (uint32_t i = 0; wide_unit_ok(text, i); i++) {
        if (wide_prefix_at(text, needle, i)) {
            c->eax = text + i * 2u;
            return;
        }
        if (!wide_unit(text, i)) break;
    }
    c->eax = 0;
}

typedef struct CrtStringOverride {
    const char *name;
    void (*impl)(CPU *c);
} CrtStringOverride;

static const CrtStringOverride g_overrides[] = {
    { "memcpy", h_memcpy },
    { "memmove", h_memmove },
    { "memset", h_memset },
    { "strncpy", h_strncpy },
    { "strncmp", h_strncmp },
    { "_strnicmp", h_strnicmp },
    { "strchr", h_strchr },
    { "strrchr", h_strrchr },
    { "strstr", h_strstr },
    { "strpbrk", h_strpbrk },
    { "strspn", h_strspn },
    { "strcspn", h_strcspn },
    { "strnlen", h_strnlen },
    { "_strupr", h_strupr },
    { "_strlwr", h_strlwr },
    { "_strdup", h_strdup },
    { "_itoa", h_itoa },
    { "atoi", h_atoi },
    { "atol", h_atol },
    { "isdigit", h_isdigit },
    { "isalpha", h_isalpha },
    { "isspace", h_isspace },
    { "iswspace", h_iswspace },
    { "toupper", h_toupper },
    { "tolower", h_tolower },
};

/* Game imports that the as-built table never defined.  The resolver used to
 * install a guessed-arity no-op stub for these; strcmp and memchr in
 * particular returned whatever happened to be in eax, and every is* predicate
 * returned false. */
typedef struct CrtStringAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} CrtStringAddition;

static const CrtStringAddition g_additions[] = {
    { "strcmp", 2, h_strcmp },
    { "_stricmp", 2, h_stricmp },
    { "memchr", 3, h_memchr },
    { "isalnum", 1, h_isalnum },
    { "isgraph", 1, h_isgraph },
    { "islower", 1, h_islower },
    { "isprint", 1, h_isprint },
    { "isupper", 1, h_isupper },
    { "isxdigit", 1, h_isxdigit },
    { "wcsrchr", 2, h_wcsrchr },
    { "wcsstr", 2, h_wcsstr },
    /* _mbsstr only differs from strstr on a code page with lead bytes.  The
     * guest runs CP1251, which is SBCS, so the byte search is exact. */
    { "_mbsstr", 2, h_strstr },
};

int wc3_hle_msvcr80_string_install(void)
{
    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("MSVCR80.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("MSVCR80.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 1,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
