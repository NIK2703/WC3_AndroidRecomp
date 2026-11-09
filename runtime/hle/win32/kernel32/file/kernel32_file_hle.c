/*
 * kernel32_file_hle.c - target-side path-case cache and archive read-ahead.
 *
 * The as-built h_create_file()/h_get_file_attributes() call ci_resolve() on
 * every request.  ci_resolve() first asks access(F_OK); only a case mismatch
 * falls back to opendir() + readdir() + strcasecmp().  The data directory is
 * effectively immutable during a session, so this module remembers the exact
 * leaf spelling and chains the original handler with a resolved path.  The
 * as-built GH handle table and all read/write/overlapped behaviour stay in
 * quarantine.
 *
 * Exactness rules:
 *   - only the leaf is case-folded, matching ci_resolve();
 *   - an exact-spelling hit is passed through to the as-built live access;
 *   - a cached case-variant spelling is revalidated with access(host) and
 *     access(exact) before it is used;
 *   - a missing file is not positively cached;
 *   - a directory change flushes the cache;
 *   - CreateFileA with a create/truncate/write disposition invalidates the
 *     entry, because the requested spelling may now be the real one.
 *
 * O-27: the first read-only open of an archive also asks the kernel for
 * sequential read-ahead.  This is advisory only; it does not change file
 * offsets, handles or guest-visible data.
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "kernel32_file_hle.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cpu.h"
#include "file_hle.h"
#include "legacy_shim.h"
#include "wc3_debug.h"

#define FILE_PATH_MAX 512u
#define FILE_CACHE_ENTRIES 128u
#define FILE_PREFETCH_ENTRIES 32u
#define INVALID_HANDLE 0xFFFFFFFFu

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

typedef struct FileCacheEntry {
    uint8_t kind; /* 0 = empty, 1 = exact path seen, 2 = resolved spelling */
    char key[FILE_PATH_MAX];
    char exact[FILE_PATH_MAX];
} FileCacheEntry;

typedef struct PrefetchEntry {
    uint8_t used;
    char path[FILE_PATH_MAX];
} PrefetchEntry;

static FileCacheEntry g_cache[FILE_CACHE_ENTRIES];
static unsigned g_cache_next;
static PrefetchEntry g_prefetch[FILE_PREFETCH_ENTRIES];
static unsigned g_prefetch_next;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_previous_create_file)(CPU *c);
static void (*g_previous_get_attributes)(CPU *c);
static void (*g_previous_set_cwd)(CPU *c);

#if WC3_DEBUG && WC3_DEBUG_HLE
static unsigned g_cache_debug_logs;
#define CACHE_LOG(...) do { \
        if (g_cache_debug_logs < 32u) { \
            g_cache_debug_logs++; \
            fprintf(stderr, "[file] " __VA_ARGS__); \
        } \
    } while (0)
#else
#define CACHE_LOG(...) ((void)0)
#endif

static int path_is_foreign_drive(const char *guest)
{
    /* The target mapper keeps C: paths relative while the as-built win_path()
     * keeps a leading slash for a rooted C: path.  Do not cache those: the
     * fallback must continue to use the as-built spelling. */
    return guest && isalpha((unsigned char)guest[0]) && guest[1] == ':' &&
           guest[0] != 'Z' && guest[0] != 'z';
}

/* Lower-case only the leaf.  ci_resolve() does not recurse into directory
 * components, so neither may the cache. */
static void normalize_leaf(char *path)
{
    char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    for (; *leaf; leaf++) {
        if (*leaf >= 'A' && *leaf <= 'Z') *leaf = (char)(*leaf + ('a' - 'A'));
    }
}

static int make_key(const char *host, char *key, size_t capacity)
{
    size_t n;

    if (!host || !key || capacity < 2u) return 0;
    n = strlen(host);
    if (n >= capacity) return 0;
    memcpy(key, host, n + 1u);
    normalize_leaf(key);
    return 1;
}

static int cache_lookup(const char *key, char *exact, size_t capacity)
{
    int kind = 0;

    pthread_mutex_lock(&g_cache_lock);
    for (unsigned i = 0; i < FILE_CACHE_ENTRIES; i++) {
        if (!g_cache[i].kind || strcmp(g_cache[i].key, key) != 0) continue;
        kind = g_cache[i].kind;
        if (kind == 2)
            snprintf(exact, capacity, "%s", g_cache[i].exact);
        break;
    }
    pthread_mutex_unlock(&g_cache_lock);
    return kind;
}

static void cache_store(const char *key, const char *exact)
{
    FileCacheEntry *entry = NULL;

    if (!key || strlen(key) >= FILE_PATH_MAX) return;
    pthread_mutex_lock(&g_cache_lock);
    for (unsigned i = 0; i < FILE_CACHE_ENTRIES; i++) {
        if (g_cache[i].kind && strcmp(g_cache[i].key, key) == 0) {
            entry = &g_cache[i];
            break;
        }
    }
    if (!entry) {
        entry = &g_cache[g_cache_next];
        g_cache_next = (g_cache_next + 1u) % FILE_CACHE_ENTRIES;
        memset(entry, 0, sizeof *entry);
        snprintf(entry->key, sizeof entry->key, "%s", key);
    }
    if (exact && strlen(exact) < FILE_PATH_MAX) {
        entry->kind = 2;
        snprintf(entry->exact, sizeof entry->exact, "%s", exact);
    } else {
        entry->kind = 1;
        entry->exact[0] = '\0';
    }
    pthread_mutex_unlock(&g_cache_lock);
}

static void cache_remove(const char *key)
{
    if (!key) return;
    pthread_mutex_lock(&g_cache_lock);
    for (unsigned i = 0; i < FILE_CACHE_ENTRIES; i++) {
        if (g_cache[i].kind && strcmp(g_cache[i].key, key) == 0) {
            memset(&g_cache[i], 0, sizeof g_cache[i]);
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_lock);
}

static void cache_clear_all(void)
{
    pthread_mutex_lock(&g_cache_lock);
    memset(g_cache, 0, sizeof g_cache);
    memset(g_prefetch, 0, sizeof g_prefetch);
    g_cache_next = 0;
    g_prefetch_next = 0;
    pthread_mutex_unlock(&g_cache_lock);
}

/* Find the real spelling of only the last path component.  The directory
 * prefix is copied verbatim, as in the as-built ci_resolve(). */
static int scan_leaf(const char *host, char *exact, size_t capacity)
{
    const char *slash = strrchr(host, '/');
    const char *leaf = slash ? slash + 1 : host;
    char dir[FILE_PATH_MAX];
    DIR *d;
    struct dirent *entry;
    int found = 0;

    if (!leaf[0] || !strcmp(leaf, ".") || !strcmp(leaf, "..")) return 0;
    if (slash) {
        size_t n = (size_t)(slash - host);
        if (n >= sizeof dir) return 0;
        memcpy(dir, host, n);
        dir[n] = '\0';
    } else {
        snprintf(dir, sizeof dir, ".");
    }

    d = opendir(dir);
    if (!d) return 0;
    while ((entry = readdir(d)) != NULL) {
        if (!strcasecmp(entry->d_name, leaf)) {
            int written;
            if (slash) {
                written = snprintf(exact, capacity, "%.*s%s",
                                   (int)(slash - host) + 1, host,
                                   entry->d_name);
            } else {
                written = snprintf(exact, capacity, "%s", entry->d_name);
            }
            found = written >= 0 && (size_t)written < capacity;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Returns 1 when `exact` differs from the requested spelling and should be
 * installed as the guest path; 0 means "call the as-built handler unchanged". */
static int resolve_path(const char *host, const char *key,
                        char *exact, size_t capacity)
{
    int kind = cache_lookup(key, exact, capacity);

    if (kind == 1) return 0;
    if (kind == 2) {
        /* The as-built ci_resolve() prefers the exact requested spelling
         * before it scans the directory.  Keep that precedence even when a
         * case-only variant was cached earlier: a file created with the
         * requested spelling must not be shadowed by the cached one. */
        if (access(host, F_OK) == 0) {
            cache_remove(key);
            return 0;
        }
        if (access(exact, F_OK) == 0) return 1;
        cache_remove(key);
    }

    if (access(host, F_OK) == 0) {
        cache_store(key, NULL);
        return 0;
    }
    if (scan_leaf(host, exact, capacity)) {
        if (strcmp(exact, host) == 0) {
            /* access() said no but readdir() found the same spelling.  Do not
             * turn this into a cache-hit loop; let the as-built handler decide. */
            return 0;
        }
        cache_store(key, exact);
        CACHE_LOG("resolve \"%s\" -> \"%s\"\n", host, exact);
        return 1;
    }
    return 0;
}

static int archive_path(const char *path)
{
    const char *leaf;
    const char *dot;

    if (!path || !path[0]) return 0;
    leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    dot = strrchr(leaf, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".mpq") || !strcasecmp(dot, ".w3m") ||
           !strcasecmp(dot, ".w3x") || !strcasecmp(dot, ".w3n");
}

static int prefetch_seen(const char *path)
{
    int seen = 0;

    pthread_mutex_lock(&g_cache_lock);
    for (unsigned i = 0; i < FILE_PREFETCH_ENTRIES; i++) {
        if (g_prefetch[i].used && strcmp(g_prefetch[i].path, path) == 0) {
            seen = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_lock);
    return seen;
}

static void prefetch_mark(const char *path)
{
    PrefetchEntry *entry;

    if (strlen(path) >= FILE_PATH_MAX) return;
    pthread_mutex_lock(&g_cache_lock);
    for (unsigned i = 0; i < FILE_PREFETCH_ENTRIES; i++) {
        if (g_prefetch[i].used && strcmp(g_prefetch[i].path, path) == 0) {
            pthread_mutex_unlock(&g_cache_lock);
            return;
        }
    }
    entry = &g_prefetch[g_prefetch_next];
    g_prefetch_next = (g_prefetch_next + 1u) % FILE_PREFETCH_ENTRIES;
    snprintf(entry->path, sizeof entry->path, "%s", path);
    entry->used = 1;
    pthread_mutex_unlock(&g_cache_lock);
}

static void prefetch_archive(const char *path)
{
    if (!archive_path(path) || prefetch_seen(path)) return;

#if defined(__ANDROID__) || defined(__linux__)
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    close(fd);
    CACHE_LOG("fadvise sequential \"%s\"\n", path);
#else
    (void)path;
#endif
    prefetch_mark(path);
}

static void guest_path_from_host(const char *host, char *out, size_t capacity)
{
    size_t n = 0;

    if (!out || capacity < 2u) return;
    if (host && host[0] == '/') {
        if (n + 2u < capacity) {
            out[n++] = 'Z';
            out[n++] = ':';
        }
    }
    if (host) {
        for (; *host && n + 1u < capacity; host++)
            out[n++] = (*host == '/') ? '\\' : *host;
    }
    out[n] = '\0';
}

/* Install an exact spelling in the guest stack slot for arg0.  The as-built
 * handler only reads the string during this call, so the temporary block is
 * released immediately afterwards.  The original guest string is untouched. */
static uint32_t install_temp_path(CPU *c, const char *host)
{
    char guest_path[FILE_PATH_MAX + 16u];
    uint32_t address;
    size_t length;

    guest_path_from_host(host, guest_path, sizeof guest_path);
    length = strlen(guest_path);
    if (!length || length >= FILE_PATH_MAX) return 0;

    address = wc3_legacy_guest_alloc((uint32_t)length + 1u);
    if (!address) return 0;
    if (!wc3_legacy_guest_ok(address, (uint32_t)length + 1u)) {
        wc3_legacy_guest_free(address);
        return 0;
    }
    for (size_t i = 0; i < length; i++)
        wc3_legacy_wr8g(address + (uint32_t)i, (uint8_t)guest_path[i]);
    wc3_legacy_wr8g(address + (uint32_t)length, 0);

    const uint32_t slot = c->esp + 4u;
    if (slot < c->esp || !wc3_legacy_guest_ok(slot, 4u)) {
        wc3_legacy_guest_free(address);
        return 0;
    }
    wc3_legacy_wr32g(slot, address);
    return address;
}

static void chained_create_file(CPU *c)
{
    void (*previous)(CPU *) = g_previous_create_file;
    uint32_t name;
    uint32_t access;
    uint32_t disposition;
    char guest[FILE_PATH_MAX];
    char host[FILE_PATH_MAX];
    char key[FILE_PATH_MAX];
    char exact[FILE_PATH_MAX];
    int cacheable = 0;
    int mutating = 0;
    int rewrite = 0;
    uint32_t temporary = 0;

    if (!previous) {
        c->eax = INVALID_HANDLE;
        return;
    }

    name = wc3_legacy_arg32(c, 0);
    access = wc3_legacy_arg32(c, 1);
    disposition = wc3_legacy_arg32(c, 4);
    mutating = disposition != 3u || (access & 0x40000000u) != 0;

    if (wc3_hle_msvcr80_read_guest_path(name, guest, sizeof guest) &&
        !path_is_foreign_drive(guest)) {
        wc3_hle_msvcr80_map_path(guest, host, sizeof host);
        if (make_key(host, key, sizeof key)) {
            cacheable = 1;
            if (mutating) cache_remove(key);
            rewrite = resolve_path(host, key, exact, sizeof exact);
        }
    }

    if (rewrite) temporary = install_temp_path(c, exact);
    previous(c);
    if (temporary) wc3_legacy_guest_free(temporary);

    if (cacheable) {
        if (mutating) {
            cache_remove(key);
        } else if (c->eax != INVALID_HANDLE) {
            const char *opened = rewrite ? exact : host;
            prefetch_archive(opened);
        }
    }
}

static void chained_get_attributes(CPU *c)
{
    void (*previous)(CPU *) = g_previous_get_attributes;
    uint32_t name;
    char guest[FILE_PATH_MAX];
    char host[FILE_PATH_MAX];
    char key[FILE_PATH_MAX];
    char exact[FILE_PATH_MAX];
    int rewrite = 0;
    uint32_t temporary = 0;

    if (!previous) {
        c->eax = INVALID_HANDLE;
        return;
    }

    name = wc3_legacy_arg32(c, 0);
    if (wc3_hle_msvcr80_read_guest_path(name, guest, sizeof guest) &&
        !path_is_foreign_drive(guest)) {
        wc3_hle_msvcr80_map_path(guest, host, sizeof host);
        if (make_key(host, key, sizeof key))
            rewrite = resolve_path(host, key, exact, sizeof exact);
    }

    if (rewrite) temporary = install_temp_path(c, exact);
    previous(c);
    if (temporary) wc3_legacy_guest_free(temporary);
}

static void chained_set_current_directory(CPU *c)
{
    void (*previous)(CPU *) = g_previous_set_cwd;

    if (!previous) {
        c->eax = 0;
        return;
    }
    previous(c);
    if (c->eax) cache_clear_all();
}

int wc3_hle_kernel32_file_install(void)
{
    if (wc3_legacy_hle_chain("KERNEL32.dll", "CreateFileA",
                             chained_create_file,
                             &g_previous_create_file) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "GetFileAttributesA",
                             chained_get_attributes,
                             &g_previous_get_attributes) != 0)
        return -1;
    if (wc3_legacy_hle_chain("KERNEL32.dll", "SetCurrentDirectoryA",
                             chained_set_current_directory,
                             &g_previous_set_cwd) != 0)
        return -1;
    return 3;
}
