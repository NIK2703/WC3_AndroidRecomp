/*
 * ws2_32_hle.c - winsock 1/2 guest surface for Warcraft III 1.27.
 *
 * The as-built winsock table only knew WSAStartup, htons/ntohs and the
 * htonl/ntohl helpers.  game.dll imports the real socket calls by WS2_32
 * ordinal and war3.exe imports WSOCK32 by ordinal; the as-built loader turns
 * an unresolved ordinal into a guessed-arity stub, which corrupts the guest
 * stack on the first call.  This module binds the exact arity before the
 * import walk and adds named rows for the three named WS2_32 imports
 * (WSAIoctl, getaddrinfo, freeaddrinfo).
 *
 * It also owns the two synthetic modules the 1.27 network layer loads at
 * runtime.  Windows has ws2_32.dll and mswsock.dll in System32; the Android
 * process has neither, so LoadLibraryA is chained here and returns a stable
 * pseudo-HMODULE.  GetProcAddress then resolves through the same registry.
 */
#include "ws2_32_hle.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "legacy_shim.h"
#include "platform.h"
#include "wc3_debug.h"

/* WinSock 2 constants used by the guest. */
#define WSA_SOCKET_ERROR        0xFFFFFFFFu
#define WSA_INVALID_SOCKET      0xFFFFFFFFu
#define WSA_SOL_SOCKET          0xFFFFu
#define WSA_IPPROTO_TCP         6u
#define WSA_IPPROTO_UDP         17u

#define WSA_AF_INET             2u
#define WSA_AF_INET6            23u
#define WSA_SOCK_STREAM         1u
#define WSA_SOCK_DGRAM          2u

#define WSA_SO_REUSEADDR        0x0004u
#define WSA_SO_KEEPALIVE        0x0008u
#define WSA_SO_DONTROUTE        0x0010u
#define WSA_SO_BROADCAST        0x0020u
#define WSA_SO_LINGER           0x0080u
#define WSA_SO_OOBINLINE        0x0100u
#define WSA_SO_SNDBUF           0x1001u
#define WSA_SO_RCVBUF           0x1002u
#define WSA_SO_SNDTIMEO         0x1005u
#define WSA_SO_RCVTIMEO         0x1006u
#define WSA_SO_ERROR            0x1007u
#define WSA_SO_TYPE             0x1008u
#define WSA_SO_EXCLUSIVEADDRUSE 0xFFFFFFFBu
#define WSA_SO_DONTLINGER       0xFFFFFF7Fu
#define WSA_SO_UPDATE_ACCEPT_CONTEXT 0x700Bu
#define WSA_TCP_NODELAY         0x0001u

#define WSA_FIONBIO             0x8004667Eu
#define WSA_FIONREAD            0x4004667Fu
#define WSA_SIOCATMARK          0x40047307u

#define WSA_SIO_GET_BROADCAST_ADDRESS 0x48000005u
#define WSA_SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006u
#define WSA_SIO_ROUTING_INTERFACE_QUERY 0xC8000014u
#define WSA_SIO_ADDRESS_LIST_QUERY 0x48000016u
#define WSA_SIO_ADDRESS_LIST_CHANGE 0x28000017u
#define WSA_SIO_BSP_HANDLE 0x4800001Bu
#define WSA_SIO_BSP_HANDLE_SELECT 0x4800001Cu
#define WSA_SIO_BSP_HANDLE_POLL 0x4800001Du
#define WSA_SIO_BASE_HANDLE 0x48000022u
#define WSA_SIO_SET_COMPATIBILITY_MODE 0x9800000Cu

#define WSA_MSG_OOB             0x1
#define WSA_MSG_PEEK            0x2
#define WSA_MSG_DONTROUTE       0x4
#define WSA_MSG_PARTIAL         0x8000

/* WinSock error codes. */
#define WSAEINTR                10004
#define WSAEACCES               10013
#define WSAEFAULT               10014
#define WSAEINVAL               10022
#define WSAEMFILE               10024
#define WSAEWOULDBLOCK          10035
#define WSAEINPROGRESS          10036
#define WSAEALREADY             10037
#define WSAENOTSOCK             10038
#define WSAEDESTADDRREQ         10039
#define WSAEMSGSIZE             10040
#define WSAEPROTONOSUPPORT      10043
#define WSAEOPNOTSUPP           10045
#define WSAEAFNOSUPPORT         10047
#define WSAEADDRINUSE           10048
#define WSAEADDRNOTAVAIL        10049
#define WSAENETDOWN             10050
#define WSAENETUNREACH          10051
#define WSAENETRESET            10052
#define WSAECONNABORTED         10053
#define WSAECONNRESET           10054
#define WSAENOBUFS              10055
#define WSAEISCONN              10056
#define WSAENOTCONN             10057
#define WSAESHUTDOWN            10058
#define WSAETIMEDOUT            10060
#define WSAECONNREFUSED         10061
#define WSAEHOSTUNREACH         10065
#define WSASYSNOTREADY          10091
#define WSAVERNOTSUPPORTED      10092
#define WSANOTINITIALISED       10093
#define WSAHOST_NOT_FOUND       11001
#define WSATRY_AGAIN            11002
#define WSANO_RECOVERY          11003
#define WSANO_DATA              11004

#define WSA_IO_PENDING           997
#define WSA_WAIT_TIMEOUT         258
#define WSA_ERROR_INVALID_HANDLE 6
#define WSA_ERROR_NOT_ENOUGH_MEMORY 8
#define WSA_ERROR_INVALID_PARAMETER 87

#define WSA_INFINITE             0xFFFFFFFFu
#define WSA_COMPLETION_SLICE_MS  20u

#define WSA_HMOD_WS2_32         0x000A2000u
#define WSA_HMOD_MSWSOCK        0x000A2004u

#define WSA_SOCKET_SLOTS        256u
#define WSA_FD_SETSIZE          64u
#define WSA_HOSTENT_SIZE        16u
#define WSA_ADDRINFO_SIZE       32u
#define WSA_SOCKADDR_IN_SIZE    16u
#define WSA_SOCKADDR_IN6_SIZE   28u
#define WSA_GUID_SIZE           16u
#define WSA_IOCTL_MAX           1024u

typedef struct WsaSocketSlot {
    _Atomic uint32_t guest;
    _Atomic uintptr_t host;
    _Atomic uint32_t accept_context;
    _Atomic uint32_t iocp_port;
    _Atomic uint32_t iocp_key;
} WsaSocketSlot;

typedef struct WsaIocpEvent {
    uint32_t bytes;
    uint32_t key;
    uint32_t overlapped;
    int success;
    int error;
} WsaIocpEvent;

#define WSA_IOCP_PORTS        32u
#define WSA_IOCP_QUEUE_DEPTH  32u

typedef struct WsaIocpPort {
    uint32_t handle;
    int socket_associated;
    unsigned head;
    unsigned tail;
    unsigned count;
    WsaIocpEvent events[WSA_IOCP_QUEUE_DEPTH];
} WsaIocpPort;

#define WSA_PENDING_ACCEPTS   32u

typedef struct WsaPendingAccept {
    int used;
    int cancel;
    uint32_t listener_guest;
    Wc3NetSocket listener_host;
    uint32_t accept_guest;
    Wc3NetSocket accept_host;
    uint32_t output;
    uint32_t output_size;
    uint32_t local_length;
    uint32_t remote_length;
    uint32_t bytes_ptr;
    uint32_t overlapped;
    uint32_t port;
    uint32_t key;
} WsaPendingAccept;

static WsaSocketSlot g_slots[WSA_SOCKET_SLOTS];
static _Atomic uint32_t g_next_generation;
static const Wc3NetApi *g_net;
static const Wc3ThreadApi *g_thread;
static int g_installed;
static _Thread_local int g_wsa_error;
static uint16_t g_wsa_version;

static pthread_mutex_t g_socket_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_iocp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_accept_lock = PTHREAD_MUTEX_INITIALIZER;
static WsaIocpPort g_iocp_ports[WSA_IOCP_PORTS];
static WsaPendingAccept g_pending_accepts[WSA_PENDING_ACCEPTS];

typedef struct WsaFunc {
    const char *name;
    int argc;
    void (*fn)(CPU *c);
} WsaFunc;

typedef struct WsaOrdinal {
    uint16_t ordinal;
    const char *name;
} WsaOrdinal;

static void h_accept(CPU *c);
static void h_bind(CPU *c);
static void h_closesocket(CPU *c);
static void h_connect(CPU *c);
static void h_getpeername(CPU *c);
static void h_getsockname(CPU *c);
static void h_getsockopt(CPU *c);
static void h_htonl(CPU *c);
static void h_htons(CPU *c);
static void h_ioctlsocket(CPU *c);
static void h_inet_addr(CPU *c);
static void h_inet_ntoa(CPU *c);
static void h_listen(CPU *c);
static void h_ntohl(CPU *c);
static void h_ntohs(CPU *c);
static void h_recv(CPU *c);
static void h_recvfrom(CPU *c);
static void h_select(CPU *c);
static void h_send(CPU *c);
static void h_sendto(CPU *c);
static void h_setsockopt(CPU *c);
static void h_shutdown(CPU *c);
static void h_socket(CPU *c);
static void h_gethostbyname(CPU *c);
static void h_gethostname(CPU *c);
static void h_wsagetlasterror(CPU *c);
static void h_wsasetlasterror(CPU *c);
static void h_wsastartup(CPU *c);
static void h_wsacleanup(CPU *c);
static void h_wsafdisset(CPU *c);
static void h_wsaioctl(CPU *c);
static void h_getaddrinfo(CPU *c);
static void h_freeaddrinfo(CPU *c);
static void h_wsaasyncselect(CPU *c);
static void h_wsarecvex(CPU *c);
static void h_accept_ex(CPU *c);
static void h_get_accept_ex_sockaddrs(CPU *c);
static void h_create_iocp(CPU *c);
static void h_get_queued_completion(CPU *c);
static void h_close_handle(CPU *c);

/* Names that both WS2_32 and WSOCK32 export (the 1.27 import set). */
static const WsaFunc g_common_functions[] = {
    { "accept", 3, h_accept },
    { "bind", 3, h_bind },
    { "closesocket", 1, h_closesocket },
    { "connect", 3, h_connect },
    { "getpeername", 3, h_getpeername },
    { "getsockname", 3, h_getsockname },
    { "getsockopt", 5, h_getsockopt },
    { "htonl", 1, h_htonl },
    { "htons", 1, h_htons },
    { "inet_addr", 1, h_inet_addr },
    { "inet_ntoa", 1, h_inet_ntoa },
    { "ioctlsocket", 3, h_ioctlsocket },
    { "listen", 2, h_listen },
    { "ntohl", 1, h_ntohl },
    { "ntohs", 1, h_ntohs },
    { "recv", 4, h_recv },
    { "recvfrom", 6, h_recvfrom },
    { "select", 5, h_select },
    { "send", 4, h_send },
    { "sendto", 6, h_sendto },
    { "setsockopt", 5, h_setsockopt },
    { "shutdown", 2, h_shutdown },
    { "socket", 3, h_socket },
    { "gethostbyname", 1, h_gethostbyname },
    { "gethostname", 2, h_gethostname },
    { "WSAGetLastError", 0, h_wsagetlasterror },
    { "WSASetLastError", 1, h_wsasetlasterror },
    { "WSAStartup", 2, h_wsastartup },
    { "WSACleanup", 0, h_wsacleanup },
    { "__WSAFDIsSet", 2, h_wsafdisset },
    { "WSAAsyncSelect", 3, h_wsaasyncselect },
};

/* WS2_32-only named exports. */
static const WsaFunc g_ws2_functions[] = {
    { "WSAIoctl", 9, h_wsaioctl },
    { "getaddrinfo", 4, h_getaddrinfo },
    { "freeaddrinfo", 1, h_freeaddrinfo },
};

static const WsaFunc g_mswsock_functions[] = {
    { "AcceptEx", 8, h_accept_ex },
    { "GetAcceptExSockaddrs", 8, h_get_accept_ex_sockaddrs },
    { "WSARecvEx", 4, h_wsarecvex },
};

/* WS2_32 and WSOCK32 disagree at 10/11/12 (ioctlsocket vs inet_addr vs
 * inet_ntoa).  Keeping two tables is the fix for the old-tree stack corruption
 * that happened when game.dll's WS2_32 ordinals were bound through WSOCK32. */
static const WsaOrdinal g_ws2_ordinals[] = {
    { 1, "accept" }, { 2, "bind" }, { 3, "closesocket" }, { 4, "connect" },
    { 5, "getpeername" }, { 6, "getsockname" }, { 7, "getsockopt" },
    { 8, "htonl" }, { 9, "htons" }, { 10, "ioctlsocket" },
    { 11, "inet_addr" }, { 12, "inet_ntoa" }, { 13, "listen" },
    { 14, "ntohl" }, { 15, "ntohs" }, { 16, "recv" }, { 17, "recvfrom" },
    { 18, "select" }, { 19, "send" }, { 20, "sendto" }, { 21, "setsockopt" },
    { 22, "shutdown" }, { 23, "socket" }, { 52, "gethostbyname" },
    { 57, "gethostname" }, { 111, "WSAGetLastError" },
    { 112, "WSASetLastError" }, { 115, "WSAStartup" },
    { 116, "WSACleanup" }, { 151, "__WSAFDIsSet" },
};

static const WsaOrdinal g_wsock32_ordinals[] = {
    { 1, "accept" }, { 2, "bind" }, { 3, "closesocket" }, { 4, "connect" },
    { 5, "getpeername" }, { 6, "getsockname" }, { 7, "getsockopt" },
    { 8, "htonl" }, { 9, "htons" }, { 10, "inet_addr" },
    { 11, "inet_ntoa" }, { 12, "ioctlsocket" }, { 13, "listen" },
    { 14, "ntohl" }, { 15, "ntohs" }, { 16, "recv" }, { 17, "recvfrom" },
    { 18, "select" }, { 19, "send" }, { 20, "sendto" }, { 21, "setsockopt" },
    { 22, "shutdown" }, { 23, "socket" }, { 52, "gethostbyname" },
    { 57, "gethostname" }, { 111, "WSAGetLastError" },
    { 112, "WSASetLastError" }, { 115, "WSAStartup" },
    { 116, "WSACleanup" }, { 151, "__WSAFDIsSet" },
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned ac = (unsigned char)*a++;
        unsigned bc = (unsigned char)*b++;
        if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
        if (ac != bc) return 0;
    }
    return *a == *b;
}

static int guest_read(uint32_t addr, void *dst, uint32_t size)
{
    if (!size) return 1;
    if (!dst || !wc3_legacy_guest_ok(addr, size)) return 0;
    memcpy(dst, (const void *)(uintptr_t)addr, size);
    return 1;
}

static int guest_write(uint32_t addr, const void *src, uint32_t size)
{
    if (!size) return 1;
    if (!src || !wc3_legacy_guest_ok(addr, size)) return 0;
    memcpy((void *)(uintptr_t)addr, src, size);
    return 1;
}

static int guest_cstr(uint32_t addr, char *dst, uint32_t cap)
{
    if (!dst || cap == 0u || !addr) return 0;
    for (uint32_t i = 0; i + 1u < cap; i++) {
        if (!wc3_legacy_guest_ok(addr + i, 1)) return 0;
        char ch = (char)wc3_legacy_rd8g(addr + i);
        dst[i] = ch;
        if (!ch) return 1;
    }
    dst[cap - 1u] = '\0';
    return 0;
}

static int guest_cstr_write(uint32_t addr, const char *src, uint32_t cap)
{
    if (!src) src = "";
    size_t n = strlen(src);
    if (!addr || n + 1u > cap) return 0;
    if (!wc3_legacy_guest_ok(addr, (uint32_t)n + 1u)) return 0;
    memcpy((void *)(uintptr_t)addr, src, n + 1u);
    return 1;
}

static uint32_t bswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static uint16_t bswap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static int parse_ipv4(const char *text, uint32_t *out)
{
    if (!text || !out) return 0;
    uint32_t parts[4];
    unsigned n = 0;
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    while (n < 4u) {
        if (*p < '0' || *p > '9') return 0;
        uint32_t value = 0;
        unsigned digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10u + (uint32_t)(*p - '0');
            if (value > 255u) return 0;
            p++;
            digits++;
        }
        if (!digits) return 0;
        parts[n++] = value;
        if (n == 4u) break;
        if (*p != '.') return 0;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') return 0;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 1;
}

static int format_ipv4(uint32_t value, char *out, uint32_t cap)
{
    if (!out || cap < 16u) return 0;
    int n = snprintf(out, cap, "%u.%u.%u.%u",
                     (unsigned)(value & 0xFFu),
                     (unsigned)((value >> 8) & 0xFFu),
                     (unsigned)((value >> 16) & 0xFFu),
                     (unsigned)((value >> 24) & 0xFFu));
    return n > 0 && (uint32_t)n < cap;
}

static int net_ready(void)
{
    return g_net && g_net->socket && g_net->close && g_net->select &&
           g_net->send && g_net->recv && g_net->sendto && g_net->recvfrom;
}

static int net_error_result(int result)
{
    if (result >= 0) return 0;
    switch (-result) {
    case WC3_NET_ERR_AGAIN:
    case WC3_NET_ERR_WOULDBLOCK: return WSAEWOULDBLOCK;
    case WC3_NET_ERR_INTR: return WSAEINTR;
    case WC3_NET_ERR_BADF:
    case WC3_NET_ERR_NOTSOCK: return WSAENOTSOCK;
    case WC3_NET_ERR_ACCES: return WSAEACCES;
    case WC3_NET_ERR_FAULT: return WSAEFAULT;
    case WC3_NET_ERR_INVAL: return WSAEINVAL;
    case WC3_NET_ERR_MFILE: return WSAEMFILE;
    case WC3_NET_ERR_NOMEM:
    case WC3_NET_ERR_NOBUFS: return WSAENOBUFS;
    case WC3_NET_ERR_CONNREFUSED: return WSAECONNREFUSED;
    case WC3_NET_ERR_CONNRESET: return WSAECONNRESET;
    case WC3_NET_ERR_CONNABORTED: return WSAECONNABORTED;
    case WC3_NET_ERR_HOSTUNREACH: return WSAEHOSTUNREACH;
    case WC3_NET_ERR_NETUNREACH: return WSAENETUNREACH;
    case WC3_NET_ERR_TIMEDOUT: return WSAETIMEDOUT;
    case WC3_NET_ERR_INPROGRESS: return WSAEINPROGRESS;
    case WC3_NET_ERR_ALREADY: return WSAEALREADY;
    case WC3_NET_ERR_MSGSIZE: return WSAEMSGSIZE;
    case WC3_NET_ERR_ADDRINUSE: return WSAEADDRINUSE;
    case WC3_NET_ERR_ADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case WC3_NET_ERR_NETDOWN: return WSAENETDOWN;
    case WC3_NET_ERR_NOTCONN: return WSAENOTCONN;
    case WC3_NET_ERR_DESTADDRREQ: return WSAEDESTADDRREQ;
    case WC3_NET_ERR_AFNOSUPPORT: return WSAEAFNOSUPPORT;
    case WC3_NET_ERR_PROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case WC3_NET_ERR_OPNOTSUPP: return WSAEOPNOTSUPP;
    case WC3_NET_ERR_ISCONN: return WSAEISCONN;
    case WC3_NET_ERR_NODATA:
    case WC3_NET_ERR_NONAME: return WSAHOST_NOT_FOUND;
    case WC3_NET_ERR_PIPE: return WSAECONNABORTED;
    case WC3_NET_ERR_IO: return WSAECONNRESET;
    default: return WSAEINVAL;
    }
}

static uint32_t fail_net(int result)
{
    g_wsa_error = net_error_result(result);
    return WSA_SOCKET_ERROR;
}

static uint32_t fail_wsa(int code)
{
    g_wsa_error = code;
    return WSA_SOCKET_ERROR;
}

/* ------------------------------------------------------------------ */
/* guest socket table                                                  */
/* ------------------------------------------------------------------ */

static WsaSocketSlot *socket_slot(uint32_t handle)
{
    if (!handle || handle == WSA_SOCKET_ERROR) return NULL;
    unsigned index = (unsigned)(handle & 0xFFu);
    if (!index) return NULL;
    index--;
    if (index >= WSA_SOCKET_SLOTS) return NULL;
    return &g_slots[index];
}

static int socket_lookup(uint32_t handle, Wc3NetSocket *host)
{
    WsaSocketSlot *slot = socket_slot(handle);
    if (!slot) return 0;
    if (atomic_load_explicit(&slot->guest, memory_order_acquire) != handle)
        return 0;
    *host = (Wc3NetSocket)atomic_load_explicit(&slot->host,
                                               memory_order_relaxed);
    return 1;
}

static int socket_alloc(Wc3NetSocket host, uint32_t *out)
{
    if (!out) return 0;
    pthread_mutex_lock(&g_socket_lock);
    for (unsigned i = 0; i < WSA_SOCKET_SLOTS; i++) {
        WsaSocketSlot *slot = &g_slots[i];
        if (atomic_load_explicit(&slot->guest, memory_order_relaxed) != 0)
            continue;
        uint32_t generation =
            atomic_fetch_add_explicit(&g_next_generation, 1,
                                      memory_order_relaxed) + 1u;
        generation &= 0x00FFFFFFu;
        if (!generation) {
            generation = atomic_fetch_add_explicit(&g_next_generation, 1,
                                                   memory_order_relaxed) + 1u;
            generation &= 0x00FFFFFFu;
        }
        uint32_t handle = (generation << 8) | (i + 1u);
        if (!handle || handle == WSA_SOCKET_ERROR) continue;
        atomic_store_explicit(&slot->host, host, memory_order_relaxed);
        atomic_store_explicit(&slot->accept_context, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&slot->iocp_port, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->iocp_key, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->guest, handle, memory_order_release);
        *out = handle;
        pthread_mutex_unlock(&g_socket_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_socket_lock);
    return 0;
}

static void socket_release(uint32_t handle)
{
    WsaSocketSlot *slot = socket_slot(handle);
    if (!slot) return;
    pthread_mutex_lock(&g_socket_lock);
    if (atomic_load_explicit(&slot->guest, memory_order_relaxed) != handle) {
        pthread_mutex_unlock(&g_socket_lock);
        return;
    }
    atomic_store_explicit(&slot->host, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->accept_context, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->iocp_port, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->iocp_key, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->guest, 0, memory_order_release);
    pthread_mutex_unlock(&g_socket_lock);
}

static int socket_install_accepted(uint32_t accept_guest,
                                   Wc3NetSocket old_host,
                                   Wc3NetSocket accepted,
                                   uint32_t listener_guest)
{
    WsaSocketSlot *slot = socket_slot(accept_guest);
    if (!slot) return 0;
    pthread_mutex_lock(&g_socket_lock);
    if (atomic_load_explicit(&slot->guest, memory_order_acquire) !=
            accept_guest ||
        (Wc3NetSocket)atomic_load_explicit(&slot->host,
                                           memory_order_relaxed) != old_host) {
        pthread_mutex_unlock(&g_socket_lock);
        return 0;
    }
    atomic_store_explicit(&slot->host, accepted, memory_order_relaxed);
    atomic_store_explicit(&slot->accept_context, listener_guest,
                          memory_order_release);
    pthread_mutex_unlock(&g_socket_lock);
    return 1;
}

static int socket_set_iocp(uint32_t handle, uint32_t port, uint32_t key)
{
    WsaSocketSlot *slot = socket_slot(handle);
    if (!slot) return 0;
    pthread_mutex_lock(&g_socket_lock);
    int ok = atomic_load_explicit(&slot->guest, memory_order_acquire) == handle;
    if (ok) {
        atomic_store_explicit(&slot->iocp_port, port, memory_order_relaxed);
        atomic_store_explicit(&slot->iocp_key, key, memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_socket_lock);
    return ok;
}

static int socket_get_iocp(uint32_t handle, uint32_t *port, uint32_t *key)
{
    WsaSocketSlot *slot = socket_slot(handle);
    if (!slot ||
        atomic_load_explicit(&slot->guest, memory_order_acquire) != handle)
        return 0;
    if (port)
        *port = atomic_load_explicit(&slot->iocp_port,
                                     memory_order_relaxed);
    if (key)
        *key = atomic_load_explicit(&slot->iocp_key, memory_order_relaxed);
    return 1;
}

static uint32_t socket_accept_context(uint32_t handle)
{
    WsaSocketSlot *slot = socket_slot(handle);
    if (!slot ||
        atomic_load_explicit(&slot->guest, memory_order_acquire) != handle)
        return 0;
    return atomic_load_explicit(&slot->accept_context, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* guest sockaddr / hostent / addrinfo helpers                         */
/* ------------------------------------------------------------------ */

static void write_sockaddr_in(uint32_t guest, uint32_t address, uint16_t port)
{
    uint8_t raw[WSA_SOCKADDR_IN_SIZE];
    memset(raw, 0, sizeof raw);
    raw[0] = (uint8_t)(WSA_AF_INET & 0xFFu);
    raw[1] = (uint8_t)(WSA_AF_INET >> 8);
    raw[2] = (uint8_t)(port & 0xFFu);
    raw[3] = (uint8_t)(port >> 8);
    memcpy(raw + 4, &address, sizeof address);
    guest_write(guest, raw, sizeof raw);
}

static int read_sockaddr_in(uint32_t guest, uint32_t length,
                            uint32_t *address, uint16_t *port)
{
    if (!guest || length < WSA_SOCKADDR_IN_SIZE) return 0;
    uint8_t raw[WSA_SOCKADDR_IN_SIZE];
    if (!guest_read(guest, raw, sizeof raw)) return 0;
    uint16_t family = (uint16_t)(raw[0] | (raw[1] << 8));
    if (family != WSA_AF_INET) return 0;
    if (port) *port = (uint16_t)(raw[2] | (raw[3] << 8));
    if (address) memcpy(address, raw + 4, sizeof *address);
    return 1;
}

static uint32_t alloc_hostent(const char *name, const uint32_t *addresses,
                              uint32_t count)
{
    if (!name || !addresses || !count) return 0;

    uint32_t hostent = wc3_legacy_guest_alloc0(WSA_HOSTENT_SIZE);
    uint32_t name_copy = wc3_legacy_guest_alloc0((uint32_t)strlen(name) + 1u);
    uint32_t aliases = wc3_legacy_guest_alloc0(4u);
    uint32_t addr_list = wc3_legacy_guest_alloc0((count + 1u) * 4u);
    if (!hostent || !name_copy || !aliases || !addr_list) {
        if (hostent) wc3_legacy_guest_free(hostent);
        if (name_copy) wc3_legacy_guest_free(name_copy);
        if (aliases) wc3_legacy_guest_free(aliases);
        if (addr_list) wc3_legacy_guest_free(addr_list);
        return 0;
    }

    guest_cstr_write(name_copy, name, (uint32_t)strlen(name) + 1u);
    wc3_legacy_wr32g(hostent + 0, name_copy);
    wc3_legacy_wr32g(hostent + 4, aliases);
    wc3_legacy_wr16g(hostent + 8, (uint16_t)WSA_AF_INET);
    wc3_legacy_wr16g(hostent + 10, 4u);
    wc3_legacy_wr32g(hostent + 12, addr_list);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = wc3_legacy_guest_alloc0(4u);
        if (!addr) {
            wc3_legacy_guest_free(hostent);
            wc3_legacy_guest_free(name_copy);
            wc3_legacy_guest_free(aliases);
            wc3_legacy_guest_free(addr_list);
            return 0;
        }
        wc3_legacy_wr32g(addr, addresses[i]);
        wc3_legacy_wr32g(addr_list + i * 4u, addr);
    }
    wc3_legacy_wr32g(addr_list + count * 4u, 0);
    return hostent;
}

static void free_guest_addrinfo(uint32_t head)
{
    uint32_t p = head;
    unsigned guard = 0;
    while (p && guard++ < 64u) {
        if (!wc3_legacy_guest_ok(p, WSA_ADDRINFO_SIZE)) break;
        uint32_t next = wc3_legacy_rd32g(p + 28);
        uint32_t canon = wc3_legacy_rd32g(p + 24);
        uint32_t addr = wc3_legacy_rd32g(p + 20);
        if (canon) wc3_legacy_guest_free(canon);
        if (addr) wc3_legacy_guest_free(addr);
        wc3_legacy_guest_free(p);
        p = next;
    }
}

static uint32_t gai_error_code(int net_result)
{
    if (net_result >= 0) return 0;
    switch (-net_result) {
    case WC3_NET_ERR_AGAIN: return WSATRY_AGAIN;
    case WC3_NET_ERR_NONAME:
    case WC3_NET_ERR_NODATA: return WSAHOST_NOT_FOUND;
    case WC3_NET_ERR_NOMEM:
    case WC3_NET_ERR_NOBUFS: return WSAENOBUFS;
    case WC3_NET_ERR_AFNOSUPPORT: return WSAEAFNOSUPPORT;
    case WC3_NET_ERR_PROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case WC3_NET_ERR_INVAL: return WSAEINVAL;
    default: return WSANO_RECOVERY;
    }
}

static int map_family(uint32_t guest_family, int *out)
{
    switch (guest_family) {
    case 0: *out = WC3_NET_AF_UNSPEC; return 1;
    case WSA_AF_INET: *out = WC3_NET_AF_INET; return 1;
    case WSA_AF_INET6: *out = WC3_NET_AF_INET6; return 1;
    default: return 0;
    }
}

static int map_socktype(uint32_t guest_type, int *out)
{
    if (guest_type == WSA_SOCK_STREAM) { *out = WC3_NET_SOCK_STREAM; return 1; }
    if (guest_type == WSA_SOCK_DGRAM) { *out = WC3_NET_SOCK_DGRAM; return 1; }
    return 0;
}

static int local_ipv4_addresses(uint32_t *addresses, uint32_t cap,
                                uint32_t *count)
{
    if (!g_net || !g_net->interfaces || !addresses || !count) return 0;
    Wc3NetInterface interfaces[32];
    uint32_t n = 0;
    int r = g_net->interfaces(g_net->ctx, interfaces, 32u, &n);
    if (r < 0) return 0;
    uint32_t out = 0;
    for (uint32_t i = 0; i < n && out < cap; i++) {
        if ((interfaces[i].flags & WC3_NET_IF_UP) &&
            !(interfaces[i].flags & WC3_NET_IF_LOOPBACK))
            addresses[out++] = interfaces[i].address;
    }
    for (uint32_t i = 0; i < n && out < cap; i++) {
        if (interfaces[i].flags & WC3_NET_IF_LOOPBACK)
            addresses[out++] = interfaces[i].address;
    }
    *count = out;
    return out > 0;
}

static int resolve_ipv4(const char *node, uint32_t *addresses, uint32_t cap,
                        uint32_t *count)
{
    *count = 0;
    if (!g_net || !g_net->resolve) return 0;

    char hostname[256];
    hostname[0] = '\0';
    if (g_net->hostname) g_net->hostname(g_net->ctx, hostname, sizeof hostname);

    if (!node || !node[0] || (hostname[0] && ascii_ieq(node, hostname))) {
        if (local_ipv4_addresses(addresses, cap, count)) return 1;
    }
    if (node && ascii_ieq(node, "localhost")) {
        if (cap) {
            addresses[0] = 0x0100007Fu; /* 127.0.0.1 in memory order */
            *count = 1;
            return 1;
        }
        return 0;
    }

    uint32_t numeric;
    if (node && parse_ipv4(node, &numeric)) {
        if (cap) {
            addresses[0] = numeric;
            *count = 1;
            return 1;
        }
        return 0;
    }

    Wc3NetResolveHints hints;
    memset(&hints, 0, sizeof hints);
    hints.family = WC3_NET_AF_INET;
    hints.socktype = WC3_NET_SOCK_STREAM;
    Wc3NetAddrInfo info[16];
    uint32_t n = 0;
    int r = g_net->resolve(g_net->ctx, node, NULL, &hints, info, 16u, &n);
    if (r < 0) return 0;
    uint32_t out = 0;
    for (uint32_t i = 0; i < n && out < cap; i++) {
        if (info[i].family != WC3_NET_AF_INET ||
            info[i].addr_length < WSA_SOCKADDR_IN_SIZE)
            continue;
        uint32_t addr = 0;
        memcpy(&addr, info[i].addr + 4, sizeof addr);
        addresses[out++] = addr;
    }
    *count = out;
    return out > 0;
}

/* ------------------------------------------------------------------ */
/* socket calls                                                        */
/* ------------------------------------------------------------------ */

static uint32_t socket_from_arg(CPU *c, unsigned index, Wc3NetSocket *host)
{
    uint32_t handle = wc3_legacy_arg32(c, index);
    if (!socket_lookup(handle, host)) {
        g_wsa_error = WSAENOTSOCK;
        return 0;
    }
    return handle;
}

static void h_socket(CPU *c)
{
    uint32_t family = wc3_legacy_arg32(c, 0);
    uint32_t type = wc3_legacy_arg32(c, 1);
    uint32_t protocol = wc3_legacy_arg32(c, 2);
    int nfamily, ntype;
    if (!map_family(family, &nfamily)) {
        c->eax = fail_wsa(WSAEAFNOSUPPORT);
        return;
    }
    if (nfamily == WC3_NET_AF_UNSPEC) {
        c->eax = fail_wsa(WSAEAFNOSUPPORT);
        return;
    }
    if (!map_socktype(type, &ntype)) {
        c->eax = fail_wsa(WSAEPROTONOSUPPORT);
        return;
    }
    if (!net_ready()) {
        c->eax = fail_wsa(WSASYSNOTREADY);
        return;
    }

    Wc3NetSocket host = 0;
    int r = g_net->socket(g_net->ctx, nfamily, ntype, (int)protocol, &host);
    if (r < 0) {
        c->eax = fail_net(r);
        return;
    }
    uint32_t guest = 0;
    if (!socket_alloc(host, &guest)) {
        (void)g_net->close(g_net->ctx, host);
        c->eax = fail_wsa(WSAENOBUFS);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] socket(%u,%u,%u) -> %08X\n",
            (unsigned)family, (unsigned)type, (unsigned)protocol,
            (unsigned)guest);
#endif
    c->eax = guest;
}

static void h_closesocket(CPU *c)
{
    Wc3NetSocket host = 0;
    uint32_t handle = socket_from_arg(c, 0, &host);
    if (!handle) {
        c->eax = WSA_SOCKET_ERROR;
        return;
    }
    int r = g_net ? g_net->close(g_net->ctx, host) : -WC3_NET_ERR_BADF;
    socket_release(handle);
    if (r < 0) c->eax = fail_net(r);
    else c->eax = 0;
}

static int address_argument(CPU *c, unsigned addr_index, unsigned len_index,
                            uint32_t *addr, uint32_t *len)
{
    *addr = wc3_legacy_arg32(c, addr_index);
    *len = wc3_legacy_arg32(c, len_index);
    if (!*addr || *len < WSA_SOCKADDR_IN_SIZE || *len > WC3_NET_ADDR_MAX)
        return 0;
    return wc3_legacy_guest_ok(*addr, *len) ? 1 : 0;
}

static void h_bind(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t addr, len;
    if (!address_argument(c, 1, 2, &addr, &len)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    int r = g_net->bind(g_net->ctx, host, (const void *)(uintptr_t)addr, len);
    if (r < 0) {
        c->eax = fail_net(r);
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_NET
    uint32_t ip = 0;
    uint16_t port = 0;
    char text[32] = "?";
    if (read_sockaddr_in(addr, len, &ip, &port)) format_ipv4(ip, text, sizeof text);
    fprintf(stderr, "[net] bind %s:%u ok\n", text, (unsigned)bswap16(port));
#endif
    c->eax = 0;
}

static void h_connect(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t addr, len;
    if (!address_argument(c, 1, 2, &addr, &len)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    int r = g_net->connect(g_net->ctx, host,
                           (const void *)(uintptr_t)addr, len);
    c->eax = r < 0 ? fail_net(r) : 0;
}

static void h_listen(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    int r = g_net->listen(g_net->ctx, host, (int)wc3_legacy_arg32(c, 1));
    c->eax = r < 0 ? fail_net(r) : 0;
}

static void h_accept(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t addr = wc3_legacy_arg32(c, 1);
    uint32_t len_ptr = wc3_legacy_arg32(c, 2);
    if ((addr && !len_ptr) || (!addr && len_ptr)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t len = 0;
    if (len_ptr) {
        if (!guest_read(len_ptr, &len, 4u) ||
            len < WSA_SOCKADDR_IN_SIZE || len > WC3_NET_ADDR_MAX ||
            !wc3_legacy_guest_ok(addr, len)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
    }

    Wc3NetSocket accepted = 0;
    int r = g_net->accept(g_net->ctx, host, &accepted,
                          addr ? (void *)(uintptr_t)addr : NULL,
                          len_ptr ? &len : NULL);
    if (r < 0) {
        c->eax = fail_net(r);
        return;
    }
    uint32_t guest = 0;
    if (!socket_alloc(accepted, &guest)) {
        (void)g_net->close(g_net->ctx, accepted);
        c->eax = fail_wsa(WSAENOBUFS);
        return;
    }
    if (len_ptr) guest_write(len_ptr, &len, 4u);
    c->eax = guest;
}

static void h_getpeername(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t addr = wc3_legacy_arg32(c, 1);
    uint32_t len_ptr = wc3_legacy_arg32(c, 2);
    if (!addr || !len_ptr) { c->eax = fail_wsa(WSAEFAULT); return; }
    uint32_t len = 0;
    if (!guest_read(len_ptr, &len, 4u) ||
        len < WSA_SOCKADDR_IN_SIZE || len > WC3_NET_ADDR_MAX ||
        !wc3_legacy_guest_ok(addr, len)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    int r = g_net->getpeername(g_net->ctx, host,
                               (void *)(uintptr_t)addr, &len);
    if (r < 0) { c->eax = fail_net(r); return; }
    guest_write(len_ptr, &len, 4u);
    c->eax = 0;
}

static void h_getsockname(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t addr = wc3_legacy_arg32(c, 1);
    uint32_t len_ptr = wc3_legacy_arg32(c, 2);
    if (!addr || !len_ptr) { c->eax = fail_wsa(WSAEFAULT); return; }
    uint32_t len = 0;
    if (!guest_read(len_ptr, &len, 4u) ||
        len < WSA_SOCKADDR_IN_SIZE || len > WC3_NET_ADDR_MAX ||
        !wc3_legacy_guest_ok(addr, len)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    int r = g_net->getsockname(g_net->ctx, host,
                               (void *)(uintptr_t)addr, &len);
    if (r < 0) { c->eax = fail_net(r); return; }
    guest_write(len_ptr, &len, 4u);
    c->eax = 0;
}

static void h_send(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t buf = wc3_legacy_arg32(c, 1);
    uint32_t len = wc3_legacy_arg32(c, 2);
    int flags = (int)wc3_legacy_arg32(c, 3);
    if (len && (!buf || !wc3_legacy_guest_ok(buf, len))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t sent = 0;
    int r = g_net->send(g_net->ctx, host,
                        (const void *)(uintptr_t)buf, len, flags, &sent);
    c->eax = r < 0 ? fail_net(r) : sent;
}

static void h_recv(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t buf = wc3_legacy_arg32(c, 1);
    uint32_t len = wc3_legacy_arg32(c, 2);
    int flags = (int)wc3_legacy_arg32(c, 3);
    if (len && (!buf || !wc3_legacy_guest_ok(buf, len))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t received = 0;
    int r = g_net->recv(g_net->ctx, host, (void *)(uintptr_t)buf, len,
                        flags, &received);
    c->eax = r < 0 ? fail_net(r) : received;
}

static void h_sendto(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t buf = wc3_legacy_arg32(c, 1);
    uint32_t len = wc3_legacy_arg32(c, 2);
    int flags = (int)wc3_legacy_arg32(c, 3);
    uint32_t addr = wc3_legacy_arg32(c, 4);
    uint32_t addr_len = wc3_legacy_arg32(c, 5);
    if ((len && (!buf || !wc3_legacy_guest_ok(buf, len))) ||
        !addr || addr_len < WSA_SOCKADDR_IN_SIZE || addr_len > WC3_NET_ADDR_MAX ||
        !wc3_legacy_guest_ok(addr, addr_len)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t sent = 0;
    int r = g_net->sendto(g_net->ctx, host, (const void *)(uintptr_t)buf,
                          len, flags, (const void *)(uintptr_t)addr,
                          addr_len, &sent);
    c->eax = r < 0 ? fail_net(r) : sent;
}

static void h_recvfrom(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t buf = wc3_legacy_arg32(c, 1);
    uint32_t len = wc3_legacy_arg32(c, 2);
    int flags = (int)wc3_legacy_arg32(c, 3);
    uint32_t addr = wc3_legacy_arg32(c, 4);
    uint32_t addr_len_ptr = wc3_legacy_arg32(c, 5);
    if (len && (!buf || !wc3_legacy_guest_ok(buf, len))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    if ((addr && !addr_len_ptr) || (!addr && addr_len_ptr)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t addr_len = 0;
    if (addr_len_ptr) {
        if (!guest_read(addr_len_ptr, &addr_len, 4u) ||
            addr_len > WC3_NET_ADDR_MAX ||
            (addr && !wc3_legacy_guest_ok(addr, addr_len))) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
    }
    uint32_t received = 0;
    int r = g_net->recvfrom(g_net->ctx, host, (void *)(uintptr_t)buf, len,
                            flags, addr ? (void *)(uintptr_t)addr : NULL,
                            addr_len_ptr ? &addr_len : NULL, &received);
    if (r < 0) { c->eax = fail_net(r); return; }
    if (addr_len_ptr) guest_write(addr_len_ptr, &addr_len, 4u);
    c->eax = received;
}

static void h_shutdown(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    int r = g_net->shutdown(g_net->ctx, host, (int)wc3_legacy_arg32(c, 1));
    c->eax = r < 0 ? fail_net(r) : 0;
}

/* ------------------------------------------------------------------ */
/* options, ioctls and select                                          */
/* ------------------------------------------------------------------ */

static int map_socket_option(uint32_t level, uint32_t option, int *mapped,
                             int *dont_linger)
{
    *dont_linger = 0;
    if (level == WSA_SOL_SOCKET) {
        switch (option) {
        case WSA_SO_REUSEADDR: *mapped = WC3_NET_OPT_REUSEADDR; return 1;
        case WSA_SO_KEEPALIVE: *mapped = WC3_NET_OPT_KEEPALIVE; return 1;
        case WSA_SO_DONTROUTE: *mapped = WC3_NET_OPT_DONTROUTE; return 1;
        case WSA_SO_BROADCAST: *mapped = WC3_NET_OPT_BROADCAST; return 1;
        case WSA_SO_LINGER: *mapped = WC3_NET_OPT_LINGER; return 1;
        case WSA_SO_OOBINLINE: *mapped = WC3_NET_OPT_OOBINLINE; return 1;
        case WSA_SO_SNDBUF: *mapped = WC3_NET_OPT_SNDBUF; return 1;
        case WSA_SO_RCVBUF: *mapped = WC3_NET_OPT_RCVBUF; return 1;
        case WSA_SO_SNDTIMEO: *mapped = WC3_NET_OPT_SNDTIMEO_MS; return 1;
        case WSA_SO_RCVTIMEO: *mapped = WC3_NET_OPT_RCVTIMEO_MS; return 1;
        case WSA_SO_ERROR: *mapped = WC3_NET_OPT_ERROR; return 1;
        case WSA_SO_TYPE: *mapped = WC3_NET_OPT_TYPE; return 1;
        case WSA_SO_EXCLUSIVEADDRUSE:
            *mapped = WC3_NET_OPT_EXCLUSIVEADDRUSE;
            return 1;
        case WSA_SO_DONTLINGER:
            *mapped = WC3_NET_OPT_LINGER;
            *dont_linger = 1;
            return 1;
        default: return 0;
        }
    }
    if (level == WSA_IPPROTO_TCP && option == WSA_TCP_NODELAY) {
        *mapped = WC3_NET_OPT_TCP_NODELAY;
        return 1;
    }
    return 0;
}

static void h_setsockopt(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t level = wc3_legacy_arg32(c, 1);
    uint32_t option = wc3_legacy_arg32(c, 2);
    uint32_t value = wc3_legacy_arg32(c, 3);
    uint32_t length = wc3_legacy_arg32(c, 4);
    int mapped, dont_linger;
    if (!map_socket_option(level, option, &mapped, &dont_linger)) {
        c->eax = fail_wsa(WSAEOPNOTSUPP);
        return;
    }
    if (!value || length > WSA_IOCTL_MAX ||
        (length && !wc3_legacy_guest_ok(value, length))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint8_t linger_off[4] = { 0, 0, 0, 0 };
    const void *data = dont_linger ? (const void *)linger_off
                                   : (const void *)(uintptr_t)value;
    uint32_t data_len = dont_linger ? 4u : length;
    int r = g_net->setsockopt(g_net->ctx, host, mapped, data, data_len);
    c->eax = r < 0 ? fail_net(r) : 0;
}

static void h_getsockopt(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t level = wc3_legacy_arg32(c, 1);
    uint32_t option = wc3_legacy_arg32(c, 2);
    uint32_t value = wc3_legacy_arg32(c, 3);
    uint32_t len_ptr = wc3_legacy_arg32(c, 4);
    int mapped, dont_linger;
    if (!map_socket_option(level, option, &mapped, &dont_linger)) {
        c->eax = fail_wsa(WSAEOPNOTSUPP);
        return;
    }
    uint32_t capacity = 0;
    if (!value || !len_ptr || !guest_read(len_ptr, &capacity, 4u)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    if (capacity > WSA_IOCTL_MAX) capacity = WSA_IOCTL_MAX;
    if (capacity < 4u || !wc3_legacy_guest_ok(value, capacity)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }

    uint32_t length = capacity;
    int r;
    if (dont_linger) {
        uint8_t raw[4] = { 0, 0, 0, 0 };
        length = 4u;
        r = g_net->getsockopt(g_net->ctx, host, WC3_NET_OPT_LINGER,
                              raw, &length);
        if (r >= 0) {
            uint32_t off = raw[0] ? 0u : 1u;
            uint32_t out_len = 4u;
            guest_write(value, &off, 4u);
            guest_write(len_ptr, &out_len, 4u);
            c->eax = 0;
            return;
        }
    } else {
        r = g_net->getsockopt(g_net->ctx, host, mapped,
                              (void *)(uintptr_t)value, &length);
    }
    if (r < 0) { c->eax = fail_net(r); return; }
    guest_write(len_ptr, &length, 4u);
    c->eax = 0;
}

static void h_ioctlsocket(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t command = wc3_legacy_arg32(c, 1);
    uint32_t argp = wc3_legacy_arg32(c, 2);
    if (!argp || !wc3_legacy_guest_ok(argp, 4u)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    if (command == WSA_FIONBIO) {
        uint32_t enable = wc3_legacy_rd32g(argp);
        int r = g_net->set_nonblocking(g_net->ctx, host, enable ? 1 : 0);
        c->eax = r < 0 ? fail_net(r) : 0;
        return;
    }
    if (command == WSA_FIONREAD) {
        uint32_t bytes = 0;
        int r = g_net->bytes_available(g_net->ctx, host, &bytes);
        if (r < 0) { c->eax = fail_net(r); return; }
        wc3_legacy_wr32g(argp, bytes);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIOCATMARK) {
        int mark = 0;
        int r = g_net->at_mark(g_net->ctx, host, &mark);
        if (r < 0) { c->eax = fail_net(r); return; }
        wc3_legacy_wr32g(argp, mark ? 1u : 0u);
        c->eax = 0;
        return;
    }
    c->eax = fail_wsa(WSAEINVAL);
}

static int fdset_from_guest(uint32_t guest, Wc3NetFdSet *out,
                            uint32_t *handles, int *present)
{
    *present = 0;
    if (!guest) return 0;
    uint32_t count = 0;
    if (!guest_read(guest, &count, 4u) || count > WSA_FD_SETSIZE ||
        !wc3_legacy_guest_ok(guest, 4u + count * 4u))
        return WSAEFAULT;
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t handle = wc3_legacy_rd32g(guest + 4u + i * 4u);
        Wc3NetSocket host = 0;
        if (!socket_lookup(handle, &host)) return WSAENOTSOCK;
        out->fd[i] = host;
        handles[i] = handle;
    }
    *present = 1;
    return 0;
}

/* select() needs the original guest handles to translate the ready host
 * values back.  Keep a small parallel snapshot per set. */
typedef struct WsaSelectSet {
    Wc3NetFdSet host;
    uint32_t guest[WSA_FD_SETSIZE];
    uint32_t guest_addr;
    int present;
} WsaSelectSet;

static void select_set_write(WsaSelectSet *set)
{
    if (!set || !set->present || !set->guest_addr) return;
    uint32_t count = set->host.count;
    if (count > WSA_FD_SETSIZE) count = WSA_FD_SETSIZE;
    wc3_legacy_wr32g(set->guest_addr, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t guest_handle = 0;
        for (uint32_t j = 0; j < WSA_FD_SETSIZE; j++) {
            if (!set->guest[j]) continue;
            Wc3NetSocket host = 0;
            if (socket_lookup(set->guest[j], &host) &&
                host == set->host.fd[i]) {
                guest_handle = set->guest[j];
                break;
            }
        }
        wc3_legacy_wr32g(set->guest_addr + 4u + i * 4u, guest_handle);
    }
}

static void h_select(CPU *c)
{
    WsaSelectSet read_set, write_set, except_set;
    memset(&read_set, 0, sizeof read_set);
    memset(&write_set, 0, sizeof write_set);
    memset(&except_set, 0, sizeof except_set);
    read_set.guest_addr = wc3_legacy_arg32(c, 1);
    write_set.guest_addr = wc3_legacy_arg32(c, 2);
    except_set.guest_addr = wc3_legacy_arg32(c, 3);

    int r = fdset_from_guest(read_set.guest_addr, &read_set.host,
                             read_set.guest, &read_set.present);
    if (!r)
        r = fdset_from_guest(write_set.guest_addr, &write_set.host,
                             write_set.guest, &write_set.present);
    if (!r)
        r = fdset_from_guest(except_set.guest_addr, &except_set.host,
                             except_set.guest, &except_set.present);
    if (r) { c->eax = fail_wsa(r); return; }
    if (!read_set.present && !write_set.present && !except_set.present) {
        c->eax = fail_wsa(WSAEINVAL);
        return;
    }

    int64_t timeout_us = -1;
    uint32_t tv = wc3_legacy_arg32(c, 4);
    if (tv) {
        int32_t sec = 0, usec = 0;
        if (!guest_read(tv, &sec, 4u) || !guest_read(tv + 4u, &usec, 4u) ||
            sec < 0 || usec < 0 || usec >= 1000000) {
            c->eax = fail_wsa(WSAEINVAL);
            return;
        }
        timeout_us = (int64_t)sec * 1000000 + usec;
    }

    int ready = g_net->select(g_net->ctx,
                              read_set.present ? &read_set.host : NULL,
                              write_set.present ? &write_set.host : NULL,
                              except_set.present ? &except_set.host : NULL,
                              timeout_us);
    if (ready < 0) { c->eax = fail_net(ready); return; }
    select_set_write(&read_set);
    select_set_write(&write_set);
    select_set_write(&except_set);
#if WC3_DEBUG && WC3_DEBUG_NET
    if (ready > 0)
        fprintf(stderr, "[net] select -> %d\n", ready);
#endif
    c->eax = (uint32_t)ready;
}

/* ------------------------------------------------------------------ */
/* byte order and address formatting                                   */
/* ------------------------------------------------------------------ */

static void h_htonl(CPU *c) { c->eax = bswap32(wc3_legacy_arg32(c, 0)); }
static void h_htons(CPU *c)
{
    c->eax = bswap16((uint16_t)(wc3_legacy_arg32(c, 0) & 0xFFFFu));
}
static void h_ntohl(CPU *c) { c->eax = bswap32(wc3_legacy_arg32(c, 0)); }
static void h_ntohs(CPU *c)
{
    c->eax = bswap16((uint16_t)(wc3_legacy_arg32(c, 0) & 0xFFFFu));
}

static void h_inet_addr(CPU *c)
{
    char text[64];
    uint32_t value = 0;
    if (!guest_cstr(wc3_legacy_arg32(c, 0), text, sizeof text) ||
        !parse_ipv4(text, &value)) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    c->eax = value;
}

static void h_inet_ntoa(CPU *c)
{
    static _Thread_local uint32_t cell;
    if (!cell) cell = wc3_legacy_guest_alloc0(32u);
    if (!cell) { c->eax = 0; return; }
    char text[32];
    if (!format_ipv4(wc3_legacy_arg32(c, 0), text, sizeof text)) {
        c->eax = 0;
        return;
    }
    guest_cstr_write(cell, text, 32u);
    c->eax = cell;
}

/* ------------------------------------------------------------------ */
/* name service                                                        */
/* ------------------------------------------------------------------ */

static void h_gethostname(CPU *c)
{
    uint32_t name = wc3_legacy_arg32(c, 0);
    uint32_t length = wc3_legacy_arg32(c, 1);
    if (!name || !length || !wc3_legacy_guest_ok(name, length)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    if (!g_net || !g_net->hostname) {
        c->eax = fail_wsa(WSASYSNOTREADY);
        return;
    }
    char host[256];
    host[0] = '\0';
    int r = g_net->hostname(g_net->ctx, host, sizeof host);
    if (r < 0) { c->eax = fail_net(r); return; }
    size_t n = strlen(host);
    if (n + 1u > length) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    if (!guest_cstr_write(name, host, length)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    c->eax = 0;
}

static void h_gethostbyname(CPU *c)
{
    char node[256];
    if (!guest_cstr(wc3_legacy_arg32(c, 0), node, sizeof node)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t addresses[32];
    uint32_t count = 0;
    if (!resolve_ipv4(node, addresses, 32u, &count) || !count) {
        c->eax = 0;
        g_wsa_error = WSAHOST_NOT_FOUND;
        return;
    }
    char canonical[256];
    canonical[0] = '\0';
    if (g_net && g_net->hostname) g_net->hostname(g_net->ctx, canonical,
                                                  sizeof canonical);
    const char *name = node[0] ? node : canonical;
    uint32_t hostent = alloc_hostent(name, addresses, count);
    if (!hostent) {
        c->eax = 0;
        g_wsa_error = WSAENOBUFS;
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] gethostbyname(%s) -> %u addr(s)\n", node,
            (unsigned)count);
#endif
    c->eax = hostent;
}

static void h_getaddrinfo(CPU *c)
{
    uint32_t node_ptr = wc3_legacy_arg32(c, 0);
    uint32_t service_ptr = wc3_legacy_arg32(c, 1);
    uint32_t hints_ptr = wc3_legacy_arg32(c, 2);
    uint32_t result_ptr = wc3_legacy_arg32(c, 3);
    char node[256], service[64];
    node[0] = '\0';
    service[0] = '\0';
    if (node_ptr && !guest_cstr(node_ptr, node, sizeof node)) {
        c->eax = WSAEINVAL;
        return;
    }
    if (service_ptr && !guest_cstr(service_ptr, service, sizeof service)) {
        c->eax = WSAEINVAL;
        return;
    }
    if (!result_ptr || !wc3_legacy_guest_ok(result_ptr, 4u)) {
        c->eax = WSAEINVAL;
        return;
    }

    Wc3NetResolveHints hints;
    memset(&hints, 0, sizeof hints);
    uint32_t guest_flags = 0;
    if (hints_ptr) {
        uint32_t raw[4];
        if (!guest_read(hints_ptr, raw, sizeof raw)) {
            c->eax = WSAEINVAL;
            return;
        }
        guest_flags = raw[0];
        int family, socktype;
        if (!map_family(raw[1], &family)) { c->eax = WSAEAFNOSUPPORT; return; }
        if (raw[2] && !map_socktype(raw[2], &socktype)) {
            c->eax = WSAEPROTONOSUPPORT;
            return;
        }
        hints.family = family;
        hints.socktype = raw[2] ? (int)raw[2] : 0;
        hints.protocol = (int)raw[3];
        if (guest_flags & 0x1u) hints.flags |= WC3_NET_AI_PASSIVE;
        if (guest_flags & 0x2u) hints.flags |= WC3_NET_AI_CANONNAME;
        if (guest_flags & 0x4u) hints.flags |= WC3_NET_AI_NUMERICHOST;
        if (guest_flags & 0x8u) hints.flags |= WC3_NET_AI_NUMERICSERV;
    }
    if (!g_net || !g_net->resolve) {
        c->eax = WSANO_RECOVERY;
        return;
    }

    Wc3NetAddrInfo info[16];
    uint32_t count = 0;
    int r = g_net->resolve(g_net->ctx, node_ptr ? node : NULL,
                           service_ptr ? service : NULL, &hints, info, 16u,
                           &count);
    if (r < 0) { c->eax = gai_error_code(r); return; }

    uint32_t head = 0, tail = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = wc3_legacy_guest_alloc0(WSA_ADDRINFO_SIZE);
        uint32_t addr = wc3_legacy_guest_alloc0(info[i].addr_length);
        if (!entry || !addr) {
            if (entry) wc3_legacy_guest_free(entry);
            if (addr) wc3_legacy_guest_free(addr);
            free_guest_addrinfo(head);
            c->eax = WSAENOBUFS;
            return;
        }
        guest_write(addr, info[i].addr, info[i].addr_length);
        uint32_t canon = 0;
        if ((guest_flags & 0x2u) && info[i].canonname[0]) {
            size_t n = strlen(info[i].canonname);
            canon = wc3_legacy_guest_alloc0((uint32_t)n + 1u);
            if (canon) guest_cstr_write(canon, info[i].canonname,
                                        (uint32_t)n + 1u);
        }
        wc3_legacy_wr32g(entry + 0, guest_flags);
        wc3_legacy_wr32g(entry + 4, (uint32_t)(info[i].family ==
                                                       WC3_NET_AF_INET
                                                   ? WSA_AF_INET
                                                   : WSA_AF_INET6));
        wc3_legacy_wr32g(entry + 8, (uint32_t)info[i].socktype);
        wc3_legacy_wr32g(entry + 12, (uint32_t)info[i].protocol);
        wc3_legacy_wr32g(entry + 16, info[i].addr_length);
        wc3_legacy_wr32g(entry + 20, addr);
        wc3_legacy_wr32g(entry + 24, canon);
        wc3_legacy_wr32g(entry + 28, 0);
        if (!head) head = entry;
        else wc3_legacy_wr32g(tail + 28, entry);
        tail = entry;
    }
    if (!head) { c->eax = WSAHOST_NOT_FOUND; return; }
    wc3_legacy_wr32g(result_ptr, head);
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] getaddrinfo(%s,%s) -> %u\n", node, service,
            (unsigned)count);
#endif
    c->eax = 0;
}

static void h_freeaddrinfo(CPU *c)
{
    free_guest_addrinfo(wc3_legacy_arg32(c, 0));
    c->eax = 0;
}

/* ------------------------------------------------------------------ */
/* WSAStartup / error state                                            */
/* ------------------------------------------------------------------ */

static void h_wsastartup(CPU *c)
{
    uint32_t version = wc3_legacy_arg32(c, 0);
    uint32_t data = wc3_legacy_arg32(c, 1);
    if (!data || !wc3_legacy_guest_ok(data, 400u)) {
        c->eax = WSAEFAULT;
        return;
    }
    if ((version & 0xFFu) < 1u || (version >> 8) > 2u) {
        c->eax = WSAVERNOTSUPPORTED;
        return;
    }
    if (!net_ready()) {
        c->eax = WSASYSNOTREADY;
        return;
    }
    if (g_net->startup) {
        int r = g_net->startup(g_net->ctx);
        if (r < 0) { c->eax = WSASYSNOTREADY; return; }
    }
    wc3_legacy_guest_zero(data, 400u);
    wc3_legacy_wr16g(data + 0, 0x0202u);
    wc3_legacy_wr16g(data + 2, 0x0202u);
    guest_cstr_write(data + 4u, "WinSock 2.0", 257u);
    guest_cstr_write(data + 261u, "Running", 129u);
    wc3_legacy_wr16g(data + 390u, 0u);
    wc3_legacy_wr16g(data + 392u, 1472u);
    wc3_legacy_wr32g(data + 396u, 0u);
    g_wsa_error = 0;
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] WSAStartup(0x%04X) ok\n", (unsigned)version);
#endif
    c->eax = 0;
}

static void h_wsacleanup(CPU *c)
{
    c->eax = 0;
}

static void h_wsagetlasterror(CPU *c)
{
    c->eax = (uint32_t)g_wsa_error;
}

static void h_wsasetlasterror(CPU *c)
{
    g_wsa_error = (int)wc3_legacy_arg32(c, 0);
}

static void h_wsafdisset(CPU *c)
{
    uint32_t handle = wc3_legacy_arg32(c, 0);
    uint32_t set = wc3_legacy_arg32(c, 1);
    uint32_t count = 0;
    if (!set || !guest_read(set, &count, 4u) || count > WSA_FD_SETSIZE ||
        !wc3_legacy_guest_ok(set, 4u + count * 4u)) {
        c->eax = 0;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (wc3_legacy_rd32g(set + 4u + i * 4u) == handle) {
            c->eax = 1;
            return;
        }
    }
    c->eax = 0;
}

static void h_wsaasyncselect(CPU *c)
{
    /* The 1.27 network layer polls with select(); the as-built tree made the
     * same observation.  Returning success keeps the optional call from
     * turning into a startup failure. */
    (void)c;
    c->eax = 0;
}

static void h_wsarecvex(CPU *c)
{
    Wc3NetSocket host = 0;
    if (!socket_from_arg(c, 0, &host)) { c->eax = WSA_SOCKET_ERROR; return; }
    uint32_t buf = wc3_legacy_arg32(c, 1);
    uint32_t len = wc3_legacy_arg32(c, 2);
    uint32_t flags_ptr = wc3_legacy_arg32(c, 3);
    int flags = 0;
    if (flags_ptr && !guest_read(flags_ptr, &flags, 4u)) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    flags &= (WSA_MSG_OOB | WSA_MSG_PEEK | WSA_MSG_DONTROUTE);
    if (len && (!buf || !wc3_legacy_guest_ok(buf, len))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }
    uint32_t received = 0;
    int r = g_net->recv(g_net->ctx, host, (void *)(uintptr_t)buf, len,
                        flags, &received);
    if (r < 0) { c->eax = fail_net(r); return; }
    if (flags_ptr) {
        uint32_t out = 0;
        guest_write(flags_ptr, &out, 4u);
    }
    c->eax = received;
}

/* ------------------------------------------------------------------ */
/* WSAIoctl                                                            */
/* ------------------------------------------------------------------ */

static int write_ioctl_bytes(uint32_t ptr, uint32_t value)
{
    if (!ptr) return 0;
    return guest_write(ptr, &value, 4u);
}

static int guid_equals(uint32_t guest, const uint8_t *guid)
{
    uint8_t raw[WSA_GUID_SIZE];
    return guest_read(guest, raw, sizeof raw) && !memcmp(raw, guid, sizeof raw);
}

static const uint8_t kWsaIdWSARecvEx[WSA_GUID_SIZE] = {
    0xC8, 0xD7, 0x89, 0xF6, 0x1F, 0x6F, 0xD2, 0x11,
    0x8B, 0x43, 0x00, 0xA0, 0xC9, 0x22, 0xE7, 0x07
};

static void h_wsaioctl(CPU *c)
{
    Wc3NetSocket host = 0;
    uint32_t handle = socket_from_arg(c, 0, &host);
    if (!handle) { c->eax = WSA_SOCKET_ERROR; return; }

    uint32_t command = wc3_legacy_arg32(c, 1);
    uint32_t inbuf = wc3_legacy_arg32(c, 2);
    uint32_t inlen = wc3_legacy_arg32(c, 3);
    uint32_t outbuf = wc3_legacy_arg32(c, 4);
    uint32_t outlen = wc3_legacy_arg32(c, 5);
    uint32_t bytes_ptr = wc3_legacy_arg32(c, 6);

    if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 0);
    if (inlen > WSA_IOCTL_MAX || outlen > WSA_IOCTL_MAX ||
        (inlen && (!inbuf || !wc3_legacy_guest_ok(inbuf, inlen))) ||
        (outlen && (!outbuf || !wc3_legacy_guest_ok(outbuf, outlen)))) {
        c->eax = fail_wsa(WSAEFAULT);
        return;
    }

    if (command == WSA_FIONBIO) {
        uint32_t enable = 0;
        if (inlen < 4u || !guest_read(inbuf, &enable, 4u)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        int r = g_net->set_nonblocking(g_net->ctx, host, enable ? 1 : 0);
        c->eax = r < 0 ? fail_net(r) : 0;
        return;
    }
    if (command == WSA_FIONREAD) {
        uint32_t bytes = 0;
        int r = g_net->bytes_available(g_net->ctx, host, &bytes);
        if (r < 0) { c->eax = fail_net(r); return; }
        if (outlen < 4u || !guest_write(outbuf, &bytes, 4u)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 4u);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIOCATMARK) {
        int mark = 0;
        int r = g_net->at_mark(g_net->ctx, host, &mark);
        if (r < 0) { c->eax = fail_net(r); return; }
        uint32_t value = mark ? 1u : 0u;
        if (outlen < 4u || !guest_write(outbuf, &value, 4u)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 4u);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_GET_EXTENSION_FUNCTION_POINTER) {
        if (inlen < WSA_GUID_SIZE || outlen < 4u) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        uint32_t thunk = 0;
        if (guid_equals(inbuf, kWsaIdWSARecvEx))
            thunk = wc3_legacy_hle_thunk_of("mswsock.dll", "WSARecvEx");
        if (!thunk) {
            c->eax = fail_wsa(WSAEOPNOTSUPP);
            return;
        }
        if (!guest_write(outbuf, &thunk, 4u)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 4u);
#if WC3_DEBUG && WC3_DEBUG_NET
        fprintf(stderr, "[net] WSAIoctl(extension WSARecvEx) -> %08X\n",
                (unsigned)thunk);
#endif
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_ADDRESS_LIST_QUERY) {
        Wc3NetInterface interfaces[32];
        uint32_t count = 0;
        if (!g_net->interfaces ||
            g_net->interfaces(g_net->ctx, interfaces, 32u, &count) < 0) {
            c->eax = fail_wsa(WSAENETDOWN);
            return;
        }
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; i++)
            if (interfaces[i].flags & WC3_NET_IF_UP) n++;
        uint32_t required = 4u + n * 8u + n * WSA_SOCKADDR_IN_SIZE;
        if (!outbuf || outlen < required) {
            if (bytes_ptr) write_ioctl_bytes(bytes_ptr, required);
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        wc3_legacy_wr32g(outbuf, n);
        uint32_t addr_base = outbuf + 4u + n * 8u;
        uint32_t written = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!(interfaces[i].flags & WC3_NET_IF_UP)) continue;
            uint32_t entry = outbuf + 4u + written * 8u;
            uint32_t addr = addr_base + written * WSA_SOCKADDR_IN_SIZE;
            wc3_legacy_wr32g(entry + 0, addr);
            wc3_legacy_wr32g(entry + 4, WSA_SOCKADDR_IN_SIZE);
            write_sockaddr_in(addr, interfaces[i].address, 0);
            written++;
        }
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, required);
#if WC3_DEBUG && WC3_DEBUG_NET
        fprintf(stderr, "[net] WSAIoctl(address list) -> %u\n", (unsigned)n);
#endif
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_GET_BROADCAST_ADDRESS) {
        Wc3NetInterface interfaces[32];
        uint32_t count = 0;
        uint32_t address = 0xFFFFFFFFu;
        if (g_net->interfaces &&
            g_net->interfaces(g_net->ctx, interfaces, 32u, &count) >= 0) {
            for (uint32_t i = 0; i < count; i++) {
                if ((interfaces[i].flags & WC3_NET_IF_UP) &&
                    !(interfaces[i].flags & WC3_NET_IF_LOOPBACK) &&
                    interfaces[i].broadcast) {
                    address = interfaces[i].broadcast;
                    break;
                }
            }
        }
        if (outlen < WSA_SOCKADDR_IN_SIZE || !outbuf) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        write_sockaddr_in(outbuf, address, 0);
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, WSA_SOCKADDR_IN_SIZE);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_ROUTING_INTERFACE_QUERY) {
        uint32_t destination = 0;
        if (inlen < WSA_SOCKADDR_IN_SIZE ||
            !read_sockaddr_in(inbuf, inlen, &destination, NULL)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        Wc3NetInterface interfaces[32];
        uint32_t count = 0;
        uint32_t source = 0;
        if (g_net->interfaces &&
            g_net->interfaces(g_net->ctx, interfaces, 32u, &count) >= 0) {
            for (uint32_t i = 0; i < count; i++) {
                if (!(interfaces[i].flags & WC3_NET_IF_UP) ||
                    (interfaces[i].flags & WC3_NET_IF_LOOPBACK))
                    continue;
                uint32_t mask = interfaces[i].netmask;
                if (mask && (destination & mask) ==
                                (interfaces[i].address & mask)) {
                    source = interfaces[i].address;
                    break;
                }
            }
            if (!source) {
                for (uint32_t i = 0; i < count; i++) {
                    if ((interfaces[i].flags & WC3_NET_IF_UP) &&
                        !(interfaces[i].flags & WC3_NET_IF_LOOPBACK)) {
                        source = interfaces[i].address;
                        break;
                    }
                }
            }
        }
        if (!source) { c->eax = fail_wsa(WSAENETUNREACH); return; }
        if (outlen < WSA_SOCKADDR_IN_SIZE || !outbuf) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        write_sockaddr_in(outbuf, source, 0);
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, WSA_SOCKADDR_IN_SIZE);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_ADDRESS_LIST_CHANGE) {
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 0u);
        c->eax = 0;
        return;
    }
    if (command == WSA_SIO_BASE_HANDLE || command == WSA_SIO_BSP_HANDLE ||
        command == WSA_SIO_BSP_HANDLE_SELECT ||
        command == WSA_SIO_BSP_HANDLE_POLL) {
        if (outlen < 4u || !guest_write(outbuf, &handle, 4u)) {
            c->eax = fail_wsa(WSAEFAULT);
            return;
        }
        if (bytes_ptr) write_ioctl_bytes(bytes_ptr, 4u);
        c->eax = 0;
        return;
    }

    c->eax = fail_wsa(WSAEOPNOTSUPP);
}

/* ------------------------------------------------------------------ */
/* synthetic modules                                                   */
/* ------------------------------------------------------------------ */

static const char *module_name_from_handle(uint32_t handle)
{
    if (handle == WSA_HMOD_WS2_32) return "ws2_32.dll";
    if (handle == WSA_HMOD_MSWSOCK) return "mswsock.dll";
    return NULL;
}

static uint32_t module_handle_from_name(const char *name)
{
    if (!name) return 0;
    const char *leaf = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    if (ascii_ieq(leaf, "ws2_32.dll") || ascii_ieq(leaf, "ws2_32"))
        return WSA_HMOD_WS2_32;
    if (ascii_ieq(leaf, "mswsock.dll") || ascii_ieq(leaf, "mswsock"))
        return WSA_HMOD_MSWSOCK;
    return 0;
}

static const char *ordinal_name(const WsaOrdinal *table, size_t count,
                                uint16_t ordinal)
{
    for (size_t i = 0; i < count; i++)
        if (table[i].ordinal == ordinal) return table[i].name;
    return NULL;
}

static void (*previous_load_library)(CPU *c);
static void (*previous_get_proc_address)(CPU *c);
static void (*previous_get_module_handle)(CPU *c);
static void (*previous_get_module_file_name)(CPU *c);

static void h_load_library_a(CPU *c)
{
    uint32_t name_ptr = wc3_legacy_arg32(c, 0);
    char name[128];
    if (guest_cstr(name_ptr, name, sizeof name)) {
        uint32_t module = module_handle_from_name(name);
        if (module) {
#if WC3_DEBUG && WC3_DEBUG_NET
            fprintf(stderr, "[net] LoadLibraryA(%s) -> synthetic %08X\n",
                    name, (unsigned)module);
#endif
            c->eax = module;
            return;
        }
    }
    if (previous_load_library) previous_load_library(c);
    else {
        wc3_legacy_set_last_error(126);
        c->eax = 0;
    }
}

static void h_get_proc_address(CPU *c)
{
    uint32_t module = wc3_legacy_arg32(c, 0);
    uint32_t name_or_ordinal = wc3_legacy_arg32(c, 1);
    const char *dll = module_name_from_handle(module);
    if (!dll) {
        if (previous_get_proc_address) previous_get_proc_address(c);
        else {
            wc3_legacy_set_last_error(127);
            c->eax = 0;
        }
        return;
    }

    const char *name = NULL;
    char name_buf[128];
    if (name_or_ordinal <= 0xFFFFu) {
        if (module == WSA_HMOD_WS2_32)
            name = ordinal_name(g_ws2_ordinals,
                                sizeof g_ws2_ordinals / sizeof g_ws2_ordinals[0],
                                (uint16_t)name_or_ordinal);
        if (!name) {
            wc3_legacy_set_last_error(127);
            c->eax = 0;
            return;
        }
    } else {
        if (!guest_cstr(name_or_ordinal, name_buf, sizeof name_buf)) {
            wc3_legacy_set_last_error(127);
            c->eax = 0;
            return;
        }
        name = name_buf;
    }

    uint32_t thunk = wc3_legacy_hle_thunk_of(dll, name);
    if (!thunk) {
        wc3_legacy_set_last_error(127);
        c->eax = 0;
        return;
    }
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] GetProcAddress(%s,%s) -> %08X\n", dll, name,
            (unsigned)thunk);
#endif
    c->eax = thunk;
}

static void h_get_module_handle_a(CPU *c)
{
    uint32_t name_ptr = wc3_legacy_arg32(c, 0);
    char name[128];
    if (guest_cstr(name_ptr, name, sizeof name)) {
        uint32_t module = module_handle_from_name(name);
        if (module) {
            c->eax = module;
            return;
        }
    }
    if (previous_get_module_handle) previous_get_module_handle(c);
    else c->eax = 0;
}

static void h_get_module_file_name_a(CPU *c)
{
    uint32_t module = wc3_legacy_arg32(c, 0);
    uint32_t buffer = wc3_legacy_arg32(c, 1);
    uint32_t capacity = wc3_legacy_arg32(c, 2);
    const char *name = module_name_from_handle(module);
    if (!name) {
        if (previous_get_module_file_name) previous_get_module_file_name(c);
        else c->eax = 0;
        return;
    }
    size_t n = strlen(name);
    if (!buffer || !capacity) { c->eax = 0; return; }
    uint32_t copy = (uint32_t)n;
    if (copy >= capacity) copy = capacity - 1u;
    if (!wc3_legacy_guest_ok(buffer, copy + 1u)) {
        c->eax = 0;
        return;
    }
    if (copy) memcpy((void *)(uintptr_t)buffer, name, copy);
    wc3_legacy_wr8g(buffer + copy, 0);
    c->eax = copy;
}

/* ------------------------------------------------------------------ */
/* install                                                             */
/* ------------------------------------------------------------------ */

static const WsaFunc *find_function(const char *name)
{
    for (size_t i = 0; i < sizeof g_common_functions /
                               sizeof g_common_functions[0]; i++)
        if (ascii_ieq(g_common_functions[i].name, name))
            return &g_common_functions[i];
    for (size_t i = 0; i < sizeof g_ws2_functions /
                               sizeof g_ws2_functions[0]; i++)
        if (ascii_ieq(g_ws2_functions[i].name, name))
            return &g_ws2_functions[i];
    for (size_t i = 0; i < sizeof g_mswsock_functions /
                               sizeof g_mswsock_functions[0]; i++)
        if (ascii_ieq(g_mswsock_functions[i].name, name))
            return &g_mswsock_functions[i];
    return NULL;
}

static int add_named(const char *dll, const WsaFunc *functions, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (wc3_legacy_hle_add(dll, functions[i].name, functions[i].argc, 0,
                               functions[i].fn) != 0)
            return -1;
    }
    return 0;
}

static int bind_ordinals(const char *dll, const WsaOrdinal *table, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const WsaFunc *fn = find_function(table[i].name);
        if (!fn) return -1;
        if (wc3_legacy_hle_bind_ordinal(dll, table[i].ordinal, fn->argc, 0,
                                        fn->fn) != 0)
            return -1;
    }
    return 0;
}

int wc3_hle_ws2_32_install(void)
{
    if (g_installed) return g_installed;

    const Wc3Platform *platform = wc3_platform();
    if (!platform) return -1;
    g_net = &platform->net;
    if (!net_ready() || !g_net->bind || !g_net->connect || !g_net->listen ||
        !g_net->accept || !g_net->setsockopt || !g_net->getsockopt ||
        !g_net->set_nonblocking || !g_net->bytes_available ||
        !g_net->at_mark || !g_net->hostname || !g_net->resolve ||
        !g_net->interfaces)
        return -1;

    if (add_named("WS2_32.dll", g_common_functions,
                  sizeof g_common_functions / sizeof g_common_functions[0]) != 0)
        return -1;
    if (add_named("WS2_32.dll", g_ws2_functions,
                  sizeof g_ws2_functions / sizeof g_ws2_functions[0]) != 0)
        return -1;
    if (add_named("WSOCK32.dll", g_common_functions,
                  sizeof g_common_functions / sizeof g_common_functions[0]) != 0)
        return -1;
    if (add_named("mswsock.dll", g_mswsock_functions,
                  sizeof g_mswsock_functions /
                      sizeof g_mswsock_functions[0]) != 0)
        return -1;

    if (bind_ordinals("WS2_32.dll", g_ws2_ordinals,
                      sizeof g_ws2_ordinals / sizeof g_ws2_ordinals[0]) != 0)
        return -1;
    if (bind_ordinals("WSOCK32.dll", g_wsock32_ordinals,
                      sizeof g_wsock32_ordinals / sizeof g_wsock32_ordinals[0]) != 0)
        return -1;

    if (wc3_legacy_hle_chain("KERNEL32.dll", "LoadLibraryA",
                             h_load_library_a,
                             &previous_load_library) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "GetProcAddress",
                             h_get_proc_address,
                             &previous_get_proc_address) != 0)
        return -1;
    (void)wc3_legacy_hle_chain("KERNEL32.dll", "GetModuleHandleA",
                               h_get_module_handle_a,
                               &previous_get_module_handle);
    (void)wc3_legacy_hle_chain("KERNEL32.dll", "GetModuleFileNameA",
                               h_get_module_file_name_a,
                               &previous_get_module_file_name);

    g_installed = (int)(sizeof g_common_functions /
                            sizeof g_common_functions[0] +
                        sizeof g_ws2_functions / sizeof g_ws2_functions[0] +
                        sizeof g_ws2_ordinals / sizeof g_ws2_ordinals[0] +
                        sizeof g_wsock32_ordinals /
                            sizeof g_wsock32_ordinals[0]);
#if WC3_DEBUG && WC3_DEBUG_NET
    fprintf(stderr, "[net] ws2_32 HLE installed (%d rows)\n", g_installed);
#endif
    return g_installed;
}
