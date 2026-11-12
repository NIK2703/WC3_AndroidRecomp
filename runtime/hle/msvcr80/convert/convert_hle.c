/*
 * convert_hle.c - target-side MSVCR80 numeric conversion and multibyte ctype.
 *
 * The as-built strtol/strtoul called the host conversion routines.  On the
 * arm64 host `long` is 64 bits, so a value above LONG_MAX no longer saturated
 * the way the 32-bit MSVC CRT does: it was truncated by the uint32_t cast
 * instead.  The as-built atof was a stub returning zero, and _ismbcspace was
 * an unconditional zero.  This module replaces those five rows with explicit
 * 32-bit parsing and the CP1251 classification the guest actually runs under.
 *
 * The guest uses CP1251 (GetACP returns 1251), which is a single-byte code
 * page.  _ismbblead therefore reports zero for every byte; a nonzero answer
 * would make the CRT treat the following byte as a trail byte and split text
 * at the wrong place.  _ismbcspace is the ASCII whitespace set.
 *
 * The as-built table also lacked the 64-bit integer conversions and strtod.
 * They follow the same MSVCRT grammar as the 32-bit pair, with 64-bit
 * saturation in EDX:EAX; strtod copies the guest string into a bounded host
 * buffer because the host parser cannot read guest memory.  C's conversion
 * routines set errno on overflow.  The as-built handlers never mapped errno
 * into guest memory, and there is no target seam for the guest CRT's errno,
 * so this slice preserves the value-only behaviour rather than inventing a
 * guest errno write.
 *
 * wcstombs runs in the default "C" locale, where MSVCRT maps each wide unit
 * 0x00..0xFF to the same byte and fails on anything above 0xFF.  The count
 * form returns the required length with a null destination, stops after n
 * produced bytes without a terminator when the buffer is exactly full, and
 * returns (size_t)-1 without a terminator on the first unrepresentable unit.
 * The guest's 16-bit units are read with the wide parser boundary checks, so
 * a bad range fails the conversion instead of faulting the host.
 */
#include "convert_hle.h"

#include <stdint.h>
#include <stdlib.h>

#include "cpu.h"
#include "hle_bridge.h"

#define ATOF_BUFFER 1024u

/* Wide numeric strings are ASCII in the C locale.  Convert guest UTF-16LE
 * units into a host buffer, mapping anything above 0x7F to a byte the parser
 * cannot consume, so the conversion stops at the first non-ASCII unit. */
#define WIDE_BUFFER 1024u

#define WIDE_FAIL UINT32_MAX

typedef struct ParsedInteger {
    uint32_t end;       /* first byte that was not part of the number */
    uint64_t magnitude; /* valid when overflow is clear */
    int negative;
    int overflow;
    int digits;
} ParsedInteger;

static void set_edx_eax(CPU *c, uint64_t value);

static int byte_at(uint32_t base, uint32_t offset, uint8_t *out)
{
    if (!base || base > UINT32_MAX - offset) return 0;
    if (!wc3_legacy_guest_ok(base + offset, 1)) return 0;
    *out = wc3_legacy_rd8g(base + offset);
    return 1;
}

static int ascii_space(uint8_t ch)
{
    return ch == 0x20u || (ch >= 0x09u && ch <= 0x0Du);
}

static int digit_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A') + 10;
    return -1;
}

/* strtol/strtoul share the subject-sequence rules.  `base` is the signed
 * third argument; 0 selects base 8/10/16 from the prefix, 2..36 is explicit,
 * and anything else is an invalid argument with no conversion. */
static ParsedInteger parse_integer(uint32_t text, int base)
{
    ParsedInteger r = { text, 0u, 0, 0, 0 };
    uint32_t offset = 0;
    uint8_t ch;

    while (byte_at(text, offset, &ch) && ascii_space(ch)) offset++;

    if (byte_at(text, offset, &ch) && (ch == '+' || ch == '-')) {
        r.negative = ch == '-';
        offset++;
    }

    int radix = base;
    if (radix < 0 || radix == 1 || radix > 36) {
        r.end = text;
        return r;
    }

    if (radix == 0 || radix == 16) {
        uint8_t zero, marker, digit;
        if (byte_at(text, offset, &zero) && zero == '0' &&
            byte_at(text, offset + 1u, &marker) &&
            (marker == 'x' || marker == 'X') &&
            byte_at(text, offset + 2u, &digit) &&
            digit_value(digit) >= 0 && digit_value(digit) < 16) {
            radix = 16;
            offset += 2u;
        } else if (radix == 0) {
            radix = (byte_at(text, offset, &zero) && zero == '0') ? 8 : 10;
        }
    }

    uint64_t value = 0;
    for (;;) {
        if (!byte_at(text, offset, &ch)) break;
        const int digit = digit_value(ch);
        if (digit < 0 || digit >= radix) break;
        if (!r.overflow) {
            if (value > (UINT64_MAX - (uint64_t)digit) / (uint64_t)radix)
                r.overflow = 1;
            else
                value = value * (uint64_t)radix + (uint64_t)digit;
        }
        r.digits = 1;
        offset++;
    }

    if (!r.digits) {
        /* No conversion: strtol points endptr at the original string, not at
         * the whitespace or sign it skipped. */
        r.end = text;
        return r;
    }

    r.end = text + offset;
    r.magnitude = value;
    return r;
}

static uint32_t copy_guest_text(uint32_t text, char *buffer, uint32_t capacity)
{
    uint32_t length = 0;

    while (length + 1u < capacity) {
        uint8_t ch;
        if (!byte_at(text, length, &ch)) break;
        buffer[length++] = (char)ch;
        if (!ch) break;
    }
    buffer[length] = 0;
    return length;
}

static uint32_t copy_guest_wide(uint32_t text, char *buffer, uint32_t capacity)
{
    uint32_t units = 0;

    if (!text || !capacity) {
        if (capacity) buffer[0] = 0;
        return 0;
    }
    while (units + 1u < capacity) {
        const uint32_t off = units * 2u;
        if (text > UINT32_MAX - off || !wc3_legacy_guest_ok(text + off, 2))
            break;
        const uint16_t ch = wc3_legacy_rd16g(text + off);
        if (!ch) break;
        buffer[units] = ch <= 0x7Fu ? (char)ch : (char)0xFF;
        units++;
    }
    buffer[units] = 0;
    return units;
}

/* Copy the ASCII form into the guest heap so the existing byte parser can
 * return an end pointer in guest address space.  The conversion is capped at
 * WIDE_BUFFER units; a longer number stops at the same boundary as strtod. */
static int parse_wide_integer(uint32_t text, int base, ParsedInteger *parsed)
{
    char buffer[WIDE_BUFFER];
    const uint32_t units = copy_guest_wide(text, buffer, WIDE_BUFFER);
    const uint32_t scratch = wc3_legacy_guest_alloc(units + 1u);
    if (!scratch) {
        parsed->end = text;
        parsed->digits = 0;
        return 0;
    }
    for (uint32_t i = 0; i <= units; i++)
        wc3_legacy_wr8g(scratch + i, (uint8_t)buffer[i]);

    *parsed = parse_integer(scratch, base);
    if (parsed->digits) {
        const uint32_t consumed = parsed->end - scratch;
        parsed->end = text + consumed * 2u;
    } else {
        parsed->end = text;
    }
    wc3_legacy_guest_free(scratch);
    return 1;
}

static void h_wcstol(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    ParsedInteger r;

    (void)parse_wide_integer(text, (int)wc3_legacy_arg32(c, 2), &r);
    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);
    if (!r.digits) {
        c->eax = 0;
        return;
    }

    const uint32_t limit = r.negative ? 0x80000000u : 0x7FFFFFFFu;
    if (r.overflow || r.magnitude > limit) {
        c->eax = r.negative ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
    c->eax = r.negative ? (uint32_t)(-(int64_t)(uint32_t)r.magnitude)
                        : (uint32_t)r.magnitude;
}

static void h_wcstoul(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    ParsedInteger r;

    (void)parse_wide_integer(text, (int)wc3_legacy_arg32(c, 2), &r);
    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);
    if (!r.digits) {
        c->eax = 0;
        return;
    }
    if (r.overflow || r.magnitude > 0xFFFFFFFFull) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    c->eax = r.negative ? (uint32_t)(0u - r.magnitude) : (uint32_t)r.magnitude;
}

static void h_wcstoll(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    ParsedInteger r;

    (void)parse_wide_integer(text, (int)wc3_legacy_arg32(c, 2), &r);
    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);
    if (!r.digits) {
        set_edx_eax(c, 0u);
        return;
    }

    const uint64_t limit = r.negative ? 0x8000000000000000ull
                                      : 0x7FFFFFFFFFFFFFFFull;
    if (r.overflow || r.magnitude > limit) {
        set_edx_eax(c, r.negative ? 0x8000000000000000ull
                                  : 0x7FFFFFFFFFFFFFFFull);
        return;
    }
    set_edx_eax(c, r.negative ? 0u - r.magnitude : r.magnitude);
}

static void h_wcstoull(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    ParsedInteger r;

    (void)parse_wide_integer(text, (int)wc3_legacy_arg32(c, 2), &r);
    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);
    if (!r.digits) {
        set_edx_eax(c, 0u);
        return;
    }
    if (r.overflow) {
        set_edx_eax(c, UINT64_MAX);
        return;
    }
    set_edx_eax(c, r.negative ? 0u - r.magnitude : r.magnitude);
}

static void h_wcstod(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    char buffer[WIDE_BUFFER];
    char *host_end = NULL;

    const uint32_t units = copy_guest_wide(text, buffer, WIDE_BUFFER);
    const double value = strtod(buffer, &host_end);

    if (endptr && wc3_legacy_guest_ok(endptr, 4)) {
        uint32_t consumed = 0;
        if (host_end && host_end >= buffer) {
            const size_t offset = (size_t)(host_end - buffer);
            if (offset > units) consumed = units;
            else consumed = (uint32_t)offset;
        }
        wc3_legacy_wr32g(endptr, text + consumed * 2u);
    }
    fpush(c, x87_from_f64(value));
}

static void h_atof(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    char buffer[ATOF_BUFFER];

    copy_guest_text(text, buffer, ATOF_BUFFER);

    /* Host strtod has the same decimal grammar in the C locale and performs
     * correct rounding.  The legacy handler returned zero for every input. */
    const double value = strtod(buffer, NULL);
    fpush(c, x87_from_f64(value));
}

static void h_strtod(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    char buffer[ATOF_BUFFER];
    char *host_end = NULL;

    const uint32_t length = copy_guest_text(text, buffer, ATOF_BUFFER);
    const double value = strtod(buffer, &host_end);

    if (endptr && wc3_legacy_guest_ok(endptr, 4)) {
        uint32_t consumed = 0;
        if (host_end && host_end >= buffer) {
            const size_t offset = (size_t)(host_end - buffer);
            if (offset > length) consumed = length;
            else consumed = (uint32_t)offset;
        }
        wc3_legacy_wr32g(endptr, text + consumed);
    }

    fpush(c, x87_from_f64(value));
}

static void h_strtol(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    const ParsedInteger r = parse_integer(text, (int)wc3_legacy_arg32(c, 2));

    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);

    if (!r.digits) {
        c->eax = 0;
        return;
    }

    const uint32_t limit = r.negative ? 0x80000000u : 0x7FFFFFFFu;
    if (r.overflow || r.magnitude > limit) {
        c->eax = r.negative ? 0x80000000u : 0x7FFFFFFFu;
        return;
    }
    c->eax = r.negative ? (uint32_t)(-(int64_t)(uint32_t)r.magnitude)
                        : (uint32_t)r.magnitude;
}

static void set_edx_eax(CPU *c, uint64_t value)
{
    c->eax = (uint32_t)value;
    c->edx = (uint32_t)(value >> 32);
}

static void h_strtoll(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    const ParsedInteger r = parse_integer(text, (int)wc3_legacy_arg32(c, 2));

    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);

    if (!r.digits) {
        set_edx_eax(c, 0u);
        return;
    }

    const uint64_t limit = r.negative ? 0x8000000000000000ull
                                      : 0x7FFFFFFFFFFFFFFFull;
    if (r.overflow || r.magnitude > limit) {
        set_edx_eax(c, r.negative ? 0x8000000000000000ull
                                  : 0x7FFFFFFFFFFFFFFFull);
        return;
    }
    set_edx_eax(c, r.negative ? 0u - r.magnitude : r.magnitude);
}

static void h_strtoull(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    const ParsedInteger r = parse_integer(text, (int)wc3_legacy_arg32(c, 2));

    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);

    if (!r.digits) {
        set_edx_eax(c, 0u);
        return;
    }
    if (r.overflow) {
        set_edx_eax(c, UINT64_MAX);
        return;
    }
    set_edx_eax(c, r.negative ? 0u - r.magnitude : r.magnitude);
}

static void h_strtoul(CPU *c)
{
    const uint32_t text = wc3_legacy_arg32(c, 0);
    const uint32_t endptr = wc3_legacy_arg32(c, 1);
    const ParsedInteger r = parse_integer(text, (int)wc3_legacy_arg32(c, 2));

    if (endptr && wc3_legacy_guest_ok(endptr, 4))
        wc3_legacy_wr32g(endptr, r.end);

    if (!r.digits) {
        c->eax = 0;
        return;
    }
    if (r.overflow || r.magnitude > 0xFFFFFFFFull) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    c->eax = r.negative ? (uint32_t)(0u - r.magnitude) : (uint32_t)r.magnitude;
}

static void h_wcstombs(CPU *c)
{
    const uint32_t dst = wc3_legacy_arg32(c, 0);
    const uint32_t src = wc3_legacy_arg32(c, 1);
    const uint32_t count = wc3_legacy_arg32(c, 2);

    /* A null destination asks only for the C-locale length.  The count is
     * ignored in that form, matching the CRT. */
    if (!dst) {
        uint32_t units = 0;
        for (;;) {
            const uint32_t off = units * 2u;
            if (units > 0x7FFFFFFFu || src > UINT32_MAX - off ||
                !wc3_legacy_guest_ok(src + off, 2)) {
                c->eax = WIDE_FAIL;
                return;
            }
            const uint16_t wc = wc3_legacy_rd16g(src + off);
            if (!wc) {
                c->eax = units;
                return;
            }
            if (wc > 0xFFu) {
                c->eax = WIDE_FAIL;
                return;
            }
            units++;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t off = i * 2u;
        if (i > 0x7FFFFFFFu || src > UINT32_MAX - off ||
            !wc3_legacy_guest_ok(src + off, 2)) {
            c->eax = WIDE_FAIL;
            return;
        }
        const uint16_t wc = wc3_legacy_rd16g(src + off);
        if (wc > 0xFFu) {
            c->eax = WIDE_FAIL;
            return;
        }
        if (dst > UINT32_MAX - i || !wc3_legacy_guest_ok(dst + i, 1)) {
            c->eax = WIDE_FAIL;
            return;
        }
        if (!wc) {
            wc3_legacy_wr8g(dst + i, 0u);
            c->eax = i;
            return;
        }
        wc3_legacy_wr8g(dst + i, (uint8_t)wc);
    }
    c->eax = count;
}

static void h_ismbblead(CPU *c)
{
    /* CP1251 is SBCS; every byte stands alone. */
    (void)c;
    c->eax = 0;
}

static void h_ismbcspace(CPU *c)
{
    const uint8_t ch = (uint8_t)wc3_legacy_arg32(c, 0);
    c->eax = ascii_space(ch) ? 1u : 0u;
}

typedef struct ConvertOverride {
    const char *name;
    void (*impl)(CPU *c);
} ConvertOverride;

static const ConvertOverride g_overrides[] = {
    { "atof", h_atof },
    { "strtol", h_strtol },
    { "strtoul", h_strtoul },
    { "_ismbblead", h_ismbblead },
    { "_ismbcspace", h_ismbcspace },
};

typedef struct ConvertAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} ConvertAddition;

static const ConvertAddition g_additions[] = {
    { "strtod", 2, h_strtod },
    { "strtoll", 3, h_strtoll },
    { "strtoull", 3, h_strtoull },
    { "_strtoi64", 3, h_strtoll },
    { "wcstod", 2, h_wcstod },
    { "wcstol", 3, h_wcstol },
    { "wcstoul", 3, h_wcstoul },
    { "wcstoll", 3, h_wcstoll },
    { "wcstoull", 3, h_wcstoull },
    { "wcstombs", 3, h_wcstombs },
};

int msvcr80_convert_install(void)
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
