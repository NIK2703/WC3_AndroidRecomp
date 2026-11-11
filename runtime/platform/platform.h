/*
 * platform.h - the only OS boundary for the Android target.
 *
 * HLE modules use these function tables; they do not include Android, POSIX or
 * Windows headers.  The real implementation lives under
 * runtime/platform/android, the fake implementation under runtime/platform/fake.
 *
 * NetApi is the POSIX-shaped socket surface used by the winsock HLE.  It is
 * deliberately socket-level, not Winsock-level: the guest-visible structures,
 * handles, error codes and event semantics stay in runtime/hle/win32/ws2_32.
 */
#ifndef WC3_PLATFORM_H
#define WC3_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WC3_MEM_READ = 1u << 0,
    WC3_MEM_WRITE = 1u << 1,
    WC3_MEM_EXEC = 1u << 2
};

typedef struct Wc3MemApi {
    void *ctx;
    /* Reserve guest address space at a fixed VA.  Returns 0 on success. */
    int (*reserve)(void *ctx, uint32_t guest_base, uint32_t size);
    int (*commit)(void *ctx, uint32_t guest_base, uint32_t size);
    int (*decommit)(void *ctx, uint32_t guest_base, uint32_t size);
    int (*release)(void *ctx, uint32_t guest_base, uint32_t size);
    int (*protect)(void *ctx, uint32_t guest_base, uint32_t size, uint32_t prot);
} Wc3MemApi;

enum {
    WC3_FILE_READ = 1u << 0,
    WC3_FILE_WRITE = 1u << 1,
    WC3_FILE_CREATE = 1u << 2,
    WC3_FILE_TRUNCATE = 1u << 3,
    WC3_FILE_APPEND = 1u << 4,
    WC3_FILE_EXCLUSIVE = 1u << 5
};

enum {
    WC3_FILE_STAT_REGULAR = 1u << 0,
    WC3_FILE_STAT_DIRECTORY = 1u << 1
};

typedef struct Wc3FileStat {
    uint64_t size;
    int64_t mtime_seconds;
    uint32_t mode; /* WC3_FILE_STAT_REGULAR or WC3_FILE_STAT_DIRECTORY */
} Wc3FileStat;

typedef struct Wc3FileApi {
    void *ctx;
    /* Paths here are host-visible paths after the HLE path mapper has run. */
    int (*open)(void *ctx, const char *path, int flags, uintptr_t *out);
    int (*read)(void *ctx, uintptr_t file, void *buf, uint32_t len, uint32_t *read);
    int (*write)(void *ctx, uintptr_t file, const void *buf, uint32_t len, uint32_t *written);
    int (*seek)(void *ctx, uintptr_t file, int64_t offset, int whence, uint64_t *pos);
    int (*size)(void *ctx, uintptr_t file, uint64_t *size);
    int (*stat)(void *ctx, const char *path, Wc3FileStat *out);
    int (*close)(void *ctx, uintptr_t file);
} Wc3FileApi;

/* ------------------------------------------------------------------ net */

/* Socket values are host-opaque.  The winsock HLE keeps its own guest handle
 * table and never exposes these values to the guest. */
typedef uintptr_t Wc3NetSocket;

#define WC3_NET_FD_SETSIZE 64u
#define WC3_NET_ADDR_MAX 128u
#define WC3_NET_CANON_MAX 256u

enum {
    WC3_NET_AF_UNSPEC = 0,
    WC3_NET_AF_INET = 2,
    WC3_NET_AF_INET6 = 10
};

enum {
    WC3_NET_SOCK_STREAM = 1,
    WC3_NET_SOCK_DGRAM = 2
};

enum {
    WC3_NET_IPPROTO_IP = 0,
    WC3_NET_IPPROTO_TCP = 6,
    WC3_NET_IPPROTO_UDP = 17
};

enum {
    WC3_NET_AI_PASSIVE = 1u << 0,
    WC3_NET_AI_CANONNAME = 1u << 1,
    WC3_NET_AI_NUMERICHOST = 1u << 2,
    WC3_NET_AI_NUMERICSERV = 1u << 3,
    WC3_NET_AI_V4MAPPED = 1u << 4,
    WC3_NET_AI_ALL = 1u << 5
};

enum {
    WC3_NET_SHUT_RD = 0,
    WC3_NET_SHUT_WR = 1,
    WC3_NET_SHUT_RDWR = 2
};

/* Platform-neutral message flags.  The winsock HLE maps the guest MSG_*
 * values to this set; a backend must not interpret the guest constants. */
enum {
    WC3_NET_MSG_OOB = 1u << 0,
    WC3_NET_MSG_PEEK = 1u << 1,
    WC3_NET_MSG_DONTROUTE = 1u << 2
};

/* Platform-neutral socket options.  The winsock HLE translates SO_* values,
 * including the DWORD-millisecond timeout form, before calling this table.
 * WC3_NET_OPT_ERROR returns a positive WC3_NET_ERR_* value (0 when no error is
 * pending), never a host errno. */
enum {
    WC3_NET_OPT_REUSEADDR = 1,
    WC3_NET_OPT_EXCLUSIVEADDRUSE,
    WC3_NET_OPT_BROADCAST,
    WC3_NET_OPT_KEEPALIVE,
    WC3_NET_OPT_DONTROUTE,
    WC3_NET_OPT_OOBINLINE,
    WC3_NET_OPT_RCVBUF,
    WC3_NET_OPT_SNDBUF,
    WC3_NET_OPT_RCVTIMEO_MS,
    WC3_NET_OPT_SNDTIMEO_MS,
    WC3_NET_OPT_LINGER,
    WC3_NET_OPT_TCP_NODELAY,
    WC3_NET_OPT_ERROR,
    WC3_NET_OPT_TYPE
};

/* NetApi failures are returned as -WC3_NET_ERR_*.  WC3_NET_ERR_NONE is not a
 * failure and is never returned.  Keeping the mapping here means both the
 * Android and verification backends translate host errno exactly once. */
enum {
    WC3_NET_ERR_NONE = 0,
    WC3_NET_ERR_AGAIN = 1,
    WC3_NET_ERR_INTR,
    WC3_NET_ERR_BADF,
    WC3_NET_ERR_ACCES,
    WC3_NET_ERR_FAULT,
    WC3_NET_ERR_INVAL,
    WC3_NET_ERR_MFILE,
    WC3_NET_ERR_NOMEM,
    WC3_NET_ERR_NOTSOCK,
    WC3_NET_ERR_CONNREFUSED,
    WC3_NET_ERR_CONNRESET,
    WC3_NET_ERR_CONNABORTED,
    WC3_NET_ERR_HOSTUNREACH,
    WC3_NET_ERR_NETUNREACH,
    WC3_NET_ERR_TIMEDOUT,
    WC3_NET_ERR_WOULDBLOCK,
    WC3_NET_ERR_INPROGRESS,
    WC3_NET_ERR_ALREADY,
    WC3_NET_ERR_MSGSIZE,
    WC3_NET_ERR_ADDRINUSE,
    WC3_NET_ERR_ADDRNOTAVAIL,
    WC3_NET_ERR_NETDOWN,
    WC3_NET_ERR_NOTCONN,
    WC3_NET_ERR_DESTADDRREQ,
    WC3_NET_ERR_AFNOSUPPORT,
    WC3_NET_ERR_PROTONOSUPPORT,
    WC3_NET_ERR_OPNOTSUPP,
    WC3_NET_ERR_ISCONN,
    WC3_NET_ERR_NODATA,
    WC3_NET_ERR_NONAME,
    WC3_NET_ERR_NOBUFS,
    WC3_NET_ERR_PIPE,
    WC3_NET_ERR_IO,
    WC3_NET_ERR_UNKNOWN
};

typedef struct Wc3NetFdSet {
    uint32_t count;
    Wc3NetSocket fd[WC3_NET_FD_SETSIZE];
} Wc3NetFdSet;

typedef struct Wc3NetAddrInfo {
    int32_t family;
    int32_t socktype;
    int32_t protocol;
    int32_t flags;
    uint32_t addr_length;
    uint8_t addr[WC3_NET_ADDR_MAX];
    char canonname[WC3_NET_CANON_MAX];
} Wc3NetAddrInfo;

typedef struct Wc3NetResolveHints {
    int32_t family;
    int32_t socktype;
    int32_t protocol;
    int32_t flags;
} Wc3NetResolveHints;

enum {
    WC3_NET_IF_UP = 1u << 0,
    WC3_NET_IF_LOOPBACK = 1u << 1
};

typedef struct Wc3NetInterface {
    uint32_t address;   /* IPv4 in network byte order */
    uint32_t netmask;   /* IPv4 in network byte order */
    uint32_t broadcast; /* IPv4 in network byte order */
    uint32_t flags;     /* WC3_NET_IF_* */
} Wc3NetInterface;

typedef struct Wc3NetApi {
    void *ctx;
    /* Process-wide socket runtime.  Both calls are idempotent. */
    int (*startup)(void *ctx);
    void (*cleanup)(void *ctx);

    int (*socket)(void *ctx, int family, int type, int protocol,
                  Wc3NetSocket *out);
    int (*close)(void *ctx, Wc3NetSocket socket);
    int (*bind)(void *ctx, Wc3NetSocket socket, const void *addr,
                uint32_t addr_length);
    int (*connect)(void *ctx, Wc3NetSocket socket, const void *addr,
                   uint32_t addr_length);
    int (*listen)(void *ctx, Wc3NetSocket socket, int backlog);
    int (*accept)(void *ctx, Wc3NetSocket socket, Wc3NetSocket *out,
                  void *addr, uint32_t *addr_length);
    int (*send)(void *ctx, Wc3NetSocket socket, const void *buf,
                uint32_t length, int flags, uint32_t *sent);
    int (*recv)(void *ctx, Wc3NetSocket socket, void *buf, uint32_t length,
                int flags, uint32_t *received);
    int (*sendto)(void *ctx, Wc3NetSocket socket, const void *buf,
                  uint32_t length, int flags, const void *addr,
                  uint32_t addr_length, uint32_t *sent);
    int (*recvfrom)(void *ctx, Wc3NetSocket socket, void *buf,
                    uint32_t length, int flags, void *addr,
                    uint32_t *addr_length, uint32_t *received);
    int (*shutdown)(void *ctx, Wc3NetSocket socket, int how);
    int (*getpeername)(void *ctx, Wc3NetSocket socket, void *addr,
                       uint32_t *addr_length);
    int (*getsockname)(void *ctx, Wc3NetSocket socket, void *addr,
                       uint32_t *addr_length);
    int (*setsockopt)(void *ctx, Wc3NetSocket socket, int option,
                      const void *value, uint32_t length);
    int (*getsockopt)(void *ctx, Wc3NetSocket socket, int option,
                      void *value, uint32_t *length);
    int (*set_nonblocking)(void *ctx, Wc3NetSocket socket, int enable);
    int (*bytes_available)(void *ctx, Wc3NetSocket socket, uint32_t *out);
    int (*at_mark)(void *ctx, Wc3NetSocket socket, int *out);
    /* timeout_us < 0 waits forever.  The sets are both in/out. */
    int (*select)(void *ctx, Wc3NetFdSet *read, Wc3NetFdSet *write,
                  Wc3NetFdSet *except, int64_t timeout_us);

    int (*hostname)(void *ctx, char *buf, uint32_t cap);
    int (*resolve)(void *ctx, const char *node, const char *service,
                   const Wc3NetResolveHints *hints, Wc3NetAddrInfo *out,
                   uint32_t cap, uint32_t *count);
    int (*interfaces)(void *ctx, Wc3NetInterface *out, uint32_t cap,
                      uint32_t *count);
} Wc3NetApi;

typedef struct Wc3TimeApi {
    void *ctx;
    uint64_t (*monotonic_ns)(void *ctx);
    uint64_t (*realtime_ns)(void *ctx);
    /* Local time in the guest's configured timezone; returns 0 on success. */
    int (*local_time)(void *ctx, uint64_t unix_ns, int32_t *year, int32_t *month,
                      int32_t *day, int32_t *hour, int32_t *minute,
                      int32_t *second, int32_t *millisecond);
} Wc3TimeApi;

typedef struct Wc3LocaleApi {
    void *ctx;
    uint32_t (*ansi_codepage)(void *ctx);
    uint32_t (*oem_codepage)(void *ctx);
    uint32_t (*default_lcid)(void *ctx);
    /* Wide/ANSI conversion.  The HLE string layer owns buffer limits; this
     * only performs the encoding step. */
    int (*to_wide)(void *ctx, uint32_t codepage, const char *src, int32_t src_len,
                   uint16_t *dst, int32_t dst_len);
    int (*from_wide)(void *ctx, uint32_t codepage, const uint16_t *src,
                     int32_t src_len, char *dst, int32_t dst_len);
} Wc3LocaleApi;

typedef struct Wc3WindowApi {
    void *ctx;
    int (*create)(void *ctx, uint32_t width, uint32_t height, const char *title,
                  uintptr_t *out);
    void (*destroy)(void *ctx, uintptr_t window);
    int (*size)(void *ctx, uintptr_t window, uint32_t *width, uint32_t *height);
    int (*set_title)(void *ctx, uintptr_t window, const char *title);
} Wc3WindowApi;

enum {
    WC3_INPUT_POINTER_DOWN = 1,
    WC3_INPUT_POINTER_UP = 2,
    WC3_INPUT_POINTER_MOVE = 3,
    WC3_INPUT_KEY_DOWN = 4,
    WC3_INPUT_KEY_UP = 5,
    WC3_INPUT_TEXT = 6,
    WC3_INPUT_FOCUS = 7
};

typedef struct Wc3InputEvent {
    uint32_t kind;
    uint32_t pointer_id;
    int32_t x;
    int32_t y;
    uint32_t key;
    uint32_t codepoint;
    uint32_t modifiers;
    /* Monotonic host timestamp.  The producer may leave this zero; push()
     * fills it before the event enters the queue. */
    uint64_t time_ns;
} Wc3InputEvent;

typedef struct Wc3InputApi {
    void *ctx;
    /* The Android callback copies into the input mailbox; it never calls HLE
     * or the dispatcher directly.  One producer (the activity callback thread)
     * and one consumer (the owning guest thread) are required; a full mailbox
     * returns -1 and increments the dropped-event diagnostic counter. */
    int (*push)(void *ctx, const Wc3InputEvent *event);
    int (*set_text_focus)(void *ctx, int focused);
} Wc3InputApi;

typedef struct Wc3AudioApi {
    void *ctx;
    /* One AAudio stream.  open() also creates the platform primary looping
     * voice, so the callback is always in voice-mixer mode. */
    int (*open)(void *ctx, uint32_t sample_rate, uint32_t channels,
                uint32_t bits, uintptr_t *out);
    int (*close)(void *ctx, uintptr_t stream);

    /* Host-owned looping voices mixed in the AAudio real-time callback.
     * Frames in this block are SOURCE frames; rate conversion belongs to the
     * mixer, never to the HLE.  The platform counts in-flight control calls
     * and close() drains them before freeing PCM/sequence storage. */
    int (*voice_add)(void *ctx, uintptr_t stream, uint32_t rate,
                     uint32_t channels, uint32_t bits, uint32_t bytes,
                     uintptr_t *out_voice);
    int (*voice_remove)(void *ctx, uintptr_t voice);
    int (*voice_publish)(void *ctx, uintptr_t voice, uint64_t frame_base,
                         const void *frames, uint32_t bytes,
                         uint32_t *written);
    /* DirectSound GetCurrentPosition: play/write cursors in BYTES, modulo the
     * looping buffer, plus the absolute source-frame play cursor.  write is
     * play + one 10 ms lookahead, exactly as the DirectSound guest contract. */
    int (*voice_position)(void *ctx, uintptr_t voice, uint32_t *play_bytes,
                          uint32_t *write_bytes, uint64_t *play_frame);
    int (*voice_control)(void *ctx, uintptr_t voice, int playing, int loop,
                         uint32_t frequency, int volume, int pan);
    /* Actual AAudio sample rate (the voice-mixer rate), 0 on headless. */
    uint32_t (*device_rate)(void *ctx, uintptr_t stream);
    /* The platform primary looping voice, created at open(). */
    uintptr_t (*primary_voice)(void *ctx, uintptr_t stream);
    /* DirectSound SetCurrentPosition. */
    int (*voice_seek)(void *ctx, uintptr_t voice, uint64_t abs_frame);
    /* DirectSound GetStatus: current per-voice playing/loop flags and the
     * absolute SOURCE play cursor. */
    int (*voice_state)(void *ctx, uintptr_t voice, uint32_t *playing,
                       uint32_t *loop, uint64_t *position);
    /* Device rate of the stream that owns this voice. */
    uint32_t (*voice_device_rate)(void *ctx, uintptr_t voice);
    /* One guest Unlock() that wraps the physical end of the ring.  Copies both
     * locked spans under one publish claim, so the publish>=4 streaming
     * threshold still counts Unlock calls, not ring spans. */
    int (*voice_publish2)(void *ctx, uintptr_t voice,
                          uint64_t frame_base1, const void *frames1,
                          uint32_t bytes1,
                          uint64_t frame_base2, const void *frames2,
                          uint32_t bytes2, uint32_t *written);
    /* Zero-byte Unlock: clear the stale window and, while playing, advance the
     * publish count exactly like PcmVoice::published(). */
    int (*voice_published)(void *ctx, uintptr_t voice);
} Wc3AudioApi;

typedef struct Wc3ClipboardApi {
    void *ctx;
    int (*get_text)(void *ctx, char *dst, uint32_t cap, uint32_t *needed);
    int (*set_text)(void *ctx, const char *utf8);
} Wc3ClipboardApi;

typedef struct Wc3SurfaceApi {
    void *ctx;
    int (*acquire)(void *ctx, uintptr_t *native_window);
    void (*release)(void *ctx, uintptr_t native_window);
    void (*resize)(void *ctx, uint32_t width, uint32_t height);
    void (*lost)(void *ctx);
} Wc3SurfaceApi;

typedef struct Wc3LogApi {
    void *ctx;
    /* Fatal-only release logging is the caller's policy; this API is used by
     * debug builds and by the fatal bootstrap path. */
    void (*write)(void *ctx, int priority, const char *tag, const char *text);
} Wc3LogApi;

typedef struct Wc3ConfigApi {
    void *ctx;
    /* Returns NULL when the key is absent.  The returned pointer is stable for
     * the process lifetime. */
    const char *(*get)(void *ctx, const char *key);
    int (*get_int)(void *ctx, const char *key, int fallback);
} Wc3ConfigApi;

typedef struct Wc3AssetApi {
    void *ctx;
    /* Asset root is read-only app-specific/SAF storage; save root is private
     * writable storage.  Empty/missing required MPQ data is an explicit
     * startup diagnostic, not a silent fallback. */
    int (*init)(void *ctx, const char *asset_root, const char *save_root);
    const char *(*asset_root)(void *ctx);
    const char *(*save_root)(void *ctx);
    int (*enumerate_mpq)(void *ctx, uint32_t index, char *name, size_t cap);
    int (*present)(void *ctx, const char *logical_name);
} Wc3AssetApi;

enum {
    WC3_VULKAN_DRIVER_SYSTEM = 0,
    WC3_VULKAN_DRIVER_TURNIP = 1 /* retired: arbitrary packages use ids */
};

#define WC3_VULKAN_DRIVER_ID_MAX 32

typedef struct Wc3VulkanCaps {
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t extension_count;
} Wc3VulkanCaps;

typedef struct Wc3VulkanDriverApi {
    void *ctx;
    /* Explicit ICD selection (system | turnip), not a hidden DLL substitution.
     * The DXVK selector uses device capabilities, not just loader version.
     * This legacy enum entry remains for the headless contract; the product
     * path is select_id(), because an installed package has no enum value. */
    int (*select)(void *ctx, int driver);
    int (*instance_version)(void *ctx, uint32_t *version);
    int (*device_caps)(void *ctx, uint32_t device_index, Wc3VulkanCaps *caps);

    /* ADR-036, appended in this order and never reordered.
     *
     * select_id(NULL or "") selects the system loader.  It only latches the
     * choice; the ICD is opened by instance_proc_addr() before the first
     * DXVK instance.  instance_proc_addr() returns the selected loader's
     * vkGetInstanceProcAddr, or the system loader's pointer after a
     * package/load/probe fallback (NULL only when no Vulkan loader exists).
     * Both DXVK copies must receive it before Direct3DCreate9.  active_id()
     * reports what actually runs after an ICD-level fallback.  last_error()
     * returns a 4xxx code from vulkan_driver_package.h. */
    int (*select_id)(void *ctx, const char *id);
    void *(*instance_proc_addr)(void *ctx);
    int (*active_id)(void *ctx, char *dst, size_t cap);
    int (*last_error)(void *ctx);
} Wc3VulkanDriverApi;

typedef uintptr_t Wc3ThreadHandle;

/* P0-9: a zero stack_size asks the platform for its host-thread default.
 * Android/headless use 8 MiB for guest-created host threads; the guest stack
 * itself remains the 2 MiB allocation from TA-2.  A non-zero value is honoured
 * when it is at least PTHREAD_STACK_MIN. */
#define WC3_HOST_THREAD_STACK_SIZE (8u * 1024u * 1024u)

typedef struct Wc3ThreadDesc {
    const char *name;
    uint32_t stack_size;
    int (*entry)(void *arg);
    void *arg;
} Wc3ThreadDesc;

typedef struct Wc3ThreadApi {
    void *ctx;
    int (*create)(void *ctx, const Wc3ThreadDesc *desc, Wc3ThreadHandle *out);
    int (*join)(void *ctx, Wc3ThreadHandle thread, uint32_t *exit_code);
    int (*detach)(void *ctx, Wc3ThreadHandle thread);
    Wc3ThreadHandle (*self)(void *ctx);
    uint64_t (*id)(void *ctx, Wc3ThreadHandle thread);
    void (*yield)(void *ctx);
    int (*sleep_ns)(void *ctx, uint64_t ns);
    /* Guest TLS indices are allocated by the runtime, not by the host TLS
     * implementation; these calls are only the per-thread storage hook. */
    int (*tls_alloc)(void *ctx, uint32_t *index);
    int (*tls_free)(void *ctx, uint32_t index);
    int (*tls_set)(void *ctx, uint32_t index, uintptr_t value);
    uintptr_t (*tls_get)(void *ctx, uint32_t index);
} Wc3ThreadApi;

typedef uintptr_t Wc3SyncHandle;

enum {
    WC3_WAIT_OBJECT_0 = 0,
    WC3_WAIT_ABANDONED_0 = 0x80,
    WC3_WAIT_TIMEOUT = 258,
    WC3_WAIT_FAILED = 0xFFFFFFFFu
};

typedef struct Wc3SyncApi {
    void *ctx;
    int (*event_create)(void *ctx, int manual_reset, int initial_state,
                        Wc3SyncHandle *out);
    int (*event_set)(void *ctx, Wc3SyncHandle event);
    int (*event_reset)(void *ctx, Wc3SyncHandle event);
    int (*mutex_create)(void *ctx, int initial_owner, Wc3SyncHandle *out);
    int (*mutex_release)(void *ctx, Wc3SyncHandle mutex);
    int (*semaphore_create)(void *ctx, int32_t initial, int32_t maximum,
                            Wc3SyncHandle *out);
    int (*semaphore_release)(void *ctx, Wc3SyncHandle semaphore, int32_t count,
                             int32_t *previous);
    /* timeout_ms == UINT32_MAX means INFINITE.  Returns WAIT_OBJECT_0 + index,
     * WAIT_TIMEOUT or WAIT_FAILED; wait_all is the Windows semantics. */
    int (*wait)(void *ctx, const Wc3SyncHandle *objects, uint32_t count,
                int wait_all, uint32_t timeout_ms, uint32_t *index);
    int (*close)(void *ctx, Wc3SyncHandle handle);
    int (*critical_section_create)(void *ctx, Wc3SyncHandle *out);
    int (*critical_section_enter)(void *ctx, Wc3SyncHandle cs);
    int (*critical_section_try_enter)(void *ctx, Wc3SyncHandle cs);
    int (*critical_section_leave)(void *ctx, Wc3SyncHandle cs);
} Wc3SyncApi;

typedef struct Wc3Platform {
    Wc3MemApi mem;
    Wc3FileApi file;
    Wc3TimeApi time;
    Wc3LocaleApi locale;
    Wc3WindowApi window;
    Wc3InputApi input;
    Wc3AudioApi audio;
    Wc3ClipboardApi clipboard;
    Wc3SurfaceApi surface;
    Wc3LogApi log;
    Wc3ConfigApi config;
    Wc3AssetApi asset;
    Wc3NetApi net;
    Wc3ThreadApi thread;
    Wc3SyncApi sync;
    Wc3VulkanDriverApi vulkan_driver;
} Wc3Platform;

/* The Android target installs one immutable table before guest startup. */
const Wc3Platform *wc3_platform(void);
int wc3_platform_install(const Wc3Platform *platform);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PLATFORM_H */
