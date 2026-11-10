/*
 * T0 tests for the optional W3L/PvPGN/rubattle compat helpers.
 *
 * These tests exercise the native hash and the 0x83 reply builder.  They do
 * not need the lifted corpus, Game.dll or a device.  Guest memory is mapped at
 * low fixed addresses so uint32_t guest pointers remain valid on a 64-bit host.
 */
#include "w3l_compat.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define TEST_BASE 0x10000000u
#define TEST_SIZE 0x20000u
#define PACKET_GLOBAL 0x6FD77CB8u
#define PACKET_PAGE 0x6FD70000u
#define PACKET_SIZE 0x10000u

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "w3l_compat_test: FAIL: %s\n", what);
    failures++;
}

static void *map_fixed(uint32_t addr, size_t size)
{
#ifdef _WIN32
    return VirtualAlloc((void *)(uintptr_t)addr, size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int check_hex(const uint8_t *got, const char *expected)
{
    size_t i;
    for (i = 0; i < 20u; i++) {
        int hi = hex_nibble(expected[i * 2u]);
        int lo = hex_nibble(expected[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        if (got[i] != (uint8_t)((hi << 4) | lo)) {
            return 0;
        }
    }
    return expected[40] == '\0';
}

static void hash_case(
    uint32_t src,
    uint32_t dst,
    const uint8_t *account,
    size_t account_len,
    const char *expected,
    const char *label
)
{
    CPU c;
    uint8_t got[20];
    uint8_t got_src[20];
    uint32_t i;
    uint32_t ret;

    memset(&c, 0, sizeof(c));
    for (i = 0; i < 16u; i++) {
        wr8(src + 0x20u + i, 0u);
    }
    for (i = 0; i < account_len; i++) {
        wr8(src + 0x20u + i, account[i]);
    }
    /* The wrapper copies exactly 20 bytes.  dst+0x10 is the last output
     * dword; dst+0x14 is the first byte outside the contract. */
    wr32(dst + 0x14u, 0xDEADBEEFu);
    for (i = 0; i < 20u; i++) {
        wr8(src + 0xA8u + i, 0xA5u);
    }

    ret = wc3_w3l_pvpgn_hash(&c, dst, src);
    if (ret != 1u) {
        fail(label);
        return;
    }
    for (i = 0; i < 20u; i++) {
        got[i] = rd8(dst + i);
        got_src[i] = rd8(src + 0xA8u + i);
    }
    if (!check_hex(got, expected)) {
        fail(label);
        return;
    }
    if (memcmp(got, got_src, 20u) != 0) {
        fail(label);
        return;
    }
    if (rd32(dst + 0x14u) != 0xDEADBEEFu) {
        fail("hash wrote past dst+0x14");
    }
}

static void test_hash(uint32_t base)
{
    static const uint8_t test_upper[] = { 'T', 'E', 'S', 'T' };
    static const uint8_t test_mixed[] = { 'T', 'e', 'S', 't' };
    static const uint8_t test_lower[] = { 't', 'e', 's', 't' };
    static const uint8_t with_nul[] = { 'A', 'B', 0, 'C', 'D' };
    static const uint8_t sixteen[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P'
    };
    uint32_t src = base + 0x1000u;
    uint32_t dst = base + 0x2000u;

    /* wl27 0x100018E0 lowercases A..Z and 0x100011D0 uses strlen. */
    hash_case(src, dst, test_upper, sizeof(test_upper),
              "b6a5bad9ad62fe495c4c389e737ea23165415f9c", "hash TEST");
    hash_case(src, dst, test_mixed, sizeof(test_mixed),
              "b6a5bad9ad62fe495c4c389e737ea23165415f9c", "hash TeSt");
    hash_case(src, dst, test_lower, sizeof(test_lower),
              "b6a5bad9ad62fe495c4c389e737ea23165415f9c", "hash test");
    hash_case(src, dst, with_nul, sizeof(with_nul),
              "cb49e316fe95dee366ea19070db16b041795adb9", "hash AB\\0CD");
    hash_case(src, dst, sixteen, sizeof(sixteen),
              "f33889ae514716d9a2c05e2ee9eb2982ad4fe420",
              "hash ABCDEFGHIJKLMNOP");
}

static uint32_t dispatched_target;
static uint32_t dispatched_eax;
static uint32_t dispatched_ebx;
static uint32_t dispatched_ecx;
static uint32_t dispatched_edx;
static uint32_t dispatched_esi;
static uint32_t dispatched_edi;
static uint32_t dispatched_ebp;
static uint32_t dispatched_buf;
static uint32_t dispatched_len;
static int dispatch_count;

void dispatch(CPU *c, uint32_t target)
{
    dispatched_target = target;
    dispatched_eax = c->eax;
    dispatched_ebx = c->ebx;
    dispatched_ecx = c->ecx;
    dispatched_edx = c->edx;
    dispatched_esi = c->esi;
    dispatched_edi = c->edi;
    dispatched_ebp = c->ebp;
    dispatched_buf = rd32(c->esp + 4u);
    dispatched_len = rd32(c->esp + 8u);
    dispatch_count++;
    /* The real vtable method is stdcall with two stack arguments. */
    c->esp += 12u;
    c->eax = 0u;
}

static void reset_dispatch(void)
{
    dispatched_target = 0u;
    dispatched_eax = 0u;
    dispatched_ebx = 0u;
    dispatched_ecx = 0u;
    dispatched_edx = 0u;
    dispatched_esi = 0u;
    dispatched_edi = 0u;
    dispatched_ebp = 0u;
    dispatched_buf = 0u;
    dispatched_len = 0u;
    dispatch_count = 0;
}

static void setup_0x83(
    uint32_t base,
    uint32_t *frame_out,
    uint32_t *caller_ebp_out,
    uint32_t *saved_esi_out,
    uint32_t *packet_out,
    uint32_t *conn_packet_out,
    uint32_t *conn_call_out
)
{
    uint32_t frame = base + 0x8000u;
    uint32_t packet = base + 0x9000u;
    uint32_t conn_packet = base + 0xA000u;
    uint32_t conn_call = base + 0xB000u;
    uint32_t vtable = base + 0xC000u;
    uint32_t caller_ebp = base + 0x7000u;
    uint32_t saved_esi = 0xCAFEBABEu;
    static const char name[] = "name";
    uint32_t i;

    memset((void *)(uintptr_t)base, 0, TEST_SIZE);
    wr32(PACKET_GLOBAL, packet);

    wr32(frame, caller_ebp);
    wr32(frame - 4u, saved_esi);
    wr32(packet + 0x1FCu, conn_packet);
    wr8(packet + 0x100u, 8u);
    wr8(packet + 0x101u, 0u);
    for (i = 0; i < 8u; i++) {
        wr8(packet + 0x102u + i, (uint8_t)('A' + i));
    }

    wr32(conn_packet + 0x08u, 0x11223344u);
    for (i = 0; i < 32u; i++) {
        wr8(conn_packet + 0x110u + i, (uint8_t)(0x80u + i));
    }
    for (i = 0; i < (uint32_t)(sizeof(name)); i++) {
        wr8(conn_packet + 0x0Cu + i, (uint8_t)name[i]);
    }

    wr32(conn_call, vtable);
    wr32(vtable + 0x2Cu, 0x6F900000u);

    *frame_out = frame;
    *caller_ebp_out = caller_ebp;
    *saved_esi_out = saved_esi;
    *packet_out = packet;
    *conn_packet_out = conn_packet;
    *conn_call_out = conn_call;
}

static void init_cpu(CPU *c, uint32_t frame, uint32_t conn_call)
{
    memset(c, 0, sizeof(*c));
    c->ebp = frame;
    c->esp = frame - 8u;
    c->esi = 0x11111111u;
    c->edi = 0x22222222u;
    c->ebx = 0x33333333u;
    c->ecx = conn_call;
}

static void check_frame(
    const CPU *c,
    uint32_t frame,
    uint32_t caller_ebp,
    uint32_t saved_esi,
    uint32_t expected_eax,
    const char *label
)
{
    /* L_6F8DC630: push ebp; push esi.  Handler 0x10001B10 pops both and
     * executes ret 0x10, so the final guest esp is frame + 4 + 0x10. */
    if (c->esp != frame + 0x18u) {
        fail(label);
    }
    if (c->ebp != caller_ebp) {
        fail(label);
    }
    if (c->esi != saved_esi) {
        fail(label);
    }
    if (c->ebx != 0x33333333u || c->edi != 0x22222222u) {
        fail(label);
    }
    if (c->eax != expected_eax) {
        fail(label);
    }
}

static void test_0x83_success(uint32_t base)
{
    CPU c;
    uint32_t frame, caller_ebp, saved_esi, packet, conn_packet, conn_call;
    uint32_t buf;
    uint32_t i;
    static const char name[] = "name";

    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);
    init_cpu(&c, frame, conn_call);
    reset_dispatch();

    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u, "0x83 success frame");
    if (dispatch_count != 1) {
        fail("0x83 success dispatch count");
        return;
    }
    if (dispatched_target != 0x6F900000u
        || dispatched_eax != base + 0xC000u
        || dispatched_ebx != 4u
        || dispatched_ecx != conn_call
        || dispatched_edx != dispatched_buf
        || dispatched_esi != conn_packet + 0x0Cu + 4u
        || dispatched_edi != dispatched_buf + 0x3Bu - 1u
        || dispatched_ebp != conn_packet + 0x0Cu) {
        fail("0x83 success vtable arguments");
    }
    if (dispatched_len != 0x3Bu) {
        fail("0x83 success reply length");
        return;
    }
    buf = dispatched_buf;
    if (rd8(buf + 0u) != 0xFFu || rd8(buf + 1u) != 0x83u
        || rd8(buf + 2u) != 0x08u || rd8(buf + 3u) != 0x00u
        || rd8(buf + 4u) != 0u || rd8(buf + 5u) != 0u
        || rd8(buf + 6u) != 0u || rd8(buf + 7u) != 0u) {
        fail("0x83 success header");
    }
    if (rd32(buf + 8u) != 0x11223344u) {
        fail("0x83 success connection id");
    }
    for (i = 0; i < 32u; i++) {
        if (rd8(buf + 0x0Cu + i) != (uint8_t)(0x80u + i)) {
            fail("0x83 success tail");
            break;
        }
    }
    if (rd8(buf + 0x2Cu) != 0x08u || rd8(buf + 0x2Du) != 0x00u) {
        fail("0x83 success data length copy");
    }
    for (i = 0; i < 8u; i++) {
        if (rd8(buf + 0x2Eu + i) != (uint8_t)('A' + i)) {
            fail("0x83 success data");
            break;
        }
    }
    for (i = 0; i < (uint32_t)(sizeof(name)); i++) {
        if (rd8(buf + 0x2Eu + 8u + i) != (uint8_t)name[i]) {
            fail("0x83 success name");
            break;
        }
    }
}

static void test_0x83_failures(uint32_t base)
{
    CPU c;
    uint32_t frame, caller_ebp, saved_esi, packet, conn_packet, conn_call;
    uint32_t i;

    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);

    /* data_len below 7 */
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr8(packet + 0x100u, 6u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 short data frame");
    if (dispatch_count != 0) {
        fail("0x83 short data dispatched");
    }

    /* data_len above 0x80 */
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr8(packet + 0x100u, 0x81u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 long data frame");
    if (dispatch_count != 0) {
        fail("0x83 long data dispatched");
    }

    /* non-terminated name in the first 0x104 bytes */
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr8(packet + 0x100u, 8u);
    for (i = 0; i < 0x104u; i++) {
        wr8(conn_packet + 0x0Cu + i, (uint8_t)'A');
    }
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 long name frame");
    if (dispatch_count != 0) {
        fail("0x83 long name dispatched");
    }

    /* incoming ecx is the send object; a null value returns eax=0 before the
     * packet global is read. */
    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);
    init_cpu(&c, frame, 0u);
    reset_dispatch();
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 0u,
                "0x83 null send object frame");
    if (dispatch_count != 0) {
        fail("0x83 null send object dispatched");
    }

    /* global packet object missing: worker returns 1 */
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr32(PACKET_GLOBAL, 0u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 null packet frame");
    if (dispatch_count != 0) {
        fail("0x83 null packet dispatched");
    }

    /* packet pointer with the sign bit: `jle` rejects it without reading. */
    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr32(PACKET_GLOBAL, 0x80000000u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 signed packet frame");
    if (dispatch_count != 0) {
        fail("0x83 signed packet dispatched");
    }

    /* connection pointer with the sign bit: `jle` rejects it without reading. */
    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr32(packet + 0x1FCu, 0x80000000u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 signed conn frame");
    if (dispatch_count != 0) {
        fail("0x83 signed conn dispatched");
    }

    /* wl27 0x10001C40 does not guard the vtable target; a null target is
     * dispatched and faults in the original, so it must not be skipped. */
    setup_0x83(base, &frame, &caller_ebp, &saved_esi, &packet,
               &conn_packet, &conn_call);
    init_cpu(&c, frame, conn_call);
    reset_dispatch();
    wr32(base + 0xC000u + 0x2Cu, 0u);
    wc3_w3l_handle_0x83(&c);
    check_frame(&c, frame, caller_ebp, saved_esi, 1u,
                "0x83 null target frame");
    if (dispatch_count != 1 || dispatched_target != 0u
        || dispatched_eax != base + 0xC000u) {
        fail("0x83 null target was not dispatched");
    }
}

int main(void)
{
    void *base;
    void *packet_page;

    base = map_fixed(TEST_BASE, TEST_SIZE);
    packet_page = map_fixed(PACKET_PAGE, PACKET_SIZE);
    if (base == NULL || packet_page == NULL) {
        fprintf(stderr, "w3l_compat_test: unavailable: fixed guest mapping\n");
        return 2;
    }

    test_hash(TEST_BASE);
    test_0x83_success(TEST_BASE);
    test_0x83_failures(TEST_BASE);

    if (failures != 0) {
        fprintf(stderr, "w3l_compat_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("w3l_compat_test: ok\n");
    return 0;
}
