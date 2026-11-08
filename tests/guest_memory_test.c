/*
 * T0: the dynamic guest-range page hint must stay a hint.
 *
 * This is a white-box test: it includes the implementation so it can observe
 * the internal one-byte page table directly.  The public range check is still
 * exercised for every acceptance and rejection; the page table is only an
 * additional assertion about the fast path's bookkeeping.
 */
#include "../runtime/guest/guest_memory.c"

#include <stdint.h>
#include <stdio.h>

static int failures;

static void fail_u8(const char *what, uint32_t addr, uint8_t want,
                    uint8_t got)
{
    fprintf(stderr, "FAIL %s at 0x%08X: want 0x%02X, got 0x%02X\n", what, addr,
            want, got);
    failures++;
}

static void expect_page(uint32_t addr, uint8_t want)
{
    uint8_t got = atomic_load_explicit(&g_dyn_page_id[addr >> GUEST_PAGE_SHIFT],
                                       memory_order_relaxed);
    if (got != want) fail_u8("page hint", addr, want, got);
}

static void expect_ok(uint32_t base, uint32_t size, int want)
{
    int got = wc3_guest_range_ok(base, size);
    if (got != want) {
        fprintf(stderr, "FAIL range_ok(0x%08X, 0x%X): want %d, got %d\n", base,
                size, want, got);
        failures++;
    }
}

static void reset_ranges(void)
{
    for (uint32_t i = 0; i < GUEST_RANGE_MAX; i++)
        atomic_store_explicit(&g_ranges[i], 0u, memory_order_relaxed);
    atomic_store_explicit(&g_range_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_range_overflow, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_range_epoch, 0u, memory_order_relaxed);
    for (uint32_t i = 0; i < GUEST_PAGE_COUNT; i++)
        atomic_store_explicit(&g_dyn_page_id[i], 0u, memory_order_relaxed);
    g_cached_range = 0u;
    g_cached_epoch = 0u;
}

static void test_aligned_merge_and_split(void)
{
    reset_ranges();

    /* A page-aligned range marks exactly its fully covered pages. */
    wc3_guest_range_add(0x20001000u, 0x3000u);
    expect_page(0x20000000u, 0u);
    expect_page(0x20001000u, 1u);
    expect_page(0x20002000u, 1u);
    expect_page(0x20003000u, 1u);
    expect_page(0x20004000u, 0u);
    expect_ok(0x20001000u, 0x3000u, 1);
    expect_ok(0x20002000u, 0x2000u, 1);
    expect_ok(0x20000FFFu, 0x3001u, 0);

    /* Re-adding the same range must not turn its own page id into overlap. */
    wc3_guest_range_add(0x20001000u, 0x3000u);
    expect_page(0x20001000u, 1u);
    expect_page(0x20002000u, 1u);
    expect_page(0x20003000u, 1u);

    /* Adjacent ranges coalesce into the same slot and keep the same id. */
    wc3_guest_range_add(0x20004000u, 0x1000u);
    expect_page(0x20004000u, 1u);
    expect_ok(0x20001000u, 0x4000u, 1);

    /* Removing the first page clears only that page's hint. */
    if (!range_remove(0x20001000u, 0x1000u)) {
        fprintf(stderr, "FAIL range_remove first page\n");
        failures++;
    }
    expect_page(0x20001000u, 0u);
    expect_page(0x20002000u, 1u);
    expect_page(0x20003000u, 1u);
    expect_page(0x20004000u, 1u);
    expect_ok(0x20001000u, 0x1000u, 0);
    expect_ok(0x20002000u, 0x3000u, 1);
}

static void test_split_remainders(void)
{
    reset_ranges();

    /* Two coalesced pages in slot 0. */
    wc3_guest_range_add(0x20010000u, 0x1000u);
    wc3_guest_range_add(0x20011000u, 0x1000u);
    expect_page(0x20010000u, 1u);
    expect_page(0x20011000u, 1u);

    /* A range in slot 1, then punch a hole in its middle.  The left remainder
     * keeps slot 1, the right remainder is appended in slot 2. */
    wc3_guest_range_add(0x20020000u, 0x4000u);
    expect_page(0x20020000u, 2u);
    expect_page(0x20021000u, 2u);
    expect_page(0x20022000u, 2u);
    expect_page(0x20023000u, 2u);
    if (!range_remove(0x20021000u, 0x1000u)) {
        fprintf(stderr, "FAIL range_remove middle page\n");
        failures++;
    }
    expect_page(0x20020000u, 2u);
    expect_page(0x20021000u, 0u);
    expect_page(0x20022000u, 3u);
    expect_page(0x20023000u, 3u);
    expect_ok(0x20020000u, 0x1000u, 1);
    expect_ok(0x20021000u, 0x1000u, 0);
    expect_ok(0x20022000u, 0x2000u, 1);
    expect_ok(0x20020000u, 0x3000u, 0);

    /* Removing the remainders clears their pages, including the split slots. */
    range_remove(0x20020000u, 0x1000u);
    expect_page(0x20020000u, 0u);
    range_remove(0x20022000u, 0x2000u);
    expect_page(0x20022000u, 0u);
    expect_page(0x20023000u, 0u);
    expect_ok(0x20022000u, 0x2000u, 0);
}

static void test_partial_pages(void)
{
    reset_ranges();

    /* No page is wholly covered, so no hint may be published. */
    wc3_guest_range_add(0x30000001u, 0x2u);
    expect_page(0x30000000u, 0u);
    expect_ok(0x30000001u, 0x2u, 1);

    /* A later aligned range on the next page is independent. */
    wc3_guest_range_add(0x30001000u, 0x1000u);
    expect_page(0x30001000u, 2u);
    expect_ok(0x30001000u, 0x1000u, 1);
    expect_ok(0x30000001u, 0x2u, 1);

    range_remove(0x30000001u, 0x2u);
    expect_page(0x30000000u, 0u);
    expect_page(0x30001000u, 2u);
    range_remove(0x30001000u, 0x1000u);
    expect_page(0x30001000u, 0u);
}

static void test_stale_hint_cannot_widen(void)
{
    reset_ranges();

    /* A hint naming an empty slot must fall back to the exact scan. */
    atomic_store_explicit(&g_dyn_page_id[0x40000000u >> GUEST_PAGE_SHIFT], 1u,
                          memory_order_relaxed);
    expect_ok(0x40000000u, 0x1000u, 0);

    /* A hint naming a real slot with different bounds must also fall back. */
    wc3_guest_range_add(0x40001000u, 0x1000u);
    atomic_store_explicit(&g_dyn_page_id[0x40000000u >> GUEST_PAGE_SHIFT], 1u,
                          memory_order_relaxed);
    expect_ok(0x40000000u, 0x1000u, 0);
    expect_ok(0x40001000u, 0x1000u, 1);

    /* An overlap marker always uses the exact scan, even when the range is
     * genuinely present. */
    atomic_store_explicit(&g_dyn_page_id[0x40001000u >> GUEST_PAGE_SHIFT],
                          GUEST_DYN_PAGE_OVERFLOW, memory_order_relaxed);
    g_cached_range = 0u;
    g_cached_epoch = 0u;
    expect_ok(0x40001000u, 0x1000u, 1);

    range_remove(0x40001000u, 0x1000u);
    expect_page(0x40001000u, GUEST_DYN_PAGE_OVERFLOW);
    expect_ok(0x40001000u, 0x1000u, 0);
}

int main(void)
{
    test_aligned_merge_and_split();
    test_split_remainders();
    test_partial_pages();
    test_stale_hint_cannot_widen();

    if (failures != 0) {
        fprintf(stderr, "guest memory: %d failure(s)\n", failures);
        return 1;
    }
    printf("guest memory: ok\n");
    return 0;
}
