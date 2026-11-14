/*
 * T0: the HLE dynamic-range table keeps exact coalescing when the most recent
 * slot is not the matching one.  The last-slot check is an accelerator only:
 * the full scan must still extend an older entry and the page index must
 * describe the widened range.
 *
 * White-box test: the implementation is included so the exact table and the
 * one-byte page index can be observed directly.  All public range queries are
 * checked through guest_range_ok().
 */
#include "../runtime/hle/android/guest_mem.c"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check_u32(const char *what, uint32_t want, uint32_t got)
{
    if (want != got) {
        fprintf(stderr, "FAIL %s: want 0x%08X, got 0x%08X\n", what, want,
                got);
        failures++;
    }
}

static void check_int(const char *what, int want, int got)
{
    if (want != got) {
        fprintf(stderr, "FAIL %s: want %d, got %d\n", what, want, got);
        failures++;
    }
}

static void expect_range(int i, uint32_t lo, uint32_t hi)
{
    check_u32("range lo", lo, g_extra[i].lo);
    check_u32("range hi", hi, g_extra[i].hi);
}

static void expect_page(uint32_t addr, uint8_t want)
{
    uint8_t got = __atomic_load_n(&g_extra_page[addr >> GUEST_PAGE_SHIFT],
                                  __ATOMIC_ACQUIRE);
    check_u32("page hint", want, got);
}

static void expect_ok(uint32_t base, uint32_t size, int want)
{
    check_int("range_ok", want, guest_range_ok(base, size));
}

static void reset_ranges(void)
{
    memset(g_extra, 0, sizeof g_extra);
    memset(g_extra_page, 0, sizeof g_extra_page);
    g_extra_n = 0;
}

/* The common lock path extends the slot it just claimed. */
static void test_last_slot_extend(void)
{
    reset_ranges();
    guest_range_add(0x10000000u, 0x1000u);
    guest_range_add(0x10001000u, 0x1000u);

    check_int("last-slot count", 1, g_extra_n);
    expect_range(0, 0x10000000u, 0x10002000u);
    expect_page(0x10000000u, 1u);
    expect_page(0x10001000u, 1u);
    expect_ok(0x10000000u, 0x2000u, 1);
}

/* A new range must not steal an older matching slot when the last slot is
 * unrelated; this is the full-scan fallback the fast path must preserve. */
static void test_older_slot_fallback(void)
{
    reset_ranges();
    guest_range_add(0x20000000u, 0x1000u);
    guest_range_add(0x30000000u, 0x1000u);
    guest_range_add(0x20001000u, 0x1000u);

    check_int("fallback count", 2, g_extra_n);
    expect_range(0, 0x20000000u, 0x20002000u);
    expect_range(1, 0x30000000u, 0x30001000u);
    expect_page(0x20001000u, 1u);
    expect_page(0x30000000u, 1u);
    expect_ok(0x20000000u, 0x2000u, 1);
    expect_ok(0x30000000u, 0x1000u, 1);
}

/* A claimed slot may still be zero-filled by another writer.  The accelerator
 * and the full scan both skip it; the new range goes to the next slot. */
static void test_unfilled_last_slot(void)
{
    reset_ranges();
    g_extra_n = 1;
    guest_range_add(0x40000000u, 0x1000u);

    check_int("unfilled count", 2, g_extra_n);
    expect_range(0, 0u, 0u);
    expect_range(1, 0x40000000u, 0x40001000u);
    expect_page(0x40000000u, 1u);
    expect_ok(0x40000000u, 0x1000u, 1);
}

/* Widening an old slot must mark the newly covered pages, not only the tail
 * of the range that happened to be passed in. */
static void test_page_index_tracks_widening(void)
{
    reset_ranges();
    guest_range_add(0x50000FFFu, 0x2u);
    check_int("partial count", 1, g_extra_n);
    expect_page(0x50000000u, 0u);
    expect_ok(0x50000FFFu, 0x2u, 1);

    guest_range_add(0x50001000u, 0x1000u);
    check_int("widened count", 1, g_extra_n);
    expect_range(0, 0x50000FFFu, 0x50002000u);
    expect_page(0x50001000u, 1u);
    expect_ok(0x50000FFFu, 0x1001u, 1);
    expect_ok(0x50001000u, 0x1000u, 1);
}

/* Zero and wrapping inputs are rejected before a slot is claimed or a page is
 * marked. */
static void test_invalid_inputs(void)
{
    reset_ranges();
    guest_range_add(0u, 0x1000u);
    guest_range_add(0x60000000u, 0u);
    guest_range_add(0xFFFFF000u, 0x2000u);

    check_int("invalid count", 0, g_extra_n);
    expect_page(0x60000000u, 0u);
    expect_page(0xFFFFF000u, 0u);
    expect_ok(0x60000000u, 0x1000u, 0);
}

int main(void)
{
    test_last_slot_extend();
    test_older_slot_fallback();
    test_unfilled_last_slot();
    test_page_index_tracks_widening();
    test_invalid_inputs();

    if (failures != 0) {
        fprintf(stderr, "hle guest memory: %d failure(s)\n", failures);
        return 1;
    }
    printf("hle guest memory: ok\n");
    return 0;
}
