/*
 * ws2_32_hle_test.c - T0 contract for the 1.27 winsock HLE.
 *
 * The real ws2_32_hle.c is linked into this test.  A bounded fake guest
 * arena, a fake Wc3NetApi and a fake as-built registry drive the same static
 * handlers that the product installs, so the test covers:
 *
 *   - the WS2_32/WSOCK32 ordinal split at 10/11/12;
 *   - WSAStartup version negotiation and WSADATA layout;
 *   - guest SOCKET lifecycle in the reserved handle range;
 *   - sockaddr and fd_set conversion;
 *   - gethostbyname / getaddrinfo guest layouts;
 *   - SO_DONTLINGER and the mswsock extension pointers;
 *   - the real AcceptEx background accept plus the completion-port bridge.
 *
 * No device, corpus or game assets are required.
 */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <pthread.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "platform.h"
#include "ws2_32_hle.h"

#define GUEST_BASE       0x20000000u
#define GUEST_SIZE       0x00100000u
#define CALL_STACK       0x20070000u
#define HEAP_BASE        0x20080000u
#define HEAP_SIZE        0x00020000u

#define SCRATCH_WSADATA  0x20001000u
#define SCRATCH_SOCKADDR 0x20002000u
#define SCRATCH_FDSET    0x20003000u
#define SCRATCH_TIMEVAL  0x20003100u
#define SCRATCH_NAME     0x20004000u
#define SCRATCH_HINTS    0x20004100u
#define SCRATCH_RESULT   0x20004200u
#define SCRATCH_OPTION   0x20004300u
#define SCRATCH_GUID     0x20004400u
#define SCRATCH_IOCTL_OUT 0x20004500u
#define SCRATCH_IOCTL_LEN 0x20004510u
#define SCRATCH_OVERLAP  0x20005000u
#define SCRATCH_ACCEPT   0x20005100u
#define SCRATCH_BYTES    0x20005200u
#define SCRATCH_OUTLEN   0x20005300u
#define SCRATCH_LEN      0x20005310u
#define SCRATCH_LOCAL    0x20005320u
#define SCRATCH_REMOTE   0x20005330u
#define SCRATCH_FLAGS    0x20005340u
#define SCRATCH_WSABUF   0x20005400u
#define SCRATCH_IO_DATA  0x20005500u
#define SCRATCH_WSABUF2  0x20005600u
#define SCRATCH_BYTES2   0x20005620u
#define SCRATCH_FLAGS2   0x20005624u

#define WSA_SOL_SOCKET   0xFFFFu
#define WSA_SO_DONTLINGER 0xFFFFFF7Fu
#define WSA_SO_UPDATE_ACCEPT_CONTEXT 0x700Bu
#define WSA_SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006u
#define WSA_AF_INET      2u
#define WSA_SOCK_STREAM  1u
#define WSA_IO_PENDING   997u
#define TEST_WSAENOTSOCK      10038u
#define TEST_WSAEAFNOSUPPORT  10047u
#define TEST_WSAEOPNOTSUPP    10045u
#define TEST_WSAEINVAL        10022u
#define TEST_WSAEWOULDBLOCK   10035u
#define TEST_WSAENOBUFS       10055u

#define FAKE_PORT        0x00091000u
#define FAKE_PORT_KEY    0x55u
#define FAKE_ACCEPT_EX   0x7F000010u
#define FAKE_GET_SOCKADDRS 0x7F000020u

#define IP4(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))

#define REQUIRE(cond)                                                     \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "ws2_32 hle: %s:%d: %s\n", __FILE__, __LINE__, \
                    #cond);                                               \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static uint8_t *g_mem;
static uint32_t g_heap_used;
static uint32_t g_free_calls;
static uint32_t g_alloc_calls;

static uint8_t *guest_ptr(uint32_t addr, uint32_t size)
{
    if (addr < GUEST_BASE) return NULL;
    uint64_t off = (uint64_t)addr - GUEST_BASE;
    if (off > GUEST_SIZE || (uint64_t)size > GUEST_SIZE - off) return NULL;
    return g_mem + off;
}

int wc3_legacy_guest_ok(uint32_t addr, uint32_t size)
{
    return guest_ptr(addr, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 1u);
    return p ? *p : 0u;
}

uint16_t wc3_legacy_rd16g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 2u);
    uint16_t v = 0u;
    if (p) memcpy(&v, p, sizeof v);
    return v;
}

uint32_t wc3_legacy_rd32g(uint32_t addr)
{
    uint8_t *p = guest_ptr(addr, 4u);
    uint32_t v = 0u;
    if (p) memcpy(&v, p, sizeof v);
    return v;
}

void wc3_legacy_wr8g(uint32_t addr, uint8_t value)
{
    uint8_t *p = guest_ptr(addr, 1u);
    if (p) *p = value;
}

void wc3_legacy_wr16g(uint32_t addr, uint16_t value)
{
    uint8_t *p = guest_ptr(addr, 2u);
    if (p) memcpy(p, &value, sizeof value);
}

void wc3_legacy_wr32g(uint32_t addr, uint32_t value)
{
    uint8_t *p = guest_ptr(addr, 4u);
    if (p) memcpy(p, &value, sizeof value);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0u;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

uint32_t wc3_legacy_guest_alloc0(uint32_t size)
{
    if (!size) size = 1u;
    uint32_t rounded = (size + 15u) & ~15u;
    if (g_heap_used > HEAP_SIZE || rounded > HEAP_SIZE - g_heap_used) return 0;
    uint32_t addr = HEAP_BASE + g_heap_used;
    g_heap_used += rounded;
    g_alloc_calls++;
    memset(guest_ptr(addr, size), 0, size);
    return addr;
}

void wc3_legacy_guest_free(uint32_t addr)
{
    if (addr) g_free_calls++;
}

void wc3_legacy_guest_zero(uint32_t addr, uint32_t size)
{
    uint8_t *p = guest_ptr(addr, size);
    if (p && size) memset(p, 0, size);
}

static uint32_t g_last_error;

void wc3_legacy_set_last_error(CPU *c, uint32_t code) { (void)c; g_last_error = code; }
uint32_t wc3_legacy_get_last_error(CPU *c) { (void)c; return g_last_error; }

/* ---- fake as-built registry ------------------------------------------- */

typedef struct FakeRow {
    const char *dll;
    const char *name;
    int argc;
    void (*fn)(CPU *c);
} FakeRow;

typedef struct FakeOrdinal {
    const char *dll;
    uint16_t ordinal;
    int argc;
    void (*fn)(CPU *c);
} FakeOrdinal;

static FakeRow g_rows[256];
static unsigned g_row_count;
static FakeOrdinal g_ordinals[128];
static unsigned g_ordinal_count;

static void (*g_chain_create_iocp)(CPU *c);
static void (*g_chain_gqcs)(CPU *c);
static void (*g_chain_close_handle)(CPU *c);

static void fake_prev_create_iocp(CPU *c);
static void fake_prev_gqcs(CPU *c);
static void fake_prev_close(CPU *c);
static void fake_prev_read_file(CPU *c);
static void fake_prev_write_file(CPU *c);
static void fake_prev_load_library(CPU *c);
static void fake_prev_proc_address(CPU *c);
static void fake_prev_module(CPU *c);
static void fake_prev_module_name(CPU *c);

int wc3_legacy_hle_add(const char *dll, const char *name, int argc,
                       int cdecl_abi, void (*impl)(CPU *c))
{
    (void)cdecl_abi;
    if (!dll || !name || !impl || g_row_count >= 256u) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].argc = argc;
    g_rows[g_row_count].fn = impl;
    g_row_count++;
    return 0;
}

int wc3_legacy_hle_bind_ordinal(const char *dll, uint16_t ordinal, int argc,
                                int cdecl_abi, void (*impl)(CPU *c))
{
    (void)cdecl_abi;
    if (!dll || !impl || g_ordinal_count >= 128u) return -1;
    g_ordinals[g_ordinal_count].dll = dll;
    g_ordinals[g_ordinal_count].ordinal = ordinal;
    g_ordinals[g_ordinal_count].argc = argc;
    g_ordinals[g_ordinal_count].fn = impl;
    g_ordinal_count++;
    return 0;
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c), void (**previous)(CPU *c))
{
    if (!dll || !name || !impl || !previous) return -1;
    FakeRow *row = NULL;
    for (unsigned i = 0; i < g_row_count; i++) {
        if (strcmp(g_rows[i].dll, dll) == 0 &&
            strcmp(g_rows[i].name, name) == 0) {
            row = &g_rows[i];
            break;
        }
    }
    if (!row || !row->fn) return -1;
    *previous = row->fn;
    row->fn = impl;
    if (strcmp(name, "CreateIoCompletionPort") == 0)
        g_chain_create_iocp = impl;
    else if (strcmp(name, "GetQueuedCompletionStatus") == 0)
        g_chain_gqcs = impl;
    else if (strcmp(name, "CloseHandle") == 0)
        g_chain_close_handle = impl;
    return 0;
}

uint32_t wc3_legacy_hle_thunk_of(const char *dll, const char *name)
{
    if (!dll || !name) return 0;
    if (strcmp(dll, "ws2_32.dll") == 0) {
        if (strcmp(name, "WSARecv") == 0) return 0x7F000050u;
        if (strcmp(name, "WSASend") == 0) return 0x7F000054u;
        if (strcmp(name, "WSAIoctl") == 0) return 0x7F000058u;
    }
    if (strcmp(dll, "mswsock.dll") != 0) return 0;
    if (strcmp(name, "AcceptEx") == 0) return FAKE_ACCEPT_EX;
    if (strcmp(name, "GetAcceptExSockaddrs") == 0) return FAKE_GET_SOCKADDRS;
    if (strcmp(name, "WSARecvEx") == 0) return 0x7F000030u;
    return 0;
}

static const FakeRow *find_named(const char *dll, const char *name)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (strcmp(g_rows[i].dll, dll) == 0 &&
            strcmp(g_rows[i].name, name) == 0)
            return &g_rows[i];
    return NULL;
}

static const FakeOrdinal *find_ordinal(const char *dll, uint16_t ordinal)
{
    for (unsigned i = 0; i < g_ordinal_count; i++)
        if (strcmp(g_ordinals[i].dll, dll) == 0 &&
            g_ordinals[i].ordinal == ordinal)
            return &g_ordinals[i];
    return NULL;
}

/* ---- fake platform: guest arena and sockets --------------------------- */

typedef struct FakeSock {
    int used;
    int listening;
    int accepted;
    uint32_t local_addr;
    uint16_t local_port;
    uint32_t peer_addr;
    uint16_t peer_port;
} FakeSock;

#define FAKE_SOCK_BASE 0x5000u
#define FAKE_SOCK_MAX  40

static FakeSock g_socks[FAKE_SOCK_MAX];
static _Atomic int g_accept_ready;
static _Atomic uint32_t g_recv_length;
static uint8_t g_recv_bytes[256];
static _Atomic uint32_t g_sent_length;
static uint8_t g_sent_bytes[256];
static int g_connect_result;

static void sleep_ms(uint32_t ms);
static uint32_t g_closed_sockets;
static uint32_t g_linger_value;
static int g_startup_calls;

static FakeSock *sock_from_handle(Wc3NetSocket s)
{
    if (s < FAKE_SOCK_BASE || s >= FAKE_SOCK_BASE + FAKE_SOCK_MAX) return NULL;
    FakeSock *f = &g_socks[s - FAKE_SOCK_BASE];
    return f->used ? f : NULL;
}

static void put_sockaddr(uint8_t *dst, uint32_t ip, uint16_t port)
{
    memset(dst, 0, 16u);
    dst[0] = 2;
    dst[1] = 0;
    dst[2] = (uint8_t)(port >> 8);
    dst[3] = (uint8_t)(port & 0xFFu);
    memcpy(dst + 4, &ip, 4u);
}

static int fake_net_startup(void *ctx) { (void)ctx; g_startup_calls++; return 0; }
static void fake_net_cleanup(void *ctx) { (void)ctx; }

static int fake_net_socket(void *ctx, int family, int type, int protocol,
                           Wc3NetSocket *out)
{
    (void)ctx; (void)family; (void)type; (void)protocol;
    for (unsigned i = 0; i < FAKE_SOCK_MAX; i++) {
        if (g_socks[i].used) continue;
        memset(&g_socks[i], 0, sizeof g_socks[i]);
        g_socks[i].used = 1;
        g_socks[i].local_addr = IP4(192, 168, 1, 5);
        g_socks[i].local_port = 0x1234u;
        if (out) *out = (Wc3NetSocket)(FAKE_SOCK_BASE + i);
        return 0;
    }
    return -WC3_NET_ERR_NOBUFS;
}

static int fake_net_close(void *ctx, Wc3NetSocket s)
{
    (void)ctx;
    FakeSock *f = sock_from_handle(s);
    if (!f) return -WC3_NET_ERR_BADF;
    memset(f, 0, sizeof *f);
    g_closed_sockets++;
    return 0;
}

static int fake_net_bind(void *ctx, Wc3NetSocket s, const void *addr,
                         uint32_t addr_length)
{
    (void)ctx;
    FakeSock *f = sock_from_handle(s);
    if (!f || !addr || addr_length < 16u) return -WC3_NET_ERR_INVAL;
    const uint8_t *raw = (const uint8_t *)addr;
    if (raw[0] != 2 || raw[1] != 0) return -WC3_NET_ERR_AFNOSUPPORT;
    f->local_port = (uint16_t)((raw[2] << 8) | raw[3]);
    memcpy(&f->local_addr, raw + 4, 4u);
    return 0;
}

static int fake_net_connect(void *ctx, Wc3NetSocket s, const void *addr,
                            uint32_t addr_length)
{
    (void)ctx;
    FakeSock *f = sock_from_handle(s);
    if (!f || !addr || addr_length < 16u) return -WC3_NET_ERR_INVAL;
    if (g_connect_result) return g_connect_result;
    const uint8_t *raw = (const uint8_t *)addr;
    f->peer_port = (uint16_t)((raw[2] << 8) | raw[3]);
    memcpy(&f->peer_addr, raw + 4, 4u);
    return 0;
}

static int fake_net_listen(void *ctx, Wc3NetSocket s, int backlog)
{
    (void)ctx; (void)backlog;
    FakeSock *f = sock_from_handle(s);
    if (!f) return -WC3_NET_ERR_BADF;
    f->listening = 1;
    return 0;
}

static int fake_net_accept(void *ctx, Wc3NetSocket s, Wc3NetSocket *out,
                           void *addr, uint32_t *addr_length)
{
    (void)ctx; (void)addr; (void)addr_length;
    FakeSock *listener = sock_from_handle(s);
    if (!listener) return -WC3_NET_ERR_BADF;
    if (!g_accept_ready) return -WC3_NET_ERR_AGAIN;
    for (unsigned i = 0; i < FAKE_SOCK_MAX; i++) {
        if (g_socks[i].used) continue;
        memset(&g_socks[i], 0, sizeof g_socks[i]);
        g_socks[i].used = 1;
        g_socks[i].accepted = 1;
        g_socks[i].local_addr = listener->local_addr;
        g_socks[i].local_port = listener->local_port;
        g_socks[i].peer_addr = IP4(10, 0, 0, 2);
        g_socks[i].peer_port = 0x5678u;
        if (out) *out = (Wc3NetSocket)(FAKE_SOCK_BASE + i);
        return 0;
    }
    return -WC3_NET_ERR_NOBUFS;
}

static int fake_net_send(void *ctx, Wc3NetSocket s, const void *buf,
                         uint32_t length, int flags, uint32_t *sent)
{
    (void)ctx; (void)flags;
    if (!sock_from_handle(s)) return -WC3_NET_ERR_BADF;
    uint32_t copy = length < sizeof g_sent_bytes ? length
                                                  : (uint32_t)sizeof g_sent_bytes;
    if (buf && copy) memcpy(g_sent_bytes, buf, copy);
    atomic_store(&g_sent_length, copy);
    if (sent) *sent = length;
    return 0;
}

static int fake_net_recv(void *ctx, Wc3NetSocket s, void *buf,
                         uint32_t length, int flags, uint32_t *received)
{
    (void)ctx; (void)flags;
    if (!sock_from_handle(s)) return -WC3_NET_ERR_BADF;
    uint32_t available = atomic_load(&g_recv_length);
    if (!available) {
        if (received) *received = 0;
        return -WC3_NET_ERR_AGAIN;
    }
    uint32_t copy = length < available ? length : available;
    if (copy && buf) memcpy(buf, g_recv_bytes, copy);
    if (copy < available)
        memmove(g_recv_bytes, g_recv_bytes + copy, available - copy);
    atomic_store(&g_recv_length, available - copy);
    if (received) *received = copy;
    return 0;
}

static int fake_net_sendto(void *ctx, Wc3NetSocket s, const void *buf,
                           uint32_t length, int flags, const void *addr,
                           uint32_t addr_length, uint32_t *sent)
{
    (void)ctx; (void)buf; (void)flags; (void)addr; (void)addr_length;
    if (!sock_from_handle(s)) return -WC3_NET_ERR_BADF;
    if (sent) *sent = length;
    return 0;
}

static int fake_net_recvfrom(void *ctx, Wc3NetSocket s, void *buf,
                             uint32_t length, int flags, void *addr,
                             uint32_t *addr_length, uint32_t *received)
{
    (void)ctx; (void)buf; (void)length; (void)flags; (void)addr;
    (void)addr_length;
    if (!sock_from_handle(s)) return -WC3_NET_ERR_BADF;
    if (received) *received = 0;
    return 0;
}

static int fake_net_shutdown(void *ctx, Wc3NetSocket s, int how)
{
    (void)ctx; (void)how;
    return sock_from_handle(s) ? 0 : -WC3_NET_ERR_BADF;
}

static int fake_net_names(void *ctx, Wc3NetSocket s, void *addr,
                          uint32_t *addr_length, int peer)
{
    (void)ctx;
    FakeSock *f = sock_from_handle(s);
    if (!f || !addr || !addr_length) return -WC3_NET_ERR_INVAL;
    if (*addr_length < 16u) return -WC3_NET_ERR_INVAL;
    put_sockaddr((uint8_t *)addr, peer ? f->peer_addr : f->local_addr,
                 peer ? f->peer_port : f->local_port);
    *addr_length = 16u;
    return 0;
}

static int fake_net_getpeername(void *ctx, Wc3NetSocket s, void *addr,
                                uint32_t *addr_length)
{
    return fake_net_names(ctx, s, addr, addr_length, 1);
}

static int fake_net_getsockname(void *ctx, Wc3NetSocket s, void *addr,
                                uint32_t *addr_length)
{
    return fake_net_names(ctx, s, addr, addr_length, 0);
}

static int fake_net_setsockopt(void *ctx, Wc3NetSocket s, int option,
                               const void *value, uint32_t length)
{
    (void)ctx; (void)length;
    if (!sock_from_handle(s) || !value) return -WC3_NET_ERR_INVAL;
    if (option == WC3_NET_OPT_LINGER) {
        const uint8_t *raw = (const uint8_t *)value;
        g_linger_value = (uint32_t)raw[0];
    }
    return 0;
}

static int fake_net_getsockopt(void *ctx, Wc3NetSocket s, int option,
                               void *value, uint32_t *length)
{
    (void)ctx;
    if (!sock_from_handle(s) || !value || !length || *length < 4u)
        return -WC3_NET_ERR_INVAL;
    if (option == WC3_NET_OPT_LINGER) {
        memset(value, 0, 4u);
    } else {
        uint32_t v = option == WC3_NET_OPT_TYPE ? 1u : 0u;
        memcpy(value, &v, 4u);
    }
    *length = 4u;
    return 0;
}

static int fake_net_set_nonblocking(void *ctx, Wc3NetSocket s, int enable)
{
    (void)ctx; (void)enable;
    return sock_from_handle(s) ? 0 : -WC3_NET_ERR_BADF;
}

static int fake_net_bytes_available(void *ctx, Wc3NetSocket s, uint32_t *out)
{
    (void)ctx;
    if (!sock_from_handle(s) || !out) return -WC3_NET_ERR_BADF;
    *out = 0;
    return 0;
}

static int fake_net_at_mark(void *ctx, Wc3NetSocket s, int *out)
{
    (void)ctx;
    if (!sock_from_handle(s) || !out) return -WC3_NET_ERR_BADF;
    *out = 0;
    return 0;
}

static int fake_net_select(void *ctx, Wc3NetFdSet *read, Wc3NetFdSet *write,
                           Wc3NetFdSet *except, int64_t timeout_us)
{
    (void)ctx; (void)except;
    Wc3NetSocket rinput[WC3_NET_FD_SETSIZE];
    Wc3NetSocket winput[WC3_NET_FD_SETSIZE];
    uint32_t rn = 0, wn = 0;
    if (read) {
        rn = read->count;
        if (rn > WC3_NET_FD_SETSIZE) rn = WC3_NET_FD_SETSIZE;
        if (rn) memcpy(rinput, read->fd, rn * sizeof rinput[0]);
        read->count = 0;
    }
    if (write) {
        wn = write->count;
        if (wn > WC3_NET_FD_SETSIZE) wn = WC3_NET_FD_SETSIZE;
        if (wn) memcpy(winput, write->fd, wn * sizeof winput[0]);
        write->count = 0;
    }

    int ready = 0;
    int readable = atomic_load(&g_accept_ready) ||
                   atomic_load(&g_recv_length) > 0u;
    if (read && readable) {
        for (uint32_t i = 0; i < rn; i++) {
            if (sock_from_handle(rinput[i])) {
                read->fd[read->count++] = rinput[i];
                ready++;
            }
        }
    }
    if (write) {
        for (uint32_t i = 0; i < wn; i++) {
            if (sock_from_handle(winput[i])) {
                write->fd[write->count++] = winput[i];
                ready++;
            }
        }
    }
    if (!ready && timeout_us != 0) sleep_ms(1);
    return ready;
}

static int fake_net_hostname(void *ctx, char *buf, uint32_t cap)
{
    (void)ctx;
    if (!buf || cap < 10u) return -WC3_NET_ERR_INVAL;
    memcpy(buf, "test-host", 10u);
    return 0;
}

static void fill_resolve_entry(Wc3NetAddrInfo *out, uint32_t ip, uint16_t port,
                               const char *canon)
{
    memset(out, 0, sizeof *out);
    out->family = WC3_NET_AF_INET;
    out->socktype = WC3_NET_SOCK_STREAM;
    out->protocol = 6;
    out->addr_length = 16u;
    put_sockaddr(out->addr, ip, port);
    if (canon) {
        size_t n = strlen(canon);
        if (n >= sizeof out->canonname) n = sizeof out->canonname - 1u;
        memcpy(out->canonname, canon, n);
        out->canonname[n] = '\0';
    }
}

static int fake_net_resolve(void *ctx, const char *node, const char *service,
                            const Wc3NetResolveHints *hints,
                            Wc3NetAddrInfo *out, uint32_t cap, uint32_t *count)
{
    (void)ctx; (void)hints;
    if (!out || !count || cap == 0u) return -WC3_NET_ERR_INVAL;
    uint16_t port = service ? (uint16_t)atoi(service) : 0u;
    if (node && strcmp(node, "multi.test") == 0) {
        if (cap < 2u) return -WC3_NET_ERR_INVAL;
        fill_resolve_entry(&out[0], IP4(192, 168, 1, 5), port, node);
        fill_resolve_entry(&out[1], IP4(10, 1, 2, 3), port, node);
        *count = 2u;
        return 0;
    }
    fill_resolve_entry(&out[0], IP4(192, 168, 1, 5), port,
                       node ? node : "test-host");
    *count = 1u;
    return 0;
}

static int fake_net_interfaces(void *ctx, Wc3NetInterface *out, uint32_t cap,
                               uint32_t *count)
{
    (void)ctx;
    if (!out || !count || cap < 2u) return -WC3_NET_ERR_INVAL;
    memset(out, 0, 2u * sizeof *out);
    out[0].address = IP4(192, 168, 1, 5);
    out[0].netmask = IP4(255, 255, 255, 0);
    out[0].broadcast = IP4(192, 168, 1, 255);
    out[0].flags = WC3_NET_IF_UP;
    out[1].address = IP4(127, 0, 0, 1);
    out[1].netmask = IP4(255, 0, 0, 0);
    out[1].flags = WC3_NET_IF_UP | WC3_NET_IF_LOOPBACK;
    *count = 2u;
    return 0;
}

/* ---- fake thread API --------------------------------------------------- */

typedef struct FakeThreadStart {
    int (*entry)(void *arg);
    void *arg;
} FakeThreadStart;

static void *fake_thread_trampoline(void *opaque)
{
    FakeThreadStart *start = (FakeThreadStart *)opaque;
    (void)start->entry(start->arg);
    free(start);
    return NULL;
}

static int fake_thread_create(void *ctx, const Wc3ThreadDesc *desc,
                              Wc3ThreadHandle *out)
{
    (void)ctx;
    if (!desc || !desc->entry || !out) return -1;
    FakeThreadStart *start = (FakeThreadStart *)malloc(sizeof *start);
    pthread_t *thread = (pthread_t *)malloc(sizeof *thread);
    if (!start || !thread) {
        free(start);
        free(thread);
        return -1;
    }
    start->entry = desc->entry;
    start->arg = desc->arg;
    if (pthread_create(thread, NULL, fake_thread_trampoline, start) != 0) {
        free(start);
        free(thread);
        return -1;
    }
    *out = (Wc3ThreadHandle)(uintptr_t)thread;
    return 0;
}

static int fake_thread_detach(void *ctx, Wc3ThreadHandle thread)
{
    (void)ctx;
    pthread_t *t = (pthread_t *)(uintptr_t)thread;
    if (!t) return -1;
    (void)pthread_detach(*t);
    free(t);
    return 0;
}

/* ---- fake as-built IOCP ------------------------------------------------ */

static void sleep_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static void fake_prev_create_iocp(CPU *c)
{
    uint32_t file = wc3_legacy_arg32(c, 0);
    uint32_t port = wc3_legacy_arg32(c, 1);
    if (!file && !port) {
        wc3_legacy_set_last_error(NULL, 0u);
        c->eax = FAKE_PORT;
        return;
    }
    wc3_legacy_set_last_error(NULL, 6u);
    c->eax = 0;
}

static uint32_t g_last_gqcs_timeout;
static unsigned g_gqcs_calls;

static void fake_prev_gqcs(CPU *c)
{
    uint32_t ms = wc3_legacy_arg32(c, 4);
    g_last_gqcs_timeout = ms;
    g_gqcs_calls++;
    if (ms && ms != 0xFFFFFFFFu) sleep_ms(ms < 20u ? ms : 20u);
    wc3_legacy_wr32g(wc3_legacy_arg32(c, 1), 0u);
    wc3_legacy_wr32g(wc3_legacy_arg32(c, 2), 0u);
    wc3_legacy_wr32g(wc3_legacy_arg32(c, 3), 0u);
    wc3_legacy_set_last_error(NULL, 258u);
    c->eax = 0;
}

static void fake_prev_close(CPU *c)
{
    (void)c;
    wc3_legacy_set_last_error(NULL, 0u);
    c->eax = 1;
}

static unsigned g_prev_read_calls;
static unsigned g_prev_write_calls;

static void fake_prev_read_file(CPU *c)
{
    (void)c;
    g_prev_read_calls++;
    wc3_legacy_set_last_error(NULL, 0u);
    c->eax = 1;
}

static void fake_prev_write_file(CPU *c)
{
    (void)c;
    g_prev_write_calls++;
    wc3_legacy_set_last_error(NULL, 0u);
    c->eax = 1;
}

static void *set_accept_ready_after_delay(void *arg)
{
    (void)arg;
    sleep_ms(50);
    atomic_store(&g_accept_ready, 1);
    return NULL;
}

static void fake_prev_load_library(CPU *c) { c->eax = 0u; }
static void fake_prev_proc_address(CPU *c) { c->eax = 0u; }
static void fake_prev_module(CPU *c) { c->eax = 0u; }
static void fake_prev_module_name(CPU *c) { c->eax = 0u; }

/* ---- platform instance ------------------------------------------------- */

static Wc3Platform g_platform;

const Wc3Platform *wc3_platform(void) { return &g_platform; }

/* ---- handler invocation ------------------------------------------------ */

static CPU call_fn(void (*fn)(CPU *c), const uint32_t *args, unsigned count)
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = CALL_STACK;
    for (unsigned i = 0; i < count; i++)
        wc3_legacy_wr32g(c.esp + 4u + i * 4u, args[i]);
    fn(&c);
    return c;
}

static CPU call_named(const char *dll, const char *name,
                      const uint32_t *args, unsigned count)
{
    const FakeRow *row = find_named(dll, name);
    CPU c;
    memset(&c, 0, sizeof c);
    if (!row) {
        fprintf(stderr, "ws2_32 hle: missing row %s!%s\n", dll, name);
        c.eax = 0xDEADBEEFu;
        return c;
    }
    return call_fn(row->fn, args, count);
}

static uint32_t wsa_last_error(const char *dll)
{
    const uint32_t args[1] = { 0 };
    return call_named(dll, "WSAGetLastError", args, 0u).eax;
}

static int test_install_and_ordinals(void)
{
    /* The product chains handlers the as-built KERNEL32 table already owns.
     * Seed those rows with the same stand-ins the as-built table provides. */
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "LoadLibraryA", 1, 0,
                               fake_prev_load_library) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "GetProcAddress", 2, 0,
                               fake_prev_proc_address) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "GetModuleHandleA", 1, 0,
                               fake_prev_module) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "GetModuleFileNameA", 3, 0,
                               fake_prev_module_name) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "CreateIoCompletionPort", 4, 0,
                               fake_prev_create_iocp) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "GetQueuedCompletionStatus", 5,
                               0, fake_prev_gqcs) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "CloseHandle", 1, 0,
                               fake_prev_close) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "ReadFile", 5, 0,
                               fake_prev_read_file) == 0);
    REQUIRE(wc3_legacy_hle_add("KERNEL32.dll", "WriteFile", 5, 0,
                               fake_prev_write_file) == 0);
    REQUIRE(wc3_hle_ws2_32_install() > 0);

    const FakeRow *ws2_ioctl = find_named("WS2_32.dll", "ioctlsocket");
    const FakeRow *ws2_addr = find_named("WS2_32.dll", "inet_addr");
    const FakeRow *ws2_ntoa = find_named("WS2_32.dll", "inet_ntoa");
    const FakeRow *ws_ioctl = find_named("WSOCK32.dll", "ioctlsocket");
    const FakeRow *ws_addr = find_named("WSOCK32.dll", "inet_addr");
    const FakeRow *ws_ntoa = find_named("WSOCK32.dll", "inet_ntoa");
    REQUIRE(ws2_ioctl && ws2_addr && ws2_ntoa);
    REQUIRE(ws_ioctl && ws_addr && ws_ntoa);

    const FakeOrdinal *a = find_ordinal("WS2_32.dll", 10);
    const FakeOrdinal *b = find_ordinal("WS2_32.dll", 11);
    const FakeOrdinal *c = find_ordinal("WS2_32.dll", 12);
    REQUIRE(a && b && c);
    REQUIRE(a->fn == ws2_ioctl->fn && a->argc == 3);
    REQUIRE(b->fn == ws2_addr->fn && b->argc == 1);
    REQUIRE(c->fn == ws2_ntoa->fn && c->argc == 1);

    a = find_ordinal("WSOCK32.dll", 10);
    b = find_ordinal("WSOCK32.dll", 11);
    c = find_ordinal("WSOCK32.dll", 12);
    REQUIRE(a && b && c);
    REQUIRE(a->fn == ws_addr->fn && a->argc == 1);
    REQUIRE(b->fn == ws_ntoa->fn && b->argc == 1);
    REQUIRE(c->fn == ws_ioctl->fn && c->argc == 3);

    const FakeRow *async_select = find_named("WS2_32.dll", "WSAAsyncSelect");
    REQUIRE(async_select != NULL);
    REQUIRE(async_select->argc == 4);

    REQUIRE(find_named("mswsock.dll", "AcceptEx") != NULL);
    REQUIRE(find_named("mswsock.dll", "GetAcceptExSockaddrs") != NULL);
    REQUIRE(find_named("mswsock.dll", "WSARecvEx") != NULL);
    REQUIRE(g_chain_create_iocp != NULL);
    REQUIRE(g_chain_gqcs != NULL);
    REQUIRE(g_chain_close_handle != NULL);
    return 0;
}

static int test_wsastartup(void)
{
    uint32_t args[2] = { 0x0101u, SCRATCH_WSADATA };
    CPU c = call_named("WS2_32.dll", "WSAStartup", args, 2u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 0u) == 0x0101u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 2u) == 0x0202u);
    REQUIRE(strcmp((const char *)guest_ptr(SCRATCH_WSADATA + 4u, 1u),
                   "WinSock 1.1") == 0);

    args[0] = 0x0002u; /* MAKEWORD(2, 0): low byte is major. */
    c = call_named("WS2_32.dll", "WSAStartup", args, 2u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 0u) == 0x0200u);
    REQUIRE(strcmp((const char *)guest_ptr(SCRATCH_WSADATA + 4u, 1u),
                   "WinSock 2.0") == 0);

    args[0] = 0x0202u;
    c = call_named("WS2_32.dll", "WSAStartup", args, 2u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 0u) == 0x0202u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 390u) == 0u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_WSADATA + 392u) == 1472u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_WSADATA + 396u) == 0u);
    REQUIRE(g_startup_calls == 3);
    return 0;
}

static uint32_t make_socket(uint32_t family, uint32_t type, uint32_t protocol)
{
    const uint32_t args[3] = { family, type, protocol };
    return call_named("WS2_32.dll", "socket", args, 3u).eax;
}

static int test_socket_lifecycle(void)
{
    const uint32_t bad[3] = { 999u, WSA_SOCK_STREAM, 0u };
    CPU c = call_named("WS2_32.dll", "socket", bad, 3u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAEAFNOSUPPORT);

    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);
    REQUIRE((s & 0xFF000000u) == 0x0B000000u);

    const uint32_t close_args[1] = { s };
    c = call_named("WS2_32.dll", "closesocket", close_args, 1u);
    REQUIRE(c.eax == 0u);
    c = call_named("WS2_32.dll", "closesocket", close_args, 1u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAENOTSOCK);
    return 0;
}

static void write_name(uint32_t addr, const char *text)
{
    size_t n = strlen(text) + 1u;
    memcpy(guest_ptr(addr, (uint32_t)n), text, n);
}

static void put_guest_sockaddr(uint32_t addr, uint32_t ip, uint16_t port)
{
    put_sockaddr(guest_ptr(addr, 16u), ip, port);
}

static int test_bind_and_select(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    put_guest_sockaddr(SCRATCH_SOCKADDR, IP4(192, 168, 1, 5), 0x1234u);
    const uint32_t bind_args[3] = { s, SCRATCH_SOCKADDR, 16u };
    REQUIRE(call_named("WS2_32.dll", "bind", bind_args, 3u).eax == 0u);

    wc3_legacy_wr32g(SCRATCH_LEN, 16u);
    const uint32_t name_args[3] = { s, SCRATCH_REMOTE, SCRATCH_LEN };
    REQUIRE(call_named("WS2_32.dll", "getsockname", name_args, 3u).eax == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_LEN) == 16u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_REMOTE + 0u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_REMOTE + 2u) == 0x3412u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_REMOTE + 4u) == IP4(192, 168, 1, 5));

    /* fd_set: count is at +0, SOCKET values start at +4. */
    wc3_legacy_wr32g(SCRATCH_FDSET, 1u);
    wc3_legacy_wr32g(SCRATCH_FDSET + 4u, s);
    wc3_legacy_wr32g(SCRATCH_TIMEVAL, 0u);
    wc3_legacy_wr32g(SCRATCH_TIMEVAL + 4u, 0u);
    g_accept_ready = 1;
    const uint32_t sel_args[5] = { 1u, SCRATCH_FDSET, 0u, 0u,
                                    SCRATCH_TIMEVAL };
    CPU c = call_named("WS2_32.dll", "select", sel_args, 5u);
    REQUIRE(c.eax == 1u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FDSET) == 1u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FDSET + 4u) == s);

    wc3_legacy_wr32g(SCRATCH_FDSET, 1u);
    wc3_legacy_wr32g(SCRATCH_FDSET + 4u, s);
    g_accept_ready = 0;
    c = call_named("WS2_32.dll", "select", sel_args, 5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FDSET) == 0u);
    g_accept_ready = 1;

    const uint32_t fd_args[2] = { s, SCRATCH_FDSET };
    wc3_legacy_wr32g(SCRATCH_FDSET, 1u);
    wc3_legacy_wr32g(SCRATCH_FDSET + 4u, s);
    REQUIRE(call_named("WS2_32.dll", "__WSAFDIsSet", fd_args, 2u).eax == 1u);

    const uint32_t close_args[1] = { s };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_args, 1u).eax == 0u);
    return 0;
}

static int test_connect_inprogress(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);
    put_guest_sockaddr(SCRATCH_SOCKADDR, IP4(10, 199, 28, 62), 6112u);

    g_connect_result = -WC3_NET_ERR_INPROGRESS;
    const uint32_t args[3] = { s, SCRATCH_SOCKADDR, 16u };
    CPU c = call_named("WS2_32.dll", "connect", args, 3u);
    g_connect_result = 0;

    /* Winsock reports a nonblocking connect as WSAEWOULDBLOCK.  Reporting
     * WSAEINPROGRESS here makes the 1.27 join path discard the socket. */
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAEWOULDBLOCK);

    const uint32_t close_args[1] = { s };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_args, 1u).eax == 0u);
    return 0;
}

static int test_names(void)
{
    write_name(SCRATCH_NAME, "test.local");
    const uint32_t name_args[1] = { SCRATCH_NAME };
    CPU c = call_named("WS2_32.dll", "gethostbyname", name_args, 1u);
    REQUIRE(c.eax != 0u);
    uint32_t hostent = c.eax;
    uint32_t name_ptr = wc3_legacy_rd32g(hostent + 0u);
    uint32_t aliases = wc3_legacy_rd32g(hostent + 4u);
    uint32_t addr_list = wc3_legacy_rd32g(hostent + 12u);
    REQUIRE(name_ptr != 0u);
    REQUIRE(strcmp((const char *)guest_ptr(name_ptr, 1u), "test.local") == 0);
    REQUIRE(wc3_legacy_rd32g(aliases) == 0u);
    REQUIRE(wc3_legacy_rd16g(hostent + 8u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd16g(hostent + 10u) == 4u);
    uint32_t first_addr = wc3_legacy_rd32g(addr_list);
    REQUIRE(first_addr != 0u);
    REQUIRE(wc3_legacy_rd32g(first_addr) == IP4(192, 168, 1, 5));
    REQUIRE(wc3_legacy_rd32g(addr_list + 4u) == 0u);

    write_name(SCRATCH_NAME, "multi.test");
    wc3_legacy_wr32g(SCRATCH_HINTS + 0u, 0u);
    wc3_legacy_wr32g(SCRATCH_HINTS + 4u, WSA_AF_INET);
    wc3_legacy_wr32g(SCRATCH_HINTS + 8u, WSA_SOCK_STREAM);
    wc3_legacy_wr32g(SCRATCH_HINTS + 12u, 6u);
    write_name(SCRATCH_RESULT + 0x100u, "1434");
    uint32_t before_free = g_free_calls;
    const uint32_t gai_args[4] = { SCRATCH_NAME, SCRATCH_RESULT + 0x100u,
                                   SCRATCH_HINTS, SCRATCH_RESULT };
    c = call_named("WS2_32.dll", "getaddrinfo", gai_args, 4u);
    REQUIRE(c.eax == 0u);
    uint32_t ai = wc3_legacy_rd32g(SCRATCH_RESULT);
    REQUIRE(ai != 0u);
    REQUIRE(wc3_legacy_rd32g(ai + 0u) == 0u);
    REQUIRE(wc3_legacy_rd32g(ai + 4u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd32g(ai + 8u) == WSA_SOCK_STREAM);
    REQUIRE(wc3_legacy_rd32g(ai + 12u) == 6u);
    REQUIRE(wc3_legacy_rd32g(ai + 16u) == 16u);
    uint32_t ai_addr = wc3_legacy_rd32g(ai + 20u);
    REQUIRE(ai_addr != 0u);
    REQUIRE(wc3_legacy_rd16g(ai_addr + 0u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd16g(ai_addr + 2u) == 0x9A05u); /* 1434 = 0x059A */
    REQUIRE(wc3_legacy_rd32g(ai_addr + 4u) == IP4(192, 168, 1, 5));
    uint32_t next = wc3_legacy_rd32g(ai + 28u);
    REQUIRE(next != 0u);
    REQUIRE(wc3_legacy_rd32g(next + 4u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd32g(next + 28u) == 0u);

    const uint32_t free_args[1] = { ai };
    REQUIRE(call_named("WS2_32.dll", "freeaddrinfo", free_args, 1u).eax == 0u);
    REQUIRE(g_free_calls >= before_free + 4u);
    return 0;
}

static int test_options_and_extension(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    wc3_legacy_wr32g(SCRATCH_OPTION, 1u);
    const uint32_t set_args[5] = { s, WSA_SOL_SOCKET, WSA_SO_DONTLINGER,
                                   SCRATCH_OPTION, 4u };
    g_linger_value = 0xFFFFu;
    REQUIRE(call_named("WS2_32.dll", "setsockopt", set_args, 5u).eax == 0u);
    REQUIRE(g_linger_value == 0u);

    wc3_legacy_wr32g(SCRATCH_OPTION, 0u);
    g_linger_value = 0xFFFFu;
    REQUIRE(call_named("WS2_32.dll", "setsockopt", set_args, 5u).eax == 0u);
    REQUIRE(g_linger_value == 1u);

    /* SO_UPDATE_ACCEPT_CONTEXT is only valid after a real AcceptEx. */
    wc3_legacy_wr32g(SCRATCH_OPTION, s);
    const uint32_t ctx_args[5] = { s, WSA_SOL_SOCKET,
                                   WSA_SO_UPDATE_ACCEPT_CONTEXT,
                                   SCRATCH_OPTION, 4u };
    CPU c = call_named("WS2_32.dll", "setsockopt", ctx_args, 5u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAEINVAL);

    /* WSAID_ACCEPTEX through SIO_GET_EXTENSION_FUNCTION_POINTER. */
    static const uint8_t acceptex_guid[16] = {
        0xF1, 0x7D, 0x36, 0xB5, 0xAC, 0xCB, 0xCF, 0x11,
        0x95, 0xCA, 0x00, 0x80, 0x5F, 0x48, 0xA1, 0x92
    };
    memcpy(guest_ptr(SCRATCH_GUID, 16u), acceptex_guid, 16u);
    wc3_legacy_wr32g(SCRATCH_IOCTL_OUT, 0u);
    wc3_legacy_wr32g(SCRATCH_IOCTL_LEN, 0u);
    const uint32_t ioctl_args[9] = {
        s, WSA_SIO_GET_EXTENSION_FUNCTION_POINTER, SCRATCH_GUID, 16u,
        SCRATCH_IOCTL_OUT, 4u, SCRATCH_IOCTL_LEN, 0u, 0u
    };
    c = call_named("WS2_32.dll", "WSAIoctl", ioctl_args, 9u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_IOCTL_OUT) == FAKE_ACCEPT_EX);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_IOCTL_LEN) == 4u);

    const uint32_t close_args[1] = { s };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_args, 1u).eax == 0u);
    return 0;
}

static int test_acceptex_completion(void)
{
    uint32_t listener = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    uint32_t accept_socket = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(listener != 0xFFFFFFFFu);
    REQUIRE(accept_socket != 0xFFFFFFFFu);

    const uint32_t listen_args[2] = { listener, 4u };
    REQUIRE(call_named("WS2_32.dll", "listen", listen_args, 2u).eax == 0u);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args,
                       4u);
    REQUIRE(c.eax == FAKE_PORT);

    /* The game starts its network IOCP worker threads before the first socket
     * is associated. The created port must take the private-queue polling path
     * immediately; otherwise the first INFINITE GetQueuedCompletionStatus
     * blocks in the as-built queue and later socket completions are never
     * observed. A finite wait proves the bridge split the wait into slices
     * while it checked the private queue. */
    g_last_gqcs_timeout = 0u;
    g_gqcs_calls = 0u;
    const uint32_t idle_gqcs_args[5] = {
        FAKE_PORT, SCRATCH_LOCAL, SCRATCH_REMOTE, SCRATCH_OUTLEN, 60u
    };
    c = call_named("KERNEL32.dll", "GetQueuedCompletionStatus", idle_gqcs_args,
                   5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 258u);
    REQUIRE(g_gqcs_calls > 1u);
    REQUIRE(g_last_gqcs_timeout < 60u);

    const uint32_t assoc_args[4] = { listener, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_BYTES, 0xDEADBEEFu);
    wc3_legacy_wr32g(SCRATCH_ACCEPT, 0u);
    atomic_store(&g_accept_ready, 0);

    const uint32_t accept_ex_args[8] = {
        listener, accept_socket, SCRATCH_ACCEPT, 0u, 32u, 32u,
        SCRATCH_BYTES, SCRATCH_OVERLAP
    };
    c = call_named("mswsock.dll", "AcceptEx", accept_ex_args, 8u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);

    wc3_legacy_wr32g(SCRATCH_LOCAL, 0xDEADBEEFu);
    wc3_legacy_wr32g(SCRATCH_REMOTE, 0xDEADBEEFu);
    wc3_legacy_wr32g(SCRATCH_OUTLEN, 0xDEADBEEFu);
    pthread_t ready_thread;
    REQUIRE(pthread_create(&ready_thread, NULL, set_accept_ready_after_delay,
                           NULL) == 0);
    g_last_gqcs_timeout = 0u;
    g_gqcs_calls = 0u;
    const uint32_t gqcs_args[5] = {
        FAKE_PORT, SCRATCH_LOCAL, SCRATCH_REMOTE, SCRATCH_OUTLEN, 0xFFFFFFFFu
    };
    c = call_named("KERNEL32.dll", "GetQueuedCompletionStatus", gqcs_args, 5u);
    (void)pthread_join(ready_thread, NULL);
    REQUIRE(c.eax == 1u);
    REQUIRE(g_gqcs_calls > 0u);
    REQUIRE(g_last_gqcs_timeout != 0xFFFFFFFFu);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_LOCAL) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_REMOTE) == FAKE_PORT_KEY);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OUTLEN) == SCRATCH_OVERLAP);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 0u);

    /* The address buffer is receive + local + remote, with local at +0 and
     * remote at +local_length (32). */
    wc3_legacy_wr32g(SCRATCH_LEN + 0u, 0u);
    wc3_legacy_wr32g(SCRATCH_LEN + 4u, 0u);
    wc3_legacy_wr32g(SCRATCH_LEN + 8u, 0u);
    wc3_legacy_wr32g(SCRATCH_LEN + 12u, 0u);
    const uint32_t gas_args[8] = {
        SCRATCH_ACCEPT, 0u, 32u, 32u,
        SCRATCH_LEN + 0u, SCRATCH_LEN + 4u,
        SCRATCH_LEN + 8u, SCRATCH_LEN + 12u
    };
    c = call_named("mswsock.dll", "GetAcceptExSockaddrs", gas_args, 8u);
    REQUIRE(c.eax == 0u);
    uint32_t local_ptr = wc3_legacy_rd32g(SCRATCH_LEN + 0u);
    uint32_t local_len = wc3_legacy_rd32g(SCRATCH_LEN + 4u);
    uint32_t remote_ptr = wc3_legacy_rd32g(SCRATCH_LEN + 8u);
    uint32_t remote_len = wc3_legacy_rd32g(SCRATCH_LEN + 12u);
    REQUIRE(local_ptr == SCRATCH_ACCEPT);
    REQUIRE(local_len == 16u);
    REQUIRE(remote_ptr == SCRATCH_ACCEPT + 32u);
    REQUIRE(remote_len == 16u);
    REQUIRE(wc3_legacy_rd16g(local_ptr + 0u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd16g(local_ptr + 2u) == 0x3412u);
    REQUIRE(wc3_legacy_rd32g(local_ptr + 4u) == IP4(192, 168, 1, 5));
    REQUIRE(wc3_legacy_rd16g(remote_ptr + 0u) == WSA_AF_INET);
    REQUIRE(wc3_legacy_rd16g(remote_ptr + 2u) == 0x7856u);
    REQUIRE(wc3_legacy_rd32g(remote_ptr + 4u) == IP4(10, 0, 0, 2));

    /* The accepted guest SOCKET reuses the slot, now backed by the accepted
     * host socket; SO_UPDATE_ACCEPT_CONTEXT accepts the listener identity. */
    wc3_legacy_wr32g(SCRATCH_OPTION, listener);
    const uint32_t ctx_args[5] = { accept_socket, WSA_SOL_SOCKET,
                                   WSA_SO_UPDATE_ACCEPT_CONTEXT,
                                   SCRATCH_OPTION, 4u };
    REQUIRE(call_named("WS2_32.dll", "setsockopt", ctx_args, 5u).eax == 0u);

    wc3_legacy_wr32g(SCRATCH_LEN + 0u, 16u);
    const uint32_t name_args[3] = { accept_socket, SCRATCH_REMOTE,
                                    SCRATCH_LEN + 0u };
    REQUIRE(call_named("WS2_32.dll", "getsockname", name_args, 3u).eax == 0u);
    REQUIRE(wc3_legacy_rd16g(SCRATCH_REMOTE + 2u) == 0x3412u);

    const uint32_t close_accept[1] = { accept_socket };
    const uint32_t close_listener[1] = { listener };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_accept, 1u).eax == 0u);
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_listener, 1u).eax == 0u);

    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int test_acceptex_shape_rejection(void)
{
    uint32_t listener = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    uint32_t accept_socket = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(listener != 0xFFFFFFFFu && accept_socket != 0xFFFFFFFFu);

    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    const uint32_t args[8] = {
        listener, accept_socket, SCRATCH_ACCEPT, 1u, 32u, 32u,
        SCRATCH_BYTES, SCRATCH_OVERLAP
    };
    CPU c = call_named("mswsock.dll", "AcceptEx", args, 8u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAEOPNOTSUPP);

    wc3_legacy_wr32g(SCRATCH_OVERLAP + 16u, 0x1234u);
    const uint32_t args2[8] = {
        listener, accept_socket, SCRATCH_ACCEPT, 0u, 32u, 32u,
        SCRATCH_BYTES, SCRATCH_OVERLAP
    };
    c = call_named("mswsock.dll", "AcceptEx", args2, 8u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAEOPNOTSUPP);

    const uint32_t close_args[1] = { listener };
    const uint32_t close_args2[1] = { accept_socket };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_args, 1u).eax == 0u);
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_args2, 1u).eax == 0u);
    return 0;
}

static int test_acceptex_cancel_releases_slots(void)
{
    uint32_t listener = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(listener != 0xFFFFFFFFu);
    const uint32_t listen_args[2] = { listener, 4u };
    REQUIRE(call_named("WS2_32.dll", "listen", listen_args, 2u).eax == 0u);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);
    const uint32_t assoc_args[4] = { listener, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    atomic_store(&g_accept_ready, 0);
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_guest_zero(SCRATCH_ACCEPT, 64u);

    uint32_t accepts[33];
    for (unsigned i = 0; i < 33u; i++) {
        accepts[i] = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
        REQUIRE(accepts[i] != 0xFFFFFFFFu);
    }

    uint32_t args[8] = { listener, 0u, SCRATCH_ACCEPT, 0u, 32u, 32u,
                         SCRATCH_BYTES, SCRATCH_OVERLAP };
    for (unsigned i = 0; i < 32u; i++) {
        args[1] = accepts[i];
        c = call_named("mswsock.dll", "AcceptEx", args, 8u);
        REQUIRE(c.eax == 0u);
        REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);
    }

    /* Every pending slot is occupied. */
    args[1] = accepts[32];
    c = call_named("mswsock.dll", "AcceptEx", args, 8u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wsa_last_error("WS2_32.dll") == TEST_WSAENOBUFS);

    /* The host parks the listener with listen(..., 0) before rebuilding its
     * accept pool; that must release the pending operations. */
    const uint32_t stop_args[2] = { listener, 0u };
    REQUIRE(call_named("WS2_32.dll", "listen", stop_args, 2u).eax == 0u);
    sleep_ms(100);

    args[1] = accepts[32];
    c = call_named("mswsock.dll", "AcceptEx", args, 8u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);

    REQUIRE(call_named("WS2_32.dll", "listen", stop_args, 2u).eax == 0u);
    sleep_ms(100);

    for (unsigned i = 0; i < 33u; i++) {
        const uint32_t close_one[1] = { accepts[i] };
        REQUIRE(call_named("WS2_32.dll", "closesocket", close_one, 1u).eax == 0u);
    }
    const uint32_t close_listener[1] = { listener };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_listener, 1u).eax == 0u);
    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int wait_completion(uint32_t port, uint32_t timeout_ms,
                           uint32_t *bytes, uint32_t *key, uint32_t *ov)
{
    const uint32_t args[5] = { port, SCRATCH_LOCAL, SCRATCH_REMOTE,
                               SCRATCH_OUTLEN, timeout_ms };
    CPU c = call_named("KERNEL32.dll", "GetQueuedCompletionStatus", args, 5u);
    if (c.eax != 1u) return 0;
    if (bytes) *bytes = wc3_legacy_rd32g(SCRATCH_LOCAL);
    if (key) *key = wc3_legacy_rd32g(SCRATCH_REMOTE);
    if (ov) *ov = wc3_legacy_rd32g(SCRATCH_OUTLEN);
    return 1;
}

static int test_dynamic_wsa_exports(void)
{
    const FakeRow *recv_row = find_named("WS2_32.dll", "WSARecv");
    const FakeRow *send_row = find_named("WS2_32.dll", "WSASend");
    REQUIRE(recv_row && recv_row->argc == 7);
    REQUIRE(send_row && send_row->argc == 7);

    write_name(SCRATCH_NAME, "WSARecv");
    uint32_t args[2] = { 0x000A2000u, SCRATCH_NAME };
    CPU c = call_named("KERNEL32.dll", "GetProcAddress", args, 2u);
    REQUIRE(c.eax != 0u);

    write_name(SCRATCH_NAME, "WSASend");
    c = call_named("KERNEL32.dll", "GetProcAddress", args, 2u);
    REQUIRE(c.eax != 0u);
    return 0;
}

static int test_wsarecv_completion(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args,
                       4u);
    REQUIRE(c.eax == FAKE_PORT);
    const uint32_t assoc_args[4] = { s, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    wc3_legacy_wr32g(SCRATCH_WSABUF, 5u);
    wc3_legacy_wr32g(SCRATCH_WSABUF + 4u, SCRATCH_IO_DATA);
    memset(guest_ptr(SCRATCH_IO_DATA, 8u), 0, 8u);
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_BYTES, 0xDEADBEEFu);
    wc3_legacy_wr32g(SCRATCH_FLAGS, 0u);
    atomic_store(&g_recv_length, 5u);
    memcpy(g_recv_bytes, "hello", 5u);

    const uint32_t args[7] = {
        s, SCRATCH_WSABUF, 1u, SCRATCH_BYTES, SCRATCH_FLAGS,
        SCRATCH_OVERLAP, 0u
    };
    c = call_named("WS2_32.dll", "WSARecv", args, 7u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);

    uint32_t bytes = 0, key = 0, ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 5u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(memcmp(guest_ptr(SCRATCH_IO_DATA, 5u), "hello", 5u) == 0);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP + 4u) == 5u);
    /* Overlapped calls leave lpNumberOfBytesRecvd/lpFlags alone after the
     * pending return; the game passes dead stack locals there. */
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FLAGS) == 0u);
    REQUIRE(atomic_load(&g_recv_length) == 0u);

    const uint32_t close_socket[1] = { s };
    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_socket, 1u).eax == 0u);
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int test_wsarecv_keeps_caller_stack(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args,
                       4u);
    REQUIRE(c.eax == FAKE_PORT);
    const uint32_t assoc_args[4] = { s, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_WSABUF2, 5u);
    wc3_legacy_wr32g(SCRATCH_WSABUF2 + 4u, SCRATCH_IO_DATA);
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0x11111111u);
    wc3_legacy_wr32g(SCRATCH_FLAGS2, 0u);
    atomic_store(&g_recv_length, 0u);
    atomic_store(&g_accept_ready, 0);

    const uint32_t args[7] = {
        s, SCRATCH_WSABUF2, 1u, SCRATCH_BYTES2, SCRATCH_FLAGS2,
        SCRATCH_OVERLAP, 0u
    };
    c = call_named("WS2_32.dll", "WSARecv", args, 7u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES2) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FLAGS2) == 0u);

    /* The 1.27 caller points these at dead stack locals and its WSABUF is a
     * stack temporary too.  The pending call must have copied everything it
     * needs and must never write those slots again. */
    wc3_legacy_wr32g(SCRATCH_WSABUF2, 0xA5A5A5A5u);
    wc3_legacy_wr32g(SCRATCH_WSABUF2 + 4u, 0xA5A5A5A5u);
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0xA5A5A5A5u);
    wc3_legacy_wr32g(SCRATCH_FLAGS2, 0xA5A5A5A5u);

    memcpy(g_recv_bytes, "world", 5u);
    atomic_store(&g_recv_length, 5u);

    uint32_t bytes = 0, key = 0, ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 5u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(memcmp(guest_ptr(SCRATCH_IO_DATA, 5u), "world", 5u) == 0);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_WSABUF2) == 0xA5A5A5A5u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_WSABUF2 + 4u) == 0xA5A5A5A5u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES2) == 0xA5A5A5A5u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_FLAGS2) == 0xA5A5A5A5u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP + 4u) == 5u);

    const uint32_t close_socket[1] = { s };
    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_socket, 1u).eax == 0u);
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int test_wsasend_completion(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args,
                       4u);
    REQUIRE(c.eax == FAKE_PORT);
    const uint32_t assoc_args[4] = { s, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    wc3_legacy_wr32g(SCRATCH_WSABUF, 3u);
    wc3_legacy_wr32g(SCRATCH_WSABUF + 4u, SCRATCH_IO_DATA);
    memcpy(guest_ptr(SCRATCH_IO_DATA, 8u), "abc", 3u);
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_BYTES, 0u);
    atomic_store(&g_sent_length, 0u);
    memset(g_sent_bytes, 0, sizeof g_sent_bytes);

    const uint32_t args[7] = {
        s, SCRATCH_WSABUF, 1u, SCRATCH_BYTES, 0u, SCRATCH_OVERLAP, 0u
    };
    c = call_named("WS2_32.dll", "WSASend", args, 7u);
    REQUIRE(c.eax == 0xFFFFFFFFu);
    REQUIRE(wsa_last_error("WS2_32.dll") == WSA_IO_PENDING);

    uint32_t bytes = 0, key = 0, ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 3u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(atomic_load(&g_sent_length) == 3u);
    REQUIRE(memcmp(g_sent_bytes, "abc", 3u) == 0);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP + 4u) == 3u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES) == 0u);

    const uint32_t close_socket[1] = { s };
    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_socket, 1u).eax == 0u);
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int test_socket_readwrite_completion(void)
{
    uint32_t s = make_socket(WSA_AF_INET, WSA_SOCK_STREAM, 0u);
    REQUIRE(s != 0xFFFFFFFFu);

    const uint32_t create_args[4] = { 0u, 0u, 0u, 0u };
    CPU c = call_named("KERNEL32.dll", "CreateIoCompletionPort", create_args,
                       4u);
    REQUIRE(c.eax == FAKE_PORT);
    const uint32_t assoc_args[4] = { s, FAKE_PORT, FAKE_PORT_KEY, 0u };
    c = call_named("KERNEL32.dll", "CreateIoCompletionPort", assoc_args, 4u);
    REQUIRE(c.eax == FAKE_PORT);

    /* Overlapped ReadFile on a socket: the server connection class uses this
     * shape and expects ERROR_IO_PENDING plus an IOCP completion. */
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0u);
    memset(guest_ptr(SCRATCH_IO_DATA, 8u), 0, 8u);
    atomic_store(&g_recv_length, 5u);
    memcpy(g_recv_bytes, "world", 5u);

    const uint32_t read_args[5] = {
        s, SCRATCH_IO_DATA, 5u, SCRATCH_BYTES2, SCRATCH_OVERLAP
    };
    c = call_named("KERNEL32.dll", "ReadFile", read_args, 5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 997u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES2) == 0u);

    /* The overlapped caller may reuse its stack slot immediately; the
     * completion must not write lpNumberOfBytesRead. */
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0xA5A5A5A5u);

    uint32_t bytes = 0, key = 0, ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 5u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(memcmp(guest_ptr(SCRATCH_IO_DATA, 5u), "world", 5u) == 0);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP) == 0u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP + 4u) == 5u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES2) == 0xA5A5A5A5u);

    /* Overlapped WriteFile on the same socket, matching server StartSend. */
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0u);
    memcpy(guest_ptr(SCRATCH_IO_DATA, 8u), "abc", 3u);
    atomic_store(&g_sent_length, 0u);
    memset(g_sent_bytes, 0, sizeof g_sent_bytes);

    const uint32_t write_args[5] = {
        s, SCRATCH_IO_DATA, 3u, SCRATCH_BYTES2, SCRATCH_OVERLAP
    };
    c = call_named("KERNEL32.dll", "WriteFile", write_args, 5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 997u);
    wc3_legacy_wr32g(SCRATCH_BYTES2, 0xA5A5A5A5u);

    bytes = key = ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 3u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(atomic_load(&g_sent_length) == 3u);
    REQUIRE(memcmp(g_sent_bytes, "abc", 3u) == 0);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_OVERLAP + 4u) == 3u);
    REQUIRE(wc3_legacy_rd32g(SCRATCH_BYTES2) == 0xA5A5A5A5u);

    /* Server StartReceive/StartSend pass NULL for the byte-count pointer.
     * The pending call must still return FALSE + ERROR_IO_PENDING and the
     * completion must carry the byte count. */
    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    atomic_store(&g_recv_length, 3u);
    memcpy(g_recv_bytes, "srv", 3u);
    const uint32_t server_read[5] = {
        s, SCRATCH_IO_DATA, 3u, 0u, SCRATCH_OVERLAP
    };
    c = call_named("KERNEL32.dll", "ReadFile", server_read, 5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 997u);
    bytes = key = ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 3u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(memcmp(guest_ptr(SCRATCH_IO_DATA, 3u), "srv", 3u) == 0);

    wc3_legacy_guest_zero(SCRATCH_OVERLAP, 32u);
    memcpy(guest_ptr(SCRATCH_IO_DATA, 3u), "snd", 3u);
    atomic_store(&g_sent_length, 0u);
    memset(g_sent_bytes, 0, sizeof g_sent_bytes);
    const uint32_t server_write[5] = {
        s, SCRATCH_IO_DATA, 3u, 0u, SCRATCH_OVERLAP
    };
    c = call_named("KERNEL32.dll", "WriteFile", server_write, 5u);
    REQUIRE(c.eax == 0u);
    REQUIRE(wc3_legacy_get_last_error(NULL) == 997u);
    bytes = key = ov = 0;
    REQUIRE(wait_completion(FAKE_PORT, 1000u, &bytes, &key, &ov));
    REQUIRE(bytes == 3u);
    REQUIRE(key == FAKE_PORT_KEY);
    REQUIRE(ov == SCRATCH_OVERLAP);
    REQUIRE(atomic_load(&g_sent_length) == 3u);
    REQUIRE(memcmp(g_sent_bytes, "snd", 3u) == 0);

    /* A non-socket handle must still reach the chained as-built file path. */
    g_prev_read_calls = 0;
    g_prev_write_calls = 0;
    const uint32_t file_args[5] = {
        0x00000123u, SCRATCH_IO_DATA, 1u, SCRATCH_BYTES2, 0u
    };
    c = call_named("KERNEL32.dll", "ReadFile", file_args, 5u);
    REQUIRE(c.eax == 1u);
    REQUIRE(g_prev_read_calls == 1u);
    c = call_named("KERNEL32.dll", "WriteFile", file_args, 5u);
    REQUIRE(c.eax == 1u);
    REQUIRE(g_prev_write_calls == 1u);

    const uint32_t close_socket[1] = { s };
    const uint32_t close_port[1] = { FAKE_PORT };
    REQUIRE(call_named("WS2_32.dll", "closesocket", close_socket, 1u).eax == 0u);
    REQUIRE(call_named("KERNEL32.dll", "CloseHandle", close_port, 1u).eax == 1u);
    return 0;
}

static int init_memory(void)
{
#ifdef _WIN32
    g_mem = (uint8_t *)VirtualAlloc((void *)(uintptr_t)GUEST_BASE, GUEST_SIZE,
                                    MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)GUEST_BASE, GUEST_SIZE,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
                                              MAP_FIXED, -1, 0);
    g_mem = (p == MAP_FAILED) ? NULL : (uint8_t *)p;
#endif
    if (!g_mem) {
        fprintf(stderr, "ws2_32 hle: cannot map guest arena\n");
        return 1;
    }
    return 0;
}

static void free_memory(void)
{
#ifdef _WIN32
    if (g_mem) VirtualFree(g_mem, 0, MEM_RELEASE);
#else
    if (g_mem) munmap(g_mem, GUEST_SIZE);
#endif
    g_mem = NULL;
}

int main(void)
{
    if (init_memory() != 0) return 1;
    memset(&g_platform, 0, sizeof g_platform);
    g_platform.net.ctx = NULL;
    g_platform.net.startup = fake_net_startup;
    g_platform.net.cleanup = fake_net_cleanup;
    g_platform.net.socket = fake_net_socket;
    g_platform.net.close = fake_net_close;
    g_platform.net.bind = fake_net_bind;
    g_platform.net.connect = fake_net_connect;
    g_platform.net.listen = fake_net_listen;
    g_platform.net.accept = fake_net_accept;
    g_platform.net.send = fake_net_send;
    g_platform.net.recv = fake_net_recv;
    g_platform.net.sendto = fake_net_sendto;
    g_platform.net.recvfrom = fake_net_recvfrom;
    g_platform.net.shutdown = fake_net_shutdown;
    g_platform.net.getpeername = fake_net_getpeername;
    g_platform.net.getsockname = fake_net_getsockname;
    g_platform.net.setsockopt = fake_net_setsockopt;
    g_platform.net.getsockopt = fake_net_getsockopt;
    g_platform.net.set_nonblocking = fake_net_set_nonblocking;
    g_platform.net.bytes_available = fake_net_bytes_available;
    g_platform.net.at_mark = fake_net_at_mark;
    g_platform.net.select = fake_net_select;
    g_platform.net.hostname = fake_net_hostname;
    g_platform.net.resolve = fake_net_resolve;
    g_platform.net.interfaces = fake_net_interfaces;

    g_platform.thread.ctx = NULL;
    g_platform.thread.create = fake_thread_create;
    g_platform.thread.detach = fake_thread_detach;

    int rc = 0;
    if (test_install_and_ordinals() != 0) rc = 1;
    if (!rc && test_wsastartup() != 0) rc = 1;
    if (!rc && test_socket_lifecycle() != 0) rc = 1;
    if (!rc && test_bind_and_select() != 0) rc = 1;
    if (!rc && test_connect_inprogress() != 0) rc = 1;
    if (!rc && test_names() != 0) rc = 1;
    if (!rc && test_options_and_extension() != 0) rc = 1;
    if (!rc && test_acceptex_completion() != 0) rc = 1;
    if (!rc && test_acceptex_shape_rejection() != 0) rc = 1;
    if (!rc && test_acceptex_cancel_releases_slots() != 0) rc = 1;
    if (!rc && test_dynamic_wsa_exports() != 0) rc = 1;
    if (!rc && test_wsarecv_completion() != 0) rc = 1;
    if (!rc && test_wsarecv_keeps_caller_stack() != 0) rc = 1;
    if (!rc && test_wsasend_completion() != 0) rc = 1;
    if (!rc && test_socket_readwrite_completion() != 0) rc = 1;

    free_memory();
    if (rc == 0) puts("ws2_32 hle: ok");
    return rc;
}
