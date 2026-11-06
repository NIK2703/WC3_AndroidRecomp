/*
 * platform.h - the only OS boundary for the Android target.
 *
 * HLE modules use these function tables; they do not include Android, POSIX or
 * Windows headers.  The real implementation lives under
 * runtime/platform/android, the fake implementation under runtime/platform/fake.
 *
 * There is deliberately no NetApi: LAN/Battle.net are outside the 1.27 Android
 * target.
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

typedef struct Wc3FileApi {
    void *ctx;
    /* Paths here are host-visible paths after the HLE path mapper has run. */
    int (*open)(void *ctx, const char *path, int flags, uintptr_t *out);
    int (*read)(void *ctx, uintptr_t file, void *buf, uint32_t len, uint32_t *read);
    int (*write)(void *ctx, uintptr_t file, const void *buf, uint32_t len, uint32_t *written);
    int (*seek)(void *ctx, uintptr_t file, int64_t offset, int whence, uint64_t *pos);
    int (*size)(void *ctx, uintptr_t file, uint64_t *size);
    int (*close)(void *ctx, uintptr_t file);
} Wc3FileApi;

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
    int (*open)(void *ctx, uint32_t sample_rate, uint32_t channels,
                uint32_t bits, uintptr_t *out);
    int (*write)(void *ctx, uintptr_t stream, const void *frames,
                 uint32_t bytes, uint32_t *written);
    int (*close)(void *ctx, uintptr_t stream);
    /* Called by the platform when the device needs more frames.  The callback
     * runs on the existing audio thread and must not allocate or lock. */
    int (*set_callback)(void *ctx, uintptr_t stream,
                        void (*callback)(void *opaque, void *frames, uint32_t bytes),
                        void *opaque);
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
    WC3_VULKAN_DRIVER_TURNIP = 1
};

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
     * The DXVK selector uses device capabilities, not just loader version. */
    int (*select)(void *ctx, int driver);
    int (*instance_version)(void *ctx, uint32_t *version);
    int (*device_caps)(void *ctx, uint32_t device_index, Wc3VulkanCaps *caps);
} Wc3VulkanDriverApi;

typedef uintptr_t Wc3ThreadHandle;

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
