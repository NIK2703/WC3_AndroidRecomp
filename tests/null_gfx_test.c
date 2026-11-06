/*
 * null_gfx_test.c - host-side T0 checks for the verification frontend.
 *
 * The test exercises the actual null_stream.c and null_invariants.c code, not
 * a mock.  It covers V5.9's three invariants, the UNAVAILABLE distinction and
 * the V5.4 self-check that an altered canonical stream changes its digest.
 */
#include "null_gfx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void fill_nonzero(uint8_t *p, uint32_t n, uint8_t value)
{
    for (uint32_t i = 0; i < n; i++) p[i] = value;
}

static int create_buffers(Wc3NullGfx *g, uint32_t vb, uint32_t ib,
                          uint32_t vertices, uint32_t stride)
{
    uint16_t indices[6] = {0, 1, 2, 0, 1, 2};
    if (vertices > 4u || stride > 32u) return -1;
    if (wc3_null_gfx_resource(g, vb, WC3_NULL_GFX_VERTEX_BUFFER,
                              vertices * stride, stride) != 0)
        return -1;
    if (wc3_null_gfx_resource(g, ib, WC3_NULL_GFX_INDEX_BUFFER,
                              (uint32_t)sizeof indices, 2u) != 0)
        return -1;
    return 0;
}

static int setup_buffers(Wc3NullGfx *g, uint32_t vb, uint32_t ib,
                         uint32_t vertices, uint32_t stride)
{
    uint8_t data[4u * 32u];
    uint16_t indices[6] = {0, 1, 2, 0, 1, 2};
    if (create_buffers(g, vb, ib, vertices, stride) != 0) return -1;
    fill_nonzero(data, vertices * stride, 0xA5u);
    if (wc3_null_gfx_write(g, vb, 0, data, vertices * stride) != 0)
        return -1;
    if (wc3_null_gfx_write(g, ib, 0, indices, (uint32_t)sizeof indices) != 0)
        return -1;
    return 0;
}

static void test_stream(void)
{
    Wc3NullGfx *a = wc3_null_gfx_create(0u);
    Wc3NullGfx *b = wc3_null_gfx_create(0u);
    check(a && b, "stream create");
    if (!a || !b) {
        wc3_null_gfx_destroy(a);
        wc3_null_gfx_destroy(b);
        return;
    }

    const uint32_t args[3] = {7u, 0x12345678u, 0xDEADBEEFu};
    check(wc3_null_gfx_emit(a, 0x100u, 0x151u, args, 3u) == 0,
          "stream emit");
    check(wc3_null_gfx_state(a, 7u, 1u) == 0, "stream state");
    check(wc3_null_gfx_emit(a, 0x100u, 0x151u, args, 3u) == 0,
          "stream emit after state");
    check(wc3_null_gfx_event_count(a) == 2u, "stream event count");
    check(wc3_null_gfx_stream_hash(a) != 0u, "stream hash");
    const uint64_t ha = wc3_null_gfx_stream_hash(a);
    const uint64_t hstate = wc3_null_gfx_state_hash(a);
    check(hstate != 0u, "state hash");

    check(wc3_null_gfx_emit(b, 0x100u, 0x151u, args, 3u) == 0,
          "stream replay emit");
    check(wc3_null_gfx_state(b, 7u, 1u) == 0, "stream replay state");
    check(wc3_null_gfx_emit(b, 0x100u, 0x151u, args, 3u) == 0,
          "stream replay emit 2");
    check(wc3_null_gfx_stream_hash(b) == ha, "stream replay hash");
    check(wc3_null_gfx_state_hash(b) == hstate, "stream replay state hash");

    uint32_t altered[3] = {7u, 0x12345678u, 0xDEADBEEEu};
    check(wc3_null_gfx_emit(b, 0x100u, 0x151u, altered, 3u) == 0,
          "stream altered emit");
    check(wc3_null_gfx_stream_hash(b) != ha, "stream altered hash");

    check(wc3_null_gfx_emit(a, 0u, 0u, NULL, 9u) == -1,
          "stream argc limit");
    check(wc3_null_gfx_emit(a, 0u, 0u, NULL, 1u) == -1,
          "stream null args");
    check(wc3_null_gfx_event_count(a) == 2u, "stream event count unchanged");

    wc3_null_gfx_reset_stream(a);
    check(wc3_null_gfx_event_count(a) == 0u, "stream reset count");
    check(wc3_null_gfx_state_hash(a) == 0u ||
              wc3_null_gfx_state_hash(a) != hstate,
          "stream reset state");
    check(wc3_null_gfx_stream_hash(b) != wc3_null_gfx_stream_hash(a),
          "stream reset digest");

    wc3_null_gfx_destroy(a);
    wc3_null_gfx_destroy(b);
}

static void test_valid_draw(void)
{
    Wc3NullGfx *g = wc3_null_gfx_create(0u);
    check(g != NULL, "valid create");
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "valid buffers");
    const uint16_t indices[3] = {0, 1, 2};
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "valid draw");
    Wc3NullGfxReport r;
    wc3_null_gfx_report(g, &r);
    check(r.draw_count == 1u, "valid draw count");
    check(r.locked_range_failures == 0u, "valid locked failures");
    check(r.declared_range_failures == 0u, "valid declared failures");
    check(r.degenerate_ratio_failures == 0u, "valid degenerate failures");
    check(r.primitive_total == 1u && r.degenerate_total == 0u,
          "valid primitive counters");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_PASS,
          "valid locked check");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_PASS,
          "valid declared check");
    check(wc3_null_gfx_check_degenerate(g) == WC3_NULL_GFX_CHECK_PASS,
          "valid degenerate check");
    wc3_null_gfx_destroy(g);
}

static void test_locked_range(void)
{
    Wc3NullGfx *g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "locked buffers");
    uint8_t data[96];
    fill_nonzero(data, sizeof data, 0x5Au);
    check(wc3_null_gfx_lock(g, 1u, 0u, sizeof data, 0u) == 0,
          "locked lock");
    check(wc3_null_gfx_unlock(g, 1u, data, sizeof data) == 0,
          "locked unlock");
    const uint16_t indices[3] = {0, 1, 2};
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "locked draw");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_PASS,
          "locked pass");
    wc3_null_gfx_destroy(g);

    /* An all-zero staging buffer is not evidence of a write. */
    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(create_buffers(g, 1u, 2u, 3u, 32u) == 0, "zero buffers");
    uint8_t zero[96] = {0};
    check(wc3_null_gfx_lock(g, 1u, 0u, sizeof zero, 0u) == 0,
          "zero lock");
    check(wc3_null_gfx_unlock(g, 1u, zero, sizeof zero) == 0,
          "zero unlock");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "zero draw");
    Wc3NullGfxReport r;
    wc3_null_gfx_report(g, &r);
    check(r.locked_range_failures == 2u, "zero locked failure");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_FAIL,
          "zero locked check");
    wc3_null_gfx_destroy(g);

    /* A read-only lock must not manufacture a write. */
    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(create_buffers(g, 1u, 2u, 3u, 32u) == 0, "readonly buffers");
    check(wc3_null_gfx_lock(g, 1u, 0u, sizeof data,
                            WC3_NULL_GFX_LOCK_READONLY) == 0,
          "readonly lock");
    check(wc3_null_gfx_unlock(g, 1u, data, sizeof data) == 0,
          "readonly unlock");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "readonly draw");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_FAIL,
          "readonly locked check");
    wc3_null_gfx_destroy(g);
}

static void test_declared_range(void)
{
    const uint16_t indices[3] = {0, 1, 2};

    Wc3NullGfx *g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "declared buffers");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 4u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "declared overrun draw");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_FAIL,
          "declared overrun check");
    wc3_null_gfx_destroy(g);

    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "declared index buffers");
    const uint16_t bad[3] = {0, 1, 3};
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              bad, sizeof bad) == 0,
          "declared index draw");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_FAIL,
          "declared index check");
    wc3_null_gfx_destroy(g);

    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "declared base buffers");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, -1, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, sizeof indices) == 0,
          "declared negative base draw");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_FAIL,
          "declared negative base check");
    wc3_null_gfx_destroy(g);

    /* Index data shorter than the declared draw. */
    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "declared short buffers");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              indices, 4u) == 0,
          "declared short draw");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_FAIL,
          "declared short check");
    wc3_null_gfx_destroy(g);
}

static void test_degenerate(void)
{
    Wc3NullGfx *g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "degenerate buffers");
    const uint16_t degenerate[3] = {0, 0, 1};
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              degenerate, sizeof degenerate) == 0,
          "degenerate draw");
    Wc3NullGfxReport r;
    wc3_null_gfx_report(g, &r);
    check(r.degenerate_total == 1u, "degenerate total");
    check(wc3_null_gfx_check_degenerate(g) == WC3_NULL_GFX_CHECK_FAIL,
          "degenerate check");
    wc3_null_gfx_destroy(g);

    g = wc3_null_gfx_create(1000000u);
    if (!g) return;
    uint8_t data[4u * 32u];
    fill_nonzero(data, sizeof data, 0x33u);
    uint16_t ib[12] = {0, 0, 1, 0, 1, 2, 0, 1, 2, 0, 1, 2};
    check(wc3_null_gfx_resource(g, 1u, WC3_NULL_GFX_VERTEX_BUFFER,
                                (uint32_t)sizeof data, 32u) == 0,
          "degenerate threshold vb");
    check(wc3_null_gfx_resource(g, 2u, WC3_NULL_GFX_INDEX_BUFFER,
                                (uint32_t)sizeof ib, 2u) == 0,
          "degenerate threshold ib");
    check(wc3_null_gfx_write(g, 1u, 0, data, (uint32_t)sizeof data) == 0,
          "degenerate threshold vb write");
    check(wc3_null_gfx_write(g, 2u, 0, ib, (uint32_t)sizeof ib) == 0,
          "degenerate threshold ib write");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 4u, 0u, 4u, WC3_NULL_GFX_TRIANGLELIST,
              ib, (uint32_t)sizeof ib) == 0,
          "degenerate threshold draw");
    check(wc3_null_gfx_check_degenerate(g) == WC3_NULL_GFX_CHECK_PASS,
          "degenerate threshold pass");
    wc3_null_gfx_destroy(g);
}

static void test_unavailable(void)
{
    Wc3NullGfx *g = wc3_null_gfx_create(0u);
    check(g != NULL, "unavailable create");
    if (!g) return;
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_UNAVAILABLE,
          "no draw locked");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_UNAVAILABLE,
          "no draw declared");
    check(wc3_null_gfx_check_degenerate(g) ==
              WC3_NULL_GFX_CHECK_UNAVAILABLE,
          "no draw degenerate");
    check(setup_buffers(g, 1u, 2u, 3u, 32u) == 0, "unavailable buffers");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 3u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              NULL, 6u) == 0,
          "missing index draw");
    check(wc3_null_gfx_check_declared(g) == WC3_NULL_GFX_CHECK_PASS,
          "missing index declared");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_PASS,
          "missing index locked");
    check(wc3_null_gfx_check_degenerate(g) ==
              WC3_NULL_GFX_CHECK_UNAVAILABLE,
          "missing index degenerate");
    wc3_null_gfx_destroy(g);

    /* More write intervals than the fixed table can hold. */
    g = wc3_null_gfx_create(0u);
    if (!g) return;
    check(wc3_null_gfx_resource(g, 1u, WC3_NULL_GFX_VERTEX_BUFFER,
                                512u, 1u) == 0,
          "overflow vb");
    check(wc3_null_gfx_resource(g, 2u, WC3_NULL_GFX_INDEX_BUFFER,
                                6u, 2u) == 0,
          "overflow ib");
    const uint8_t one = 1u;
    for (uint32_t i = 0; i < 65u; i++) {
        check(wc3_null_gfx_lock(g, 1u, i * 2u, 1u, 0u) == 0,
              "overflow lock");
        check(wc3_null_gfx_unlock(g, 1u, &one, 1u) == 0,
              "overflow unlock");
    }
    const uint16_t idx[3] = {0, 1, 2};
    check(wc3_null_gfx_write(g, 2u, 0, idx, sizeof idx) == 0,
          "overflow ib write");
    check(wc3_null_gfx_draw_indexed(
              g, 1u, 2u, 0, 0u, 512u, 0u, 1u, WC3_NULL_GFX_TRIANGLELIST,
              idx, sizeof idx) == 0,
          "overflow draw");
    Wc3NullGfxReport r;
    wc3_null_gfx_report(g, &r);
    check(r.coverage_unknown == 1u, "overflow unknown report");
    check(wc3_null_gfx_check_locked(g) == WC3_NULL_GFX_CHECK_UNAVAILABLE,
          "overflow locked unavailable");
    wc3_null_gfx_destroy(g);
}

int main(void)
{
    test_stream();
    test_valid_draw();
    test_locked_range();
    test_declared_range();
    test_degenerate();
    test_unavailable();
    if (failures) {
        printf("null gfx: %d failure(s)\n", failures);
        return 1;
    }
    printf("null gfx: OK\n");
    return 0;
}
