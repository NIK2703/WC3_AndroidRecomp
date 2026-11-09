/*
 * net_android.c - POSIX socket implementation of Wc3NetApi for Android.
 *
 * The winsock HLE owns guest handles, WinSock structures and error values.
 * This file only translates the platform-neutral socket operations to the
 * Android/Linux socket API and converts errno to the WC3_NET_ERR_* namespace.
 * It is deliberately not a second Winsock implementation.
 */
#include "platform_android.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <ifaddrs.h>

static int net_err(int e)
{
    switch (e) {
    case 0: return 0;
    case EAGAIN: return -WC3_NET_ERR_AGAIN;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return -WC3_NET_ERR_WOULDBLOCK;
#endif
    case EINTR: return -WC3_NET_ERR_INTR;
    case EBADF: return -WC3_NET_ERR_BADF;
    case EACCES: return -WC3_NET_ERR_ACCES;
    case EFAULT: return -WC3_NET_ERR_FAULT;
    case EINVAL: return -WC3_NET_ERR_INVAL;
    case EMFILE: return -WC3_NET_ERR_MFILE;
    case ENOMEM: return -WC3_NET_ERR_NOMEM;
    case ENOTSOCK: return -WC3_NET_ERR_NOTSOCK;
    case ECONNREFUSED: return -WC3_NET_ERR_CONNREFUSED;
    case ECONNRESET: return -WC3_NET_ERR_CONNRESET;
    case ECONNABORTED: return -WC3_NET_ERR_CONNABORTED;
    case EHOSTUNREACH: return -WC3_NET_ERR_HOSTUNREACH;
    case ENETUNREACH: return -WC3_NET_ERR_NETUNREACH;
    case ETIMEDOUT: return -WC3_NET_ERR_TIMEDOUT;
    case EINPROGRESS: return -WC3_NET_ERR_INPROGRESS;
    case EALREADY: return -WC3_NET_ERR_ALREADY;
    case EMSGSIZE: return -WC3_NET_ERR_MSGSIZE;
    case EADDRINUSE: return -WC3_NET_ERR_ADDRINUSE;
    case EADDRNOTAVAIL: return -WC3_NET_ERR_ADDRNOTAVAIL;
    case ENETDOWN: return -WC3_NET_ERR_NETDOWN;
    case ENOTCONN: return -WC3_NET_ERR_NOTCONN;
    case EDESTADDRREQ: return -WC3_NET_ERR_DESTADDRREQ;
    case EAFNOSUPPORT: return -WC3_NET_ERR_AFNOSUPPORT;
    case EPROTONOSUPPORT: return -WC3_NET_ERR_PROTONOSUPPORT;
    case EOPNOTSUPP: return -WC3_NET_ERR_OPNOTSUPP;
    case EISCONN: return -WC3_NET_ERR_ISCONN;
    case ENODATA: return -WC3_NET_ERR_NODATA;
    case ENOBUFS: return -WC3_NET_ERR_NOBUFS;
    case EPIPE: return -WC3_NET_ERR_PIPE;
    case EIO: return -WC3_NET_ERR_IO;
    default: return -WC3_NET_ERR_UNKNOWN;
    }
}

static int net_fd(Wc3NetSocket s)
{
    if (s > (Wc3NetSocket)INT_MAX) return -1;
    return (int)s;
}

static int net_msg_flags(int flags, int sending)
{
    int out = 0;
    if (flags & WC3_NET_MSG_OOB) out |= MSG_OOB;
    if (flags & WC3_NET_MSG_PEEK) out |= MSG_PEEK;
    if (flags & WC3_NET_MSG_DONTROUTE) out |= MSG_DONTROUTE;
#ifdef MSG_NOSIGNAL
    /* Android raises SIGPIPE for a peer that has gone away.  The guest
     * expects send/sendto to report WSAECONNRESET/WSAECONNABORTED instead of
     * terminating the whole process. */
    if (sending) out |= MSG_NOSIGNAL;
#else
    (void)sending;
#endif
    return out;
}

/* Android gives every app a default network (usually mobile data).  A
 * hotspot/AP or a Wi-Fi client is a separate local network, so a limited
 * broadcast (255.255.255.255) sent on the default network never reaches the
 * LAN.  Select the LAN interface natively and steer only broadcast datagrams
 * there with IP_PKTINFO; ordinary Internet traffic keeps the default network.
 * The interface name is the stable native selector and needs no Java
 * ConnectivityManager call. */
static int is_lan_ifname(const char *name)
{
    if (!name || !name[0]) return 0;
    if (strncmp(name, "rmnet", 5) == 0 ||
        strncmp(name, "ccmni", 5) == 0 ||
        strncmp(name, "pdp", 3) == 0 ||
        strncmp(name, "tun", 3) == 0 ||
        strncmp(name, "ppp", 3) == 0 ||
        strncmp(name, "dummy", 5) == 0 ||
        strncmp(name, "lo", 2) == 0)
        return 0;
    return strncmp(name, "wlan", 4) == 0 ||
           strncmp(name, "swlan", 5) == 0 ||
           strncmp(name, "softap", 6) == 0 ||
           strncmp(name, "ap", 2) == 0 ||
           strncmp(name, "eth", 3) == 0 ||
           strncmp(name, "rndis", 5) == 0 ||
           strncmp(name, "usb", 3) == 0 ||
           strncmp(name, "br", 2) == 0;
}

static int preferred_lan_info(char *out, size_t cap, unsigned *ifindex,
                              uint32_t *address)
{
    if (!out || cap == 0) return 0;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return 0;
    int found = 0;
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(ifa->ifa_flags & IFF_UP) ||
                (ifa->ifa_flags & IFF_LOOPBACK))
                continue;
            if (!is_lan_ifname(ifa->ifa_name)) continue;
            /* Prefer an interface that can carry subnet broadcast. */
            if (pass == 0 && !(ifa->ifa_flags & IFF_BROADCAST)) continue;
            const struct sockaddr_in *sin =
                (const struct sockaddr_in *)ifa->ifa_addr;
            snprintf(out, cap, "%s", ifa->ifa_name);
            if (ifindex) *ifindex = if_nametoindex(ifa->ifa_name);
            if (address) *address = sin->sin_addr.s_addr;
            found = 1;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

/* Send the limited broadcast (255.255.255.255) on the LAN interface instead
 * of the default network.  IP_PKTINFO selects the outgoing interface and
 * source address per datagram, so a socket that also talks to Internet hosts
 * keeps its normal route.  A subnet broadcast (for example 10.0.0.255) is
 * already routed on-link by the kernel and does not need this path. */
static int send_lan_broadcast(int fd, const void *buf, uint32_t length,
                              int flags, const void *addr,
                              uint32_t addr_length, uint32_t *sent)
{
    if (!addr || addr_length < sizeof(struct sockaddr_in)) return 0;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET ||
        sin->sin_addr.s_addr != INADDR_BROADCAST)
        return 0;

    char name[IFNAMSIZ];
    unsigned ifindex = 0;
    uint32_t address = 0;
    if (!preferred_lan_info(name, sizeof name, &ifindex, &address) ||
        ifindex == 0)
        return 0;

    int type = 0;
    socklen_t type_length = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0 ||
        type != SOCK_DGRAM)
        return 0;

    struct iovec iov;
    iov.iov_base = (void *)(uintptr_t)buf;
    iov.iov_len = length;
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_name = (void *)(uintptr_t)addr;
    message.msg_namelen = (socklen_t)addr_length;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;

    char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
    memset(control, 0, sizeof control);
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = IPPROTO_IP;
    header->cmsg_type = IP_PKTINFO;
    header->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo info;
    memset(&info, 0, sizeof info);
    info.ipi_ifindex = (int)ifindex;
    info.ipi_spec_dst.s_addr = address;
    memcpy(CMSG_DATA(header), &info, sizeof info);

    ssize_t n = sendmsg(fd, &message, net_msg_flags(flags, 1));
    if (n < 0) {
#if WC3_DEBUG && WC3_DEBUG_NET
        fprintf(stderr, "[net] broadcast if=%s idx=%u failed: %s\n", name,
                ifindex, strerror(errno));
#endif
        return 0;
    }
#if WC3_DEBUG && WC3_DEBUG_NET
    char destination_text[INET_ADDRSTRLEN] = "?";
    char source_text[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &sin->sin_addr, destination_text,
              sizeof destination_text);
    inet_ntop(AF_INET, &address, source_text, sizeof source_text);
    fprintf(stderr, "[net] broadcast %s:%u via %s idx=%u src=%s\n",
            destination_text, (unsigned)ntohs(sin->sin_port), name, ifindex,
            source_text);
#endif
    *sent = (uint32_t)n;
    return 1;
}

static int android_net_startup(void *ctx)
{
    (void)ctx;
    return 0;
}

static void android_net_cleanup(void *ctx)
{
    (void)ctx;
}

static int android_net_socket(void *ctx, int family, int type, int protocol,
                              Wc3NetSocket *out)
{
    (void)ctx;
    if (!out) return -WC3_NET_ERR_INVAL;
    int fd = socket(family, type, protocol);
    if (fd < 0) return net_err(errno);
    *out = (Wc3NetSocket)fd;
    return 0;
}

static int android_net_close(void *ctx, Wc3NetSocket s)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    if (close(fd) != 0) return net_err(errno);
    return 0;
}

static int android_net_bind(void *ctx, Wc3NetSocket s, const void *addr,
                            uint32_t addr_length)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    if (!addr || addr_length < sizeof(sa_family_t))
        return -WC3_NET_ERR_INVAL;
    if (bind(fd, (const struct sockaddr *)addr, (socklen_t)addr_length) != 0)
        return net_err(errno);
    return 0;
}

static int android_net_connect(void *ctx, Wc3NetSocket s, const void *addr,
                               uint32_t addr_length)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    if (!addr || addr_length < sizeof(sa_family_t))
        return -WC3_NET_ERR_INVAL;
    if (connect(fd, (const struct sockaddr *)addr, (socklen_t)addr_length) != 0)
        return net_err(errno);
    return 0;
}

static int android_net_listen(void *ctx, Wc3NetSocket s, int backlog)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    if (listen(fd, backlog) != 0) return net_err(errno);
    return 0;
}

static int android_net_accept(void *ctx, Wc3NetSocket s, Wc3NetSocket *out,
                              void *addr, uint32_t *addr_length)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !out) return -WC3_NET_ERR_BADF;
    socklen_t len = addr_length ? (socklen_t)*addr_length : 0;
    struct sockaddr_storage ss;
    struct sockaddr *sa = (addr && addr_length) ? (struct sockaddr *)&ss
                                                 : NULL;
    int nfd = accept(fd, sa, sa ? &len : NULL);
    if (nfd < 0) return net_err(errno);
    if (sa) {
        uint32_t cap = *addr_length;
        uint32_t n = (uint32_t)len;
        if (n > cap) n = cap;
        if (n > sizeof ss) n = sizeof ss;
        if (n) memcpy(addr, &ss, n);
        /* Winsock callers size the result buffer with `addrlen` as an
         * in/out value.  Never report bytes the backend did not copy. */
        *addr_length = n;
    }
    *out = (Wc3NetSocket)nfd;
    return 0;
}

static int android_net_send(void *ctx, Wc3NetSocket s, const void *buf,
                            uint32_t length, int flags, uint32_t *sent)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || (!buf && length) || !sent) return -WC3_NET_ERR_INVAL;
    ssize_t n = send(fd, buf, length, net_msg_flags(flags, 1));
    if (n < 0) return net_err(errno);
    *sent = (uint32_t)n;
    return 0;
}

static int android_net_recv(void *ctx, Wc3NetSocket s, void *buf,
                            uint32_t length, int flags, uint32_t *received)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || (!buf && length) || !received) return -WC3_NET_ERR_INVAL;
    ssize_t n = recv(fd, buf, length, net_msg_flags(flags, 0));
    if (n < 0) return net_err(errno);
    *received = (uint32_t)n;
    return 0;
}

static int android_net_sendto(void *ctx, Wc3NetSocket s, const void *buf,
                              uint32_t length, int flags, const void *addr,
                              uint32_t addr_length, uint32_t *sent)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || (!buf && length) || !addr || !sent)
        return -WC3_NET_ERR_INVAL;
    if (send_lan_broadcast(fd, buf, length, flags, addr, addr_length, sent))
        return 0;
    ssize_t n = sendto(fd, buf, length, net_msg_flags(flags, 1),
                       (const struct sockaddr *)addr,
                       (socklen_t)addr_length);
    if (n < 0) return net_err(errno);
    *sent = (uint32_t)n;
    return 0;
}

static int android_net_recvfrom(void *ctx, Wc3NetSocket s, void *buf,
                                uint32_t length, int flags, void *addr,
                                uint32_t *addr_length, uint32_t *received)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || (!buf && length) || !received) return -WC3_NET_ERR_INVAL;
    socklen_t len = (addr && addr_length) ? (socklen_t)*addr_length : 0;
    struct sockaddr_storage ss;
    struct sockaddr *sa = (addr && addr_length) ? (struct sockaddr *)&ss
                                                 : NULL;
    ssize_t n = recvfrom(fd, buf, length, net_msg_flags(flags, 0),
                         sa, sa ? &len : NULL);
    if (n < 0) return net_err(errno);
    if (sa) {
        uint32_t cap = *addr_length;
        uint32_t copy = (uint32_t)len;
        if (copy > cap) copy = cap;
        if (copy > sizeof ss) copy = sizeof ss;
        if (copy) memcpy(addr, &ss, copy);
        *addr_length = copy;
    }
    *received = (uint32_t)n;
    return 0;
}

static int android_net_shutdown(void *ctx, Wc3NetSocket s, int how)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    int sh = SHUT_RD;
    if (how == WC3_NET_SHUT_WR) sh = SHUT_WR;
    else if (how == WC3_NET_SHUT_RDWR) sh = SHUT_RDWR;
    if (shutdown(fd, sh) != 0) return net_err(errno);
    return 0;
}

static int android_net_peer(void *ctx, Wc3NetSocket s, void *addr,
                            uint32_t *addr_length, int peer)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !addr || !addr_length) return -WC3_NET_ERR_INVAL;
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)*addr_length;
    int r = peer ? getpeername(fd, (struct sockaddr *)&ss, &len)
                 : getsockname(fd, (struct sockaddr *)&ss, &len);
    if (r != 0) return net_err(errno);
    uint32_t cap = *addr_length;
    uint32_t n = (uint32_t)len;
    if (n > cap) n = cap;
    if (n > sizeof ss) n = sizeof ss;
    if (n) memcpy(addr, &ss, n);
    *addr_length = n;
    return 0;
}

static int android_net_getpeername(void *ctx, Wc3NetSocket s, void *addr,
                                   uint32_t *addr_length)
{
    return android_net_peer(ctx, s, addr, addr_length, 1);
}

static int android_net_getsockname(void *ctx, Wc3NetSocket s, void *addr,
                                   uint32_t *addr_length)
{
    return android_net_peer(ctx, s, addr, addr_length, 0);
}

static int android_net_setsockopt(void *ctx, Wc3NetSocket s, int option,
                                  const void *value, uint32_t length)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !value) return -WC3_NET_ERR_INVAL;

    int level = SOL_SOCKET;
    int name = 0;
    int ivalue = 0;
    struct linger linger;
    struct timeval tv;

    switch (option) {
    case WC3_NET_OPT_REUSEADDR: name = SO_REUSEADDR; break;
    case WC3_NET_OPT_EXCLUSIVEADDRUSE:
        name = SO_REUSEADDR;
        ivalue = 0;
        break;
    case WC3_NET_OPT_BROADCAST: name = SO_BROADCAST; break;
    case WC3_NET_OPT_KEEPALIVE: name = SO_KEEPALIVE; break;
    case WC3_NET_OPT_DONTROUTE: name = SO_DONTROUTE; break;
    case WC3_NET_OPT_OOBINLINE: name = SO_OOBINLINE; break;
    case WC3_NET_OPT_RCVBUF: name = SO_RCVBUF; break;
    case WC3_NET_OPT_SNDBUF: name = SO_SNDBUF; break;
    case WC3_NET_OPT_RCVTIMEO_MS:
    case WC3_NET_OPT_SNDTIMEO_MS: {
        if (length < 4u) return -WC3_NET_ERR_INVAL;
        uint32_t ms = 0;
        memcpy(&ms, value, sizeof ms);
        tv.tv_sec = (time_t)(ms / 1000u);
        tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
        name = option == WC3_NET_OPT_RCVTIMEO_MS ? SO_RCVTIMEO : SO_SNDTIMEO;
        if (setsockopt(fd, SOL_SOCKET, name, &tv, sizeof tv) != 0)
            return net_err(errno);
        return 0;
    }
    case WC3_NET_OPT_LINGER:
        if (length < 4u) return -WC3_NET_ERR_INVAL;
        uint16_t onoff = 0, seconds = 0;
        memcpy(&onoff, value, sizeof onoff);
        memcpy(&seconds, (const uint8_t *)value + 2, sizeof seconds);
        linger.l_onoff = onoff ? 1 : 0;
        linger.l_linger = (int)seconds;
        if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof linger) != 0)
            return net_err(errno);
        return 0;
    case WC3_NET_OPT_TCP_NODELAY:
        level = IPPROTO_TCP;
        name = TCP_NODELAY;
        break;
    default:
        return -WC3_NET_ERR_OPNOTSUPP;
    }

    if (option == WC3_NET_OPT_EXCLUSIVEADDRUSE) {
        if (setsockopt(fd, level, name, &ivalue, sizeof ivalue) != 0)
            return net_err(errno);
        return 0;
    }
    if (length < 4u) return -WC3_NET_ERR_INVAL;
    memcpy(&ivalue, value, sizeof ivalue);
    if (setsockopt(fd, level, name, &ivalue, sizeof ivalue) != 0)
        return net_err(errno);
    return 0;
}

static int android_net_getsockopt(void *ctx, Wc3NetSocket s, int option,
                                  void *value, uint32_t *length)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !value || !length) return -WC3_NET_ERR_INVAL;

    uint32_t cap = *length;
    int ivalue = 0;
    socklen_t ilen = sizeof ivalue;
    int level = SOL_SOCKET;
    int name = 0;

    switch (option) {
    case WC3_NET_OPT_ERROR: {
        int host_error = 0;
        socklen_t elen = sizeof host_error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &host_error, &elen) != 0)
            return net_err(errno);
        if (cap < 4u) return -WC3_NET_ERR_INVAL;
        uint32_t code = host_error
                            ? (uint32_t)(-net_err(host_error))
                            : (uint32_t)WC3_NET_ERR_NONE;
        memcpy(value, &code, sizeof code);
        *length = 4u;
        return 0;
    }
    case WC3_NET_OPT_TYPE: name = SO_TYPE; break;
    case WC3_NET_OPT_RCVBUF: name = SO_RCVBUF; break;
    case WC3_NET_OPT_SNDBUF: name = SO_SNDBUF; break;
    case WC3_NET_OPT_REUSEADDR: name = SO_REUSEADDR; break;
    case WC3_NET_OPT_BROADCAST: name = SO_BROADCAST; break;
    case WC3_NET_OPT_KEEPALIVE: name = SO_KEEPALIVE; break;
    case WC3_NET_OPT_DONTROUTE: name = SO_DONTROUTE; break;
    case WC3_NET_OPT_OOBINLINE: name = SO_OOBINLINE; break;
    case WC3_NET_OPT_TCP_NODELAY:
        level = IPPROTO_TCP;
        name = TCP_NODELAY;
        break;
    case WC3_NET_OPT_RCVTIMEO_MS:
    case WC3_NET_OPT_SNDTIMEO_MS: {
        struct timeval tv;
        socklen_t tvlen = sizeof tv;
        name = option == WC3_NET_OPT_RCVTIMEO_MS ? SO_RCVTIMEO : SO_SNDTIMEO;
        if (getsockopt(fd, SOL_SOCKET, name, &tv, &tvlen) != 0)
            return net_err(errno);
        if (cap < 4u) return -WC3_NET_ERR_INVAL;
        uint32_t ms = (uint32_t)tv.tv_sec * 1000u +
                      (uint32_t)tv.tv_usec / 1000u;
        memcpy(value, &ms, sizeof ms);
        *length = 4u;
        return 0;
    }
    case WC3_NET_OPT_LINGER: {
        struct linger linger;
        socklen_t llen = sizeof linger;
        if (getsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, &llen) != 0)
            return net_err(errno);
        if (cap < 4u) return -WC3_NET_ERR_INVAL;
        uint16_t onoff = linger.l_onoff ? 1u : 0u;
        uint16_t seconds = (uint16_t)(linger.l_linger > 0xFFFF
                                          ? 0xFFFF
                                          : (linger.l_linger < 0
                                                 ? 0
                                                 : linger.l_linger));
        memcpy(value, &onoff, sizeof onoff);
        memcpy((uint8_t *)value + 2, &seconds, sizeof seconds);
        *length = 4u;
        return 0;
    }
    case WC3_NET_OPT_EXCLUSIVEADDRUSE:
        if (cap < 4u) return -WC3_NET_ERR_INVAL;
        ivalue = 1;
        memcpy(value, &ivalue, sizeof ivalue);
        *length = 4u;
        return 0;
    default:
        return -WC3_NET_ERR_OPNOTSUPP;
    }

    if (getsockopt(fd, level, name, &ivalue, &ilen) != 0)
        return net_err(errno);
    if (cap < 4u) return -WC3_NET_ERR_INVAL;
    memcpy(value, &ivalue, sizeof ivalue);
    *length = 4u;
    return 0;
}

static int android_net_set_nonblocking(void *ctx, Wc3NetSocket s, int enable)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0) return -WC3_NET_ERR_BADF;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return net_err(errno);
    if (enable) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) != 0) return net_err(errno);
    return 0;
}

static int android_net_bytes_available(void *ctx, Wc3NetSocket s, uint32_t *out)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !out) return -WC3_NET_ERR_INVAL;
    int value = 0;
    if (ioctl(fd, FIONREAD, &value) != 0) return net_err(errno);
    *out = value > 0 ? (uint32_t)value : 0u;
    return 0;
}

static int android_net_at_mark(void *ctx, Wc3NetSocket s, int *out)
{
    (void)ctx;
    int fd = net_fd(s);
    if (fd < 0 || !out) return -WC3_NET_ERR_INVAL;
    int value = 0;
    if (ioctl(fd, SIOCATMARK, &value) != 0) return net_err(errno);
    *out = value ? 1 : 0;
    return 0;
}

static int android_net_select(void *ctx, Wc3NetFdSet *read, Wc3NetFdSet *write,
                              Wc3NetFdSet *except, int64_t timeout_us)
{
    (void)ctx;
    struct pollfd fds[WC3_NET_FD_SETSIZE * 3u];
    uint8_t kinds[WC3_NET_FD_SETSIZE * 3u];
    uint32_t indexes[WC3_NET_FD_SETSIZE * 3u];
    uint32_t nfds = 0;

    struct { Wc3NetFdSet *set; uint8_t kind; } groups[3] = {
        { read, 0 }, { write, 1 }, { except, 2 }
    };
    for (unsigned g = 0; g < 3; g++) {
        if (!groups[g].set) continue;
        uint32_t count = groups[g].set->count;
        if (count > WC3_NET_FD_SETSIZE) count = WC3_NET_FD_SETSIZE;
        for (uint32_t i = 0; i < count; i++) {
            int fd = net_fd(groups[g].set->fd[i]);
            if (fd < 0) return -WC3_NET_ERR_BADF;
            fds[nfds].fd = fd;
            fds[nfds].events = groups[g].kind == 0 ? POLLIN
                              : groups[g].kind == 1 ? POLLOUT
                                                    : POLLPRI;
            fds[nfds].revents = 0;
            kinds[nfds] = groups[g].kind;
            indexes[nfds] = i;
            nfds++;
        }
    }

    int timeout_ms;
    if (timeout_us < 0) timeout_ms = -1;
    else {
        int64_t ms = (timeout_us + 999) / 1000;
        if (ms > INT_MAX) ms = INT_MAX;
        timeout_ms = (int)ms;
    }

    int ready = poll(fds, (nfds_t)nfds, timeout_ms);
    if (ready < 0) return net_err(errno);

    if (read) read->count = 0;
    if (write) write->count = 0;
    if (except) except->count = 0;

    for (uint32_t i = 0; i < nfds; i++) {
        short rev = fds[i].revents;
        int hit = 0;
        if (kinds[i] == 0) hit = (rev & (POLLIN | POLLHUP | POLLERR)) != 0;
        else if (kinds[i] == 1) hit = (rev & (POLLOUT | POLLHUP | POLLERR)) != 0;
        else hit = (rev & (POLLPRI | POLLERR)) != 0;
        if (!hit) continue;
        Wc3NetFdSet *set = kinds[i] == 0 ? read
                          : kinds[i] == 1 ? write
                                          : except;
        if (!set || set->count >= WC3_NET_FD_SETSIZE) continue;
        set->fd[set->count++] = (Wc3NetSocket)fds[i].fd;
    }

    return ready;
}

static int android_net_hostname(void *ctx, char *buf, uint32_t cap)
{
    (void)ctx;
    if (!buf || cap == 0u) return -WC3_NET_ERR_INVAL;
    if (gethostname(buf, cap) != 0) return net_err(errno);
    buf[cap - 1u] = '\0';
    return 0;
}

static int net_gai_err(int e)
{
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return -WC3_NET_ERR_AGAIN;
    case EAI_NONAME: return -WC3_NET_ERR_NONAME;
    case EAI_MEMORY: return -WC3_NET_ERR_NOMEM;
    case EAI_FAMILY: return -WC3_NET_ERR_AFNOSUPPORT;
    case EAI_SOCKTYPE: return -WC3_NET_ERR_PROTONOSUPPORT;
    case EAI_SERVICE: return -WC3_NET_ERR_NODATA;
    default: return -WC3_NET_ERR_UNKNOWN;
    }
}

static int android_net_resolve(void *ctx, const char *node, const char *service,
                               const Wc3NetResolveHints *hints,
                               Wc3NetAddrInfo *out, uint32_t cap,
                               uint32_t *count)
{
    (void)ctx;
    if (!out || !count) return -WC3_NET_ERR_INVAL;
    *count = 0;

    struct addrinfo h;
    memset(&h, 0, sizeof h);
    if (hints) {
        h.ai_family = hints->family;
        h.ai_socktype = hints->socktype;
        h.ai_protocol = hints->protocol;
        if (hints->flags & WC3_NET_AI_PASSIVE) h.ai_flags |= AI_PASSIVE;
        if (hints->flags & WC3_NET_AI_CANONNAME) h.ai_flags |= AI_CANONNAME;
        if (hints->flags & WC3_NET_AI_NUMERICHOST) h.ai_flags |= AI_NUMERICHOST;
        if (hints->flags & WC3_NET_AI_NUMERICSERV) h.ai_flags |= AI_NUMERICSERV;
    }

    struct addrinfo *res = NULL;
    int r = getaddrinfo(node, service, hints ? &h : NULL, &res);
    if (r != 0) return net_gai_err(r);

    uint32_t n = 0;
    for (struct addrinfo *ai = res; ai && n < cap; ai = ai->ai_next) {
        if (ai->ai_addrlen > WC3_NET_ADDR_MAX) continue;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        Wc3NetAddrInfo *dst = &out[n++];
        memset(dst, 0, sizeof *dst);
        dst->family = ai->ai_family;
        dst->socktype = ai->ai_socktype;
        dst->protocol = ai->ai_protocol;
        dst->flags = 0;
        dst->addr_length = (uint32_t)ai->ai_addrlen;
        if (ai->ai_addr && ai->ai_addrlen)
            memcpy(dst->addr, ai->ai_addr, (size_t)ai->ai_addrlen);
        if (ai->ai_canonname) {
            size_t len = strlen(ai->ai_canonname);
            if (len >= sizeof dst->canonname) len = sizeof dst->canonname - 1u;
            memcpy(dst->canonname, ai->ai_canonname, len);
            dst->canonname[len] = '\0';
        }
    }
    freeaddrinfo(res);
    if (n == 0) return -WC3_NET_ERR_NONAME;
    *count = n;
    return 0;
}

static int android_net_interfaces(void *ctx, Wc3NetInterface *out,
                                  uint32_t cap, uint32_t *count)
{
    (void)ctx;
    if (!out || !count) return -WC3_NET_ERR_INVAL;
    *count = 0;

    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return net_err(errno);

    Wc3NetInterface lan[WC3_NET_FD_SETSIZE];
    Wc3NetInterface other[WC3_NET_FD_SETSIZE];
    Wc3NetInterface loop[WC3_NET_FD_SETSIZE];
    uint32_t lan_count = 0;
    uint32_t other_count = 0;
    uint32_t loop_count = 0;

    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        const struct sockaddr_in *sin =
            (const struct sockaddr_in *)ifa->ifa_addr;
        Wc3NetInterface item;
        memset(&item, 0, sizeof item);
        item.address = sin->sin_addr.s_addr;
        item.flags = (ifa->ifa_flags & IFF_LOOPBACK)
                         ? WC3_NET_IF_UP | WC3_NET_IF_LOOPBACK
                         : WC3_NET_IF_UP;
        if (ifa->ifa_netmask &&
            ifa->ifa_netmask->sa_family == AF_INET) {
            const struct sockaddr_in *mask =
                (const struct sockaddr_in *)ifa->ifa_netmask;
            item.netmask = mask->sin_addr.s_addr;
        }
        if (ifa->ifa_broadaddr &&
            ifa->ifa_broadaddr->sa_family == AF_INET &&
            (ifa->ifa_flags & IFF_BROADCAST)) {
            const struct sockaddr_in *bc =
                (const struct sockaddr_in *)ifa->ifa_broadaddr;
            item.broadcast = bc->sin_addr.s_addr;
        } else if (item.netmask) {
            item.broadcast = item.address | ~item.netmask;
        }

        if (item.flags & WC3_NET_IF_LOOPBACK) {
            if (loop_count < WC3_NET_FD_SETSIZE)
                loop[loop_count++] = item;
        } else if (is_lan_ifname(ifa->ifa_name)) {
            if (lan_count < WC3_NET_FD_SETSIZE)
                lan[lan_count++] = item;
        } else if (other_count < WC3_NET_FD_SETSIZE) {
            other[other_count++] = item;
        }
    }
    freeifaddrs(list);

    /* gethostbyname(), SIO_GET_BROADCAST_ADDRESS and address-list callers take
     * the first usable entry.  Keep the real LAN interface ahead of mobile
     * data and VPN addresses so discovery does not advertise 10.x cellular. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < lan_count && n < cap; i++)
        out[n++] = lan[i];
    for (uint32_t i = 0; i < other_count && n < cap; i++)
        out[n++] = other[i];
    for (uint32_t i = 0; i < loop_count && n < cap; i++)
        out[n++] = loop[i];
    *count = n;
    return 0;
}

void wc3_android_net_install(Wc3NetApi *api)
{
    if (!api) return;
    memset(api, 0, sizeof *api);
    api->ctx = NULL;
    api->startup = android_net_startup;
    api->cleanup = android_net_cleanup;
    api->socket = android_net_socket;
    api->close = android_net_close;
    api->bind = android_net_bind;
    api->connect = android_net_connect;
    api->listen = android_net_listen;
    api->accept = android_net_accept;
    api->send = android_net_send;
    api->recv = android_net_recv;
    api->sendto = android_net_sendto;
    api->recvfrom = android_net_recvfrom;
    api->shutdown = android_net_shutdown;
    api->getpeername = android_net_getpeername;
    api->getsockname = android_net_getsockname;
    api->setsockopt = android_net_setsockopt;
    api->getsockopt = android_net_getsockopt;
    api->set_nonblocking = android_net_set_nonblocking;
    api->bytes_available = android_net_bytes_available;
    api->at_mark = android_net_at_mark;
    api->select = android_net_select;
    api->hostname = android_net_hostname;
    api->resolve = android_net_resolve;
    api->interfaces = android_net_interfaces;
}
