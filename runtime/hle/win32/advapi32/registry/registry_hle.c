/*
 * registry_hle.c - target-side ADVAPI32 registry store.
 *
 * The as-built registry only implements open/query/set/create/close and keeps
 * a flat value list keyed by a "current key" string.  This target slice owns
 * the entire registry surface so the imported delete, flush, enumeration and
 * information queries operate on the same data.  Handles are real per-open
 * keys now, while the old current-key fallback is retained for calls that
 * pass a predefined HKEY.
 *
 * The store is in memory, matching the as-built Android behaviour.  Defaults
 * for Sound and Video are seeded at install time, after the display mode has
 * been selected by app_boot.
 */
#include "registry_hle.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "hle_bridge.h"
#include "wc3_debug.h"

#define REG_ROOT "Software\\Blizzard Entertainment\\Warcraft III"
#define REG_SOUND REG_ROOT "\\Sound"
#define REG_VIDEO REG_ROOT "\\Video"

#define REG_PATH_MAX 160u
#define REG_NAME_MAX 64u
#define REG_DATA_MAX 1024u
#define REG_KEY_MAX 96u
#define REG_VALUE_MAX 128u
#define REG_OPEN_MAX 32u

/* The registry has no Windows hive behind it on Android.  Keep one text file
 * beside the other per-process state so a value the game writes survives the
 * next process launch.  WC3_DATA_DIR is the app-private state directory on
 * the GameActivity path (app_boot.c sets it before the guest starts). */
#define REG_FILE_NAME "wc3_registry.dat"
#define REG_FILE_PATH_MAX 1200u
#define REG_FILE_LINE_MAX 4096u

#define REG_HANDLE_BASE 0x00150000u
#define REG_HANDLE_STRIDE 4u

#define HKEY_CURRENT_USER 0x80000001u
#define HKEY_LOCAL_MACHINE 0x80000002u

#define REG_SUCCESS 0u
#define REG_FILE_NOT_FOUND 2u
#define REG_INVALID_HANDLE 6u
#define REG_NOT_ENOUGH_MEMORY 8u
#define REG_INVALID_PARAMETER 87u
#define REG_MORE_DATA 234u
#define REG_NO_MORE_ITEMS 259u

#define REG_SZ 1u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u

typedef struct RegKey {
    uint8_t used;
    char path[REG_PATH_MAX];
} RegKey;

typedef struct RegValue {
    uint8_t used;
    char path[REG_PATH_MAX];
    char name[REG_NAME_MAX];
    uint32_t type;
    uint32_t len;
    uint8_t data[REG_DATA_MAX];
} RegValue;

typedef struct RegOpenKey {
    uint8_t used;
    uint32_t handle;
    char path[REG_PATH_MAX];
} RegOpenKey;

static RegKey g_keys[REG_KEY_MAX];
static RegValue g_values[REG_VALUE_MAX];
static RegOpenKey g_open[REG_OPEN_MAX];
static char g_current_key[REG_PATH_MAX] = REG_ROOT;
/* g_lock owns keys, values, g_current_key, dirty state and persistence.
 * g_open_table_lock owns g_open[i].used/.handle and first-free allocation;
 * g_open_locks[i] owns g_open[i].path.  Lock order is
 * g_lock -> g_open_table_lock -> g_open_locks[i]; slot locks are leaves. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_open_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_open_locks[REG_OPEN_MAX];
static pthread_once_t g_open_locks_once = PTHREAD_ONCE_INIT;
static int g_open_locks_ready;

static void init_open_locks(void)
{
    int ready = 1;
    for (unsigned i = 0; i < REG_OPEN_MAX; i++) {
        if (pthread_mutex_init(&g_open_locks[i], NULL) != 0)
            ready = 0;
    }
    g_open_locks_ready = ready;
}

/* A save-all from the game writes every preference in one burst.  Persist
 * once at the end of that burst instead of rewriting the whole file for each
 * value, and skip the write entirely when no value actually changed. */
static int g_store_batch;
static int g_store_dirty;

static char ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int ci_equal(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ci_prefix(const char *text, const char *prefix)
{
    if (!text || !prefix) return 0;
    while (*prefix) {
        if (!*text || ascii_lower(*text) != ascii_lower(*prefix)) return 0;
        text++;
        prefix++;
    }
    return 1;
}

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
/* A Battle.net gateway value is an ANSI REG_MULTI_SZ:
 * "version\0default-index\0host\0zone\0label\0...".  The raw registry trace
 * only prints the first dword, which is the version string and hides the
 * default index.  Decode the list after the lock is released so the log never
 * runs under g_lock. */
static int battle_net_text_precision(const char *text)
{
    size_t n = text ? strlen(text) : 0u;
    return n > 96u ? 96 : (int)n;
}

static void battle_net_trace_gateways(const char *operation, const char *path,
                                      const char *name, const uint8_t *data,
                                      uint32_t type, uint32_t length,
                                      uint32_t capacity, uint32_t status)
{
    if (!ci_equal(name, "Battle.net Gateways")) return;

    fprintf(stderr,
            "[bnet] registry %s %s\\%s type=%u len=%u cap=%u status=%u\n",
            operation, path, name, (unsigned)type, (unsigned)length,
            (unsigned)capacity, (unsigned)status);
    if (type != REG_MULTI_SZ || !data || length < 2u) return;

    const char *items[32];
    unsigned count = 0;
    const uint8_t *cursor = data;
    uint32_t remaining = length;
    while (remaining && count < 32u) {
        const uint8_t *nul = (const uint8_t *)memchr(cursor, 0, remaining);
        if (!nul || nul == cursor) break;
        items[count++] = (const char *)cursor;
        remaining -= (uint32_t)(nul + 1 - cursor);
        cursor = nul + 1;
    }
    if (count < 2u) return;

    const unsigned triples = (count - 2u) / 3u;
    fprintf(stderr,
            "[bnet] gateways version=\"%.*s\" default=\"%.*s\" (%u triples)\n",
            battle_net_text_precision(items[0]), items[0],
            battle_net_text_precision(items[1]), items[1], triples);

    char *end = NULL;
    const unsigned long selected = strtoul(items[1], &end, 10);
    if (end != items[1] && end && *end == '\0' && selected < triples) {
        const char *host = items[2u + (unsigned)selected * 3u];
        fprintf(stderr, "[bnet] gateway selected[%lu] host=\"%.*s\"\n",
                selected, battle_net_text_precision(host), host);
    }

    for (unsigned i = 0; i < triples; i++) {
        const char *host = items[2u + i * 3u];
        const char *zone = items[3u + i * 3u];
        const char *label = items[4u + i * 3u];
        fprintf(stderr,
                "[bnet] gateway[%u] host=\"%.*s\" zone=\"%.*s\" name=\"%.*s\"\n",
                i, battle_net_text_precision(host), host,
                battle_net_text_precision(zone), zone,
                battle_net_text_precision(label), label);
    }
}
#endif

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (!dst || capacity == 0u) return;
    if (!src) src = "";
    size_t length = strlen(src);
    if (length >= capacity) length = capacity - 1u;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void append_text(char *dst, size_t capacity, const char *src)
{
    if (!dst || capacity == 0u || !src) return;
    const size_t used = strlen(dst);
    if (used + 1u >= capacity) return;
    copy_text(dst + used, capacity - used, src);
}

static void normalize_path(const char *input, char *output, size_t capacity)
{
    if (!output || capacity == 0u) return;
    output[0] = '\0';
    if (!input) return;
    while (*input == '\\') input++;
    copy_text(output, capacity, input);
    size_t length = strlen(output);
    while (length > 0u && output[length - 1u] == '\\') {
        output[--length] = '\0';
    }
}

static int is_absolute_key(const char *path)
{
    return path && (ci_equal(path, "Software") ||
                    ci_prefix(path, "Software\\"));
}

static void resolve_subkey(const char *parent, const char *sub,
                           char *output, size_t capacity)
{
    char normalized[REG_PATH_MAX];
    normalize_path(sub, normalized, sizeof normalized);
    if (is_absolute_key(normalized) || normalized[0] == '\0' ||
        !parent || !parent[0]) {
        copy_text(output, capacity, normalized);
        return;
    }
    copy_text(output, capacity, parent);
    append_text(output, capacity, "\\");
    append_text(output, capacity, normalized);
}

static int is_ours(const char *path)
{
    return path && ci_prefix(path, REG_ROOT);
}

static int read_guest_string(uint32_t address, char *buffer, size_t capacity)
{
    if (!buffer || capacity < 2u) return 0;
    if (!address) {
        buffer[0] = '\0';
        return 1;
    }
    for (size_t i = 0; i + 1u < capacity; i++) {
        if (address > UINT32_MAX - (uint32_t)i) return 0;
        const uint32_t at = address + (uint32_t)i;
        if (!wc3_legacy_guest_ok(at, 1u)) return 0;
        const char ch = (char)wc3_legacy_rd8g(at);
        buffer[i] = ch;
        if (!ch) return 1;
    }
    return 0;
}

static void write_guest_string(uint32_t address, const char *text,
                               uint32_t capacity)
{
    if (!text) text = "";
    const size_t length = strlen(text);
    for (uint32_t i = 0; i < length && i + 1u < capacity; i++) {
        if (!wc3_legacy_guest_ok(address + i, 1u)) return;
        wc3_legacy_wr8g(address + i, (uint8_t)text[i]);
    }
    if (capacity > 0u) {
        const uint32_t at = address + (uint32_t)(length < capacity - 1u
                                                 ? length : capacity - 1u);
        if (wc3_legacy_guest_ok(at, 1u)) wc3_legacy_wr8g(at, 0u);
    }
}

static void write_u32(uint32_t address, uint32_t value)
{
    if (address && wc3_legacy_guest_ok(address, 4u))
        wc3_legacy_wr32g(address, value);
}

static RegKey *find_key_locked(const char *path)
{
    for (unsigned i = 0; i < REG_KEY_MAX; i++) {
        if (g_keys[i].used && ci_equal(g_keys[i].path, path))
            return &g_keys[i];
    }
    return NULL;
}

static RegKey *add_key_locked(const char *path)
{
    RegKey *key = find_key_locked(path);
    if (key) return key;
    for (unsigned i = 0; i < REG_KEY_MAX; i++) {
        if (g_keys[i].used) continue;
        g_keys[i].used = 1u;
        copy_text(g_keys[i].path, sizeof g_keys[i].path, path);
        return &g_keys[i];
    }
    return NULL;
}

static void ensure_key_and_parents_locked(const char *path)
{
    char current[REG_PATH_MAX];
    normalize_path(path, current, sizeof current);
    if (!current[0]) return;

    for (;;) {
        add_key_locked(current);
        char *slash = strrchr(current, '\\');
        if (!slash) break;
        *slash = '\0';
        if (!current[0]) break;
    }
}

static RegValue *find_value_locked(const char *path, const char *name)
{
    for (unsigned i = 0; i < REG_VALUE_MAX; i++) {
        if (!g_values[i].used) continue;
        if (ci_equal(g_values[i].path, path) &&
            ci_equal(g_values[i].name, name))
            return &g_values[i];
    }
    return NULL;
}

static RegValue *slot_value_locked(const char *path, const char *name)
{
    RegValue *value = find_value_locked(path, name);
    if (value) return value;
    for (unsigned i = 0; i < REG_VALUE_MAX; i++) {
        if (g_values[i].used) continue;
        g_values[i].used = 1u;
        copy_text(g_values[i].path, sizeof g_values[i].path, path);
        copy_text(g_values[i].name, sizeof g_values[i].name, name);
        g_values[i].type = REG_SZ;
        g_values[i].len = 0u;
        return &g_values[i];
    }
    return NULL;
}

static void set_value_locked(const char *path, const char *name,
                             uint32_t type, const uint8_t *data,
                             uint32_t length)
{
    RegValue *value = slot_value_locked(path, name);
    if (!value) return;
    value->type = type;
    value->len = length > REG_DATA_MAX ? REG_DATA_MAX : length;
    if (data && value->len)
        memcpy(value->data, data, value->len);
}

static int reg_file_path(char *path, size_t capacity)
{
    const char *dir = getenv("WC3_DATA_DIR");
    if (!dir || !*dir) return 0;
    const int n = snprintf(path, capacity, "%s/%s", dir, REG_FILE_NAME);
    return n > 0 && (size_t)n < capacity;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Write the store through a temporary file and rename it into place.  Android
 * renames atomically, so a process killed during a settings write leaves the
 * previous file intact. */
static void persist_registry_locked(void)
{
    char path[REG_FILE_PATH_MAX];
    char temp[REG_FILE_PATH_MAX + 5u];
    if (!reg_file_path(path, sizeof path)) return;
    const int n = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof temp) return;

    FILE *file = fopen(temp, "wb");
    if (!file) return;

    int ok = fputs("WC3REG1\n", file) >= 0;
    for (unsigned i = 0; ok && i < REG_VALUE_MAX; i++) {
        const RegValue *value = &g_values[i];
        if (!value->used || !is_ours(value->path)) continue;
        if (fprintf(file, "%s\t%s\t%u\t%u\t", value->path, value->name,
                    value->type, value->len) < 0) {
            ok = 0;
            break;
        }
        for (uint32_t j = 0; j < value->len; j++) {
            if (fprintf(file, "%02x", value->data[j]) < 0) {
                ok = 0;
                break;
            }
        }
        if (ok && fputc('\n', file) == EOF) ok = 0;
    }
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        remove(temp);
        return;
    }
#if defined(_WIN32)
    /* MinGW's rename does not replace an existing destination. */
    remove(path);
#endif
    if (rename(temp, path) != 0) remove(temp);
}

static void maybe_persist_locked(void)
{
    if (g_store_batch || !g_store_dirty) return;
    g_store_dirty = 0;
    persist_registry_locked();
}

/* The caller brackets a group of registry writes that belong to one logical
 * save.  The store is still updated immediately; only the file write is
 * deferred until the group ends.  Both calls are safe from the guest thread
 * and from the host side. */
void wc3_hle_registry_begin_batch(void)
{
    pthread_mutex_lock(&g_lock);
    g_store_batch++;
    pthread_mutex_unlock(&g_lock);
}

void wc3_hle_registry_end_batch(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_store_batch > 0) g_store_batch--;
    maybe_persist_locked();
    pthread_mutex_unlock(&g_lock);
}

/* Restore the values written by an earlier process.  Called after the stock
 * defaults, so a saved resolution wins over the native-size default.  Returns
 * the restored count, or -1 when no valid persistence file was read. */
static int load_persisted_locked(void)
{
    char path[REG_FILE_PATH_MAX];
    if (!reg_file_path(path, sizeof path)) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;

    char line[REG_FILE_LINE_MAX];
    if (!fgets(line, sizeof line, file) || strcmp(line, "WC3REG1\n") != 0) {
        fclose(file);
        return -1;
    }

    unsigned loaded = 0u;
    while (fgets(line, sizeof line, file)) {
        size_t length = strlen(line);
        while (length > 0u &&
               (line[length - 1u] == '\n' || line[length - 1u] == '\r'))
            line[--length] = '\0';
        if (!line[0]) continue;

        char *key = line;
        char *name = strchr(key, '\t');
        if (!name) continue;
        *name++ = '\0';
        char *type_text = strchr(name, '\t');
        if (!type_text) continue;
        *type_text++ = '\0';
        char *length_text = strchr(type_text, '\t');
        if (!length_text) continue;
        *length_text++ = '\0';
        char *hex = strchr(length_text, '\t');
        if (!hex) continue;
        *hex++ = '\0';

        char *end = NULL;
        const unsigned long type = strtoul(type_text, &end, 10);
        if (!end || *end) continue;
        end = NULL;
        const unsigned long data_length = strtoul(length_text, &end, 10);
        if (!end || *end || data_length > REG_DATA_MAX) continue;
        if (strlen(hex) < (size_t)data_length * 2u) continue;

        char normalized[REG_PATH_MAX];
        normalize_path(key, normalized, sizeof normalized);
        if (!is_ours(normalized) || strlen(name) >= REG_NAME_MAX) continue;

        uint8_t data[REG_DATA_MAX];
        int valid = 1;
        for (unsigned long i = 0; i < data_length; i++) {
            const int high = hex_nibble(hex[i * 2u]);
            const int low = hex_nibble(hex[i * 2u + 1u]);
            if (high < 0 || low < 0) {
                valid = 0;
                break;
            }
            data[i] = (uint8_t)((high << 4) | low);
        }
        if (!valid) continue;

        ensure_key_and_parents_locked(normalized);
        set_value_locked(normalized, name, (uint32_t)type, data,
                         (uint32_t)data_length);
        loaded++;
    }
    fclose(file);
    return (int)loaded;
}

static int remove_value_locked(const char *path, const char *name)
{
    RegValue *value = find_value_locked(path, name);
    if (!value) return 0;
    /* used is the only structural bit: readers gate on it and never inspect
     * a retired slot's payload, which is fully overwritten on reuse. */
    value->used = 0u;
    return 1;
}

static int is_descendant(const char *path, const char *parent)
{
    const size_t length = strlen(parent);
    return ci_prefix(path, parent) && path[length] == '\\' &&
           path[length + 1u] != '\0';
}

static int delete_subtree_locked(const char *path)
{
    int removed = 0;

    for (unsigned i = 0; i < REG_VALUE_MAX; i++) {
        if (!g_values[i].used) continue;
        if (ci_equal(g_values[i].path, path) ||
            is_descendant(g_values[i].path, path)) {
            g_values[i].used = 0u;
            removed = 1;
        }
    }
    for (unsigned i = 0; i < REG_KEY_MAX; i++) {
        if (!g_keys[i].used) continue;
        if (ci_equal(g_keys[i].path, path) ||
            is_descendant(g_keys[i].path, path)) {
            g_keys[i].used = 0u;
            removed = 1;
        }
    }
    /* The table lock prevents slot reuse across this scan; the matching slot
     * lock protects the path during comparison and clearing. */
    pthread_mutex_lock(&g_open_table_lock);
    for (unsigned i = 0; i < REG_OPEN_MAX; i++) {
        if (!g_open[i].used) continue;
        pthread_mutex_lock(&g_open_locks[i]);
        if (ci_equal(g_open[i].path, path) ||
            is_descendant(g_open[i].path, path)) {
            g_open[i].used = 0u;
            g_open[i].handle = 0u;
            memset(g_open[i].path, 0, sizeof g_open[i].path);
        }
        pthread_mutex_unlock(&g_open_locks[i]);
    }
    pthread_mutex_unlock(&g_open_table_lock);
    return removed;
}

static int is_handle_in_range(uint32_t handle)
{
    return handle >= REG_HANDLE_BASE &&
           handle < REG_HANDLE_BASE + REG_OPEN_MAX * REG_HANDLE_STRIDE;
}

/* Caller holds g_open_table_lock.  Only table-owned identity is read. */
static int find_open_index_locked(uint32_t handle)
{
    for (unsigned i = 0; i < REG_OPEN_MAX; i++) {
        if (g_open[i].used && g_open[i].handle == handle)
            return (int)i;
    }
    return -1;
}

/* Caller holds g_open_table_lock and index identifies a used slot.  Pin the
 * slot, clear identity under both locks, drop the table, then clear the path
 * under the slot lock.  Both locks are released before returning. */
static void clear_open_slot_locked(int index)
{
    pthread_mutex_lock(&g_open_locks[index]);
    g_open[index].used = 0u;
    g_open[index].handle = 0u;
    pthread_mutex_unlock(&g_open_table_lock);
    memset(g_open[index].path, 0, sizeof g_open[index].path);
    pthread_mutex_unlock(&g_open_locks[index]);
}

/* Lock ownership stays inside this helper: the table lock is taken here, the
 * selected slot is pinned, the table lock is dropped, then the path is copied
 * under the slot lock.  Callers never hold g_open_table_lock across a guest
 * access because they never see it. */
static uint32_t open_handle_path(const char *path)
{
    pthread_mutex_lock(&g_open_table_lock);
    for (unsigned i = 0; i < REG_OPEN_MAX; i++) {
        if (g_open[i].used) continue;
        const uint32_t handle = REG_HANDLE_BASE + i * REG_HANDLE_STRIDE;
        g_open[i].used = 1u;
        g_open[i].handle = handle;
        pthread_mutex_lock(&g_open_locks[i]);
        pthread_mutex_unlock(&g_open_table_lock);
        copy_text(g_open[i].path, sizeof g_open[i].path, path);
        pthread_mutex_unlock(&g_open_locks[i]);
        return handle;
    }
    pthread_mutex_unlock(&g_open_table_lock);
    return 0u;
}

/* Lock ownership is likewise internal: the caller holds only g_lock. */
static int close_open_handle(uint32_t handle)
{
    pthread_mutex_lock(&g_open_table_lock);
    const int index = find_open_index_locked(handle);
    if (index < 0) {
        pthread_mutex_unlock(&g_open_table_lock);
        return 0;
    }
    clear_open_slot_locked(index);
    return 1;
}

static int is_predefined_root(uint32_t handle)
{
    return handle >= 0x80000000u && handle <= 0x8000000Fu;
}

/* Caller holds g_lock.  Resolve and pin the slot under the table lock, then
 * copy the path under the slot lock.  No pointer into g_open escapes. */
static int open_path_copy_locked(uint32_t handle, char *out, size_t capacity)
{
    int found = 0;
    pthread_mutex_lock(&g_open_table_lock);
    const int index = find_open_index_locked(handle);
    if (index >= 0) {
        pthread_mutex_lock(&g_open_locks[index]);
        pthread_mutex_unlock(&g_open_table_lock);
        copy_text(out, capacity, g_open[index].path);
        pthread_mutex_unlock(&g_open_locks[index]);
        found = 1;
    } else {
        pthread_mutex_unlock(&g_open_table_lock);
    }
    return found;
}

/* Caller holds g_lock.  Predefined roots and other handles resolve from
 * store state under that lock; target-range handles take the open table lock
 * and then their slot lock inside open_path_copy_locked. */
static void parent_path_copy_locked(uint32_t handle, char *out,
                                    size_t capacity, int *invalid)
{
    *invalid = 0;
    if (is_handle_in_range(handle)) {
        if (!open_path_copy_locked(handle, out, capacity)) *invalid = 1;
        return;
    }
    if (is_predefined_root(handle)) {
        copy_text(out, capacity, REG_ROOT);
        return;
    }
    copy_text(out, capacity, g_current_key);
}

/* Caller holds g_lock.  Same lock rule as parent_path_copy_locked. */
static void operation_path_copy_locked(uint32_t handle, char *out,
                                       size_t capacity, int *invalid)
{
    *invalid = 0;
    if (is_handle_in_range(handle)) {
        if (!open_path_copy_locked(handle, out, capacity)) *invalid = 1;
        return;
    }
    copy_text(out, capacity, g_current_key);
}

static void h_open_key(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t sub_address = wc3_legacy_arg32(c, 1u);
    const uint32_t out = wc3_legacy_arg32(c, 4u);
    char sub[REG_PATH_MAX];
    char path[REG_PATH_MAX];
    char base[REG_PATH_MAX];

    if (!read_guest_string(sub_address, sub, sizeof sub)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    /* The required output is range-checked before any host lock.  The key and
     * open are still created for an invalid destination and rolled back below. */
    const int out_valid = out && wc3_legacy_guest_ok(out, 4u);

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    parent_path_copy_locked(parent, base, sizeof base, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    resolve_subkey(base, sub, path, sizeof path);
    if (!is_ours(path)) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_FILE_NOT_FOUND;
        return;
    }

    ensure_key_and_parents_locked(path);
    const uint32_t handle = open_handle_path(path);
    if (!handle) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_NOT_ENOUGH_MEMORY;
        return;
    }
    if (!out_valid) {
        (void)close_open_handle(handle);
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_PARAMETER;
        return;
    }
    copy_text(g_current_key, sizeof g_current_key, path);
    pthread_mutex_unlock(&g_lock);

    wc3_legacy_wr32g(out, handle);
    c->eax = REG_SUCCESS;
}

static void h_create_key(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t sub_address = wc3_legacy_arg32(c, 1u);
    const uint32_t out = wc3_legacy_arg32(c, 7u);
    const uint32_t disposition = wc3_legacy_arg32(c, 8u);
    char sub[REG_PATH_MAX];
    char path[REG_PATH_MAX];
    char base[REG_PATH_MAX];

    if (!read_guest_string(sub_address, sub, sizeof sub)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    /* Both destinations are optional.  Range-check them before any host lock;
     * the key and open are still created for an invalid destination and rolled
     * back below.  Keep the original short-circuit: an invalid out is already
     * fatal, so disposition is not checked in that case. */
    const int out_valid = !out || wc3_legacy_guest_ok(out, 4u);
    const int disposition_valid =
        out_valid && (!disposition || wc3_legacy_guest_ok(disposition, 4u));

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    parent_path_copy_locked(parent, base, sizeof base, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    if (sub[0]) {
        resolve_subkey(base, sub, path, sizeof path);
    } else {
        normalize_path(base, path, sizeof path);
    }
    if (!path[0]) {
        copy_text(path, sizeof path, REG_ROOT);
    }

    const int existed = find_key_locked(path) != NULL;
    ensure_key_and_parents_locked(path);
    const uint32_t handle = open_handle_path(path);
    if (!handle) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_NOT_ENOUGH_MEMORY;
        return;
    }
    if (!out_valid || !disposition_valid) {
        (void)close_open_handle(handle);
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_PARAMETER;
        return;
    }
    const uint32_t disposition_value = existed ? 2u : 1u;
    copy_text(g_current_key, sizeof g_current_key, path);
    pthread_mutex_unlock(&g_lock);

    write_u32(out, handle);
    write_u32(disposition, disposition_value);
    c->eax = REG_SUCCESS;
}

static void h_query_value(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t name_address = wc3_legacy_arg32(c, 1u);
    const uint32_t type_out = wc3_legacy_arg32(c, 3u);
    const uint32_t data_out = wc3_legacy_arg32(c, 4u);
    const uint32_t length_io = wc3_legacy_arg32(c, 5u);
    char name[REG_NAME_MAX];
    char path[REG_PATH_MAX];

    if (!read_guest_string(name_address, name, sizeof name)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }
    if (!length_io || !wc3_legacy_guest_ok(length_io, 4u)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    /* A private copy of the stored value is taken under g_lock; all guest
     * reads and writes run after the unlock.  RegValue.len is capped at
     * REG_DATA_MAX when the slot is written. */
    uint32_t type = REG_SZ;
    uint32_t length = 0u;
    uint8_t snapshot[REG_DATA_MAX];

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    operation_path_copy_locked(handle, path, sizeof path, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
        battle_net_trace_gateways("get", path, name, NULL, 0u, 0u, 0u,
                                  REG_INVALID_HANDLE);
#endif
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    RegValue *value = find_value_locked(path, name);
    if (!value) {
        pthread_mutex_unlock(&g_lock);
#if WC3_DEBUG && WC3_DEBUG_HLE
        fprintf(stderr, "[registry] get %s\\%s = missing\n", path, name);
#endif
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
        battle_net_trace_gateways("get", path, name, NULL, 0u, 0u, 0u,
                                  REG_FILE_NOT_FOUND);
#endif
        c->eax = REG_FILE_NOT_FOUND;
        return;
    }
    type = value->type;
    length = value->len;
    if (length) memcpy(snapshot, value->data, length);
    pthread_mutex_unlock(&g_lock);

#if WC3_DEBUG && WC3_DEBUG_HLE
    uint32_t stored = 0u;
    if (length >= 4u) memcpy(&stored, snapshot, 4u);
    fprintf(stderr, "[registry] get %s\\%s = %u (type=%u len=%u)\n",
            path, name, stored, type, length);
#endif

    /* Preserve the original guest-visible order: the type is written before
     * length_io is read, so overlapping type_out/length_io behave as before. */
    write_u32(type_out, type);
    const uint32_t capacity = wc3_legacy_rd32g(length_io);
    uint32_t status = REG_SUCCESS;
    if (!data_out) {
        wc3_legacy_wr32g(length_io, length);
    } else if (capacity < length) {
        wc3_legacy_wr32g(length_io, length);
        status = REG_MORE_DATA;
    } else if (length && !wc3_legacy_guest_ok(data_out, length)) {
        status = REG_INVALID_PARAMETER;
    } else {
        for (uint32_t i = 0; i < length; i++)
            wc3_legacy_wr8g(data_out + i, snapshot[i]);
        wc3_legacy_wr32g(length_io, length);
    }
    c->eax = status;
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    battle_net_trace_gateways("get", path, name, snapshot, type, length,
                              capacity, status);
#endif
}

static void h_set_value(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t name_address = wc3_legacy_arg32(c, 1u);
    const uint32_t type = wc3_legacy_arg32(c, 3u);
    const uint32_t data = wc3_legacy_arg32(c, 4u);
    const uint32_t length = wc3_legacy_arg32(c, 5u);
    char name[REG_NAME_MAX];
    char path[REG_PATH_MAX];

    if (!read_guest_string(name_address, name, sizeof name)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }
    if (length > REG_DATA_MAX) {
        c->eax = REG_MORE_DATA;
        return;
    }
    if (length && (!data || !wc3_legacy_guest_ok(data, length))) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    /* Snapshot the caller's bytes before g_lock: the comparison and the store
     * then use the same copy, and no guest memory is touched under the lock. */
    uint8_t snapshot[REG_DATA_MAX];
    for (uint32_t i = 0; i < length; i++)
        snapshot[i] = wc3_legacy_rd8g(data + i);

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    operation_path_copy_locked(handle, path, sizeof path, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
        battle_net_trace_gateways("set", path, name, NULL, type, length, length,
                                  REG_INVALID_HANDLE);
#endif
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    ensure_key_and_parents_locked(path);
    RegValue *value = find_value_locked(path, name);
    int changed = 1;
    if (value) {
        changed = value->type != type || value->len != length;
        for (uint32_t i = 0; !changed && i < length; i++) {
            if (value->data[i] != snapshot[i]) changed = 1;
        }
    } else {
        value = slot_value_locked(path, name);
        if (!value) {
            pthread_mutex_unlock(&g_lock);
            c->eax = REG_NOT_ENOUGH_MEMORY;
            return;
        }
    }
    value->type = type;
    value->len = length;
    for (uint32_t i = 0; i < length; i++)
        value->data[i] = snapshot[i];
    if (changed) g_store_dirty = 1;
    maybe_persist_locked();
    pthread_mutex_unlock(&g_lock);

#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    battle_net_trace_gateways("set", path, name, snapshot, type, length, length,
                              REG_SUCCESS);
#endif
#if WC3_DEBUG && WC3_DEBUG_HLE
    uint32_t stored = 0u;
    if (length >= 4u) memcpy(&stored, snapshot, 4u);
    fprintf(stderr, "[registry] set %s\\%s = %u (type=%u len=%u)\n",
            path, name, stored, type, length);
#endif
    c->eax = REG_SUCCESS;
}

static void h_delete_value(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t name_address = wc3_legacy_arg32(c, 1u);
    char name[REG_NAME_MAX];
    char path[REG_PATH_MAX];

    if (!read_guest_string(name_address, name, sizeof name)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    operation_path_copy_locked(handle, path, sizeof path, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    const int removed = remove_value_locked(path, name);
    if (removed) {
        g_store_dirty = 1;
        maybe_persist_locked();
    }
    pthread_mutex_unlock(&g_lock);
    c->eax = removed ? REG_SUCCESS : REG_FILE_NOT_FOUND;
#if WC3_DEBUG && WC3_DEBUG_BATTLE_NET
    battle_net_trace_gateways("delete", path, name, NULL, 0u, 0u, 0u, c->eax);
#endif
}

static void h_delete_key(CPU *c)
{
    const uint32_t parent = wc3_legacy_arg32(c, 0u);
    const uint32_t sub_address = wc3_legacy_arg32(c, 1u);
    char sub[REG_PATH_MAX];
    char path[REG_PATH_MAX];
    char base[REG_PATH_MAX];

    if (!read_guest_string(sub_address, sub, sizeof sub)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    parent_path_copy_locked(parent, base, sizeof base, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }
    resolve_subkey(base, sub, path, sizeof path);
    if (!path[0] || ci_equal(path, REG_ROOT)) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_PARAMETER;
        return;
    }
    const int removed = delete_subtree_locked(path);
    if (removed) {
        g_store_dirty = 1;
        maybe_persist_locked();
    }
    pthread_mutex_unlock(&g_lock);
    c->eax = removed ? REG_SUCCESS : REG_FILE_NOT_FOUND;
}

static void h_flush_key(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    int invalid = 0;

    pthread_mutex_lock(&g_open_table_lock);
    if (is_handle_in_range(handle) && find_open_index_locked(handle) < 0)
        invalid = 1;
    pthread_mutex_unlock(&g_open_table_lock);
    c->eax = invalid ? REG_INVALID_HANDLE : REG_SUCCESS;
}

static void h_close_key(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    int closed = 0;

    pthread_mutex_lock(&g_open_table_lock);
    const int index = find_open_index_locked(handle);
    if (index >= 0) {
        clear_open_slot_locked(index);
        closed = 1;
    } else {
        pthread_mutex_unlock(&g_open_table_lock);
    }
    c->eax = closed ? REG_SUCCESS
                    : (is_handle_in_range(handle) ? REG_INVALID_HANDLE
                                                  : REG_SUCCESS);
}

static int collect_children_locked(const char *parent, char names[][REG_NAME_MAX],
                                   unsigned capacity, unsigned *count)
{
    const size_t parent_length = strlen(parent);
    unsigned used = 0;

    for (unsigned i = 0; i < REG_KEY_MAX; i++) {
        if (!g_keys[i].used) continue;
        if (!ci_prefix(g_keys[i].path, parent)) continue;
        if (g_keys[i].path[parent_length] != '\\') continue;
        const char *child = g_keys[i].path + parent_length + 1u;
        if (!child[0] || strchr(child, '\\')) continue;

        int duplicate = 0;
        for (unsigned k = 0; k < used; k++) {
            if (ci_equal(names[k], child)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate || used >= capacity) continue;
        copy_text(names[used], REG_NAME_MAX, child);
        used++;
    }
    *count = used;
    return 1;
}

static void h_enum_key(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t index = wc3_legacy_arg32(c, 1u);
    const uint32_t name_out = wc3_legacy_arg32(c, 2u);
    const uint32_t name_length = wc3_legacy_arg32(c, 3u);
    const uint32_t class_out = wc3_legacy_arg32(c, 5u);
    const uint32_t class_length = wc3_legacy_arg32(c, 6u);
    const uint32_t last_write = wc3_legacy_arg32(c, 7u);
    char path[REG_PATH_MAX];

    if (!name_out || !name_length ||
        !wc3_legacy_guest_ok(name_length, 4u)) {
        c->eax = REG_INVALID_PARAMETER;
        return;
    }

    /* The name capacity is a guest value; read it before the lock so the
     * locked span touches no guest memory. */
    const uint32_t capacity = wc3_legacy_rd32g(name_length);

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    operation_path_copy_locked(handle, path, sizeof path, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }

    static char children[REG_KEY_MAX][REG_NAME_MAX];
    unsigned count = 0u;
    collect_children_locked(path, children, REG_KEY_MAX, &count);
    if (index >= count) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_NO_MORE_ITEMS;
        return;
    }

    char child[REG_NAME_MAX];
    copy_text(child, sizeof child, children[index]);
    const uint32_t length = (uint32_t)strlen(child);
    pthread_mutex_unlock(&g_lock);

    if (capacity < length + 1u) {
        wc3_legacy_wr32g(name_length, length + 1u);
        c->eax = REG_MORE_DATA;
        return;
    }
    write_guest_string(name_out, child, capacity);
    wc3_legacy_wr32g(name_length, length);

    if (class_length && wc3_legacy_guest_ok(class_length, 4u)) {
        const uint32_t class_capacity = wc3_legacy_rd32g(class_length);
        if (class_out && class_capacity > 0u &&
            wc3_legacy_guest_ok(class_out, 1u))
            wc3_legacy_wr8g(class_out, 0u);
        wc3_legacy_wr32g(class_length, 0u);
    }
    if (last_write && wc3_legacy_guest_ok(last_write, 8u))
        wc3_legacy_guest_zero(last_write, 8u);

    c->eax = REG_SUCCESS;
}

static void h_query_info(CPU *c)
{
    const uint32_t handle = wc3_legacy_arg32(c, 0u);
    const uint32_t class_out = wc3_legacy_arg32(c, 1u);
    const uint32_t class_length = wc3_legacy_arg32(c, 2u);
    const uint32_t subkeys_out = wc3_legacy_arg32(c, 4u);
    const uint32_t max_subkey_out = wc3_legacy_arg32(c, 5u);
    const uint32_t max_class_out = wc3_legacy_arg32(c, 6u);
    const uint32_t values_out = wc3_legacy_arg32(c, 7u);
    const uint32_t max_name_out = wc3_legacy_arg32(c, 8u);
    const uint32_t max_data_out = wc3_legacy_arg32(c, 9u);
    const uint32_t security_out = wc3_legacy_arg32(c, 10u);
    const uint32_t last_write = wc3_legacy_arg32(c, 11u);
    char path[REG_PATH_MAX];

    pthread_mutex_lock(&g_lock);
    int invalid = 0;
    operation_path_copy_locked(handle, path, sizeof path, &invalid);
    if (invalid) {
        pthread_mutex_unlock(&g_lock);
        c->eax = REG_INVALID_HANDLE;
        return;
    }

    static char children[REG_KEY_MAX][REG_NAME_MAX];
    unsigned child_count = 0u;
    collect_children_locked(path, children, REG_KEY_MAX, &child_count);
    uint32_t max_subkey = 0u;
    for (unsigned i = 0; i < child_count; i++) {
        const uint32_t length = (uint32_t)strlen(children[i]);
        if (length > max_subkey) max_subkey = length;
    }

    uint32_t values = 0u;
    uint32_t max_name = 0u;
    uint32_t max_data = 0u;
    for (unsigned i = 0; i < REG_VALUE_MAX; i++) {
        if (!g_values[i].used || !ci_equal(g_values[i].path, path)) continue;
        values++;
        const uint32_t name_length = (uint32_t)strlen(g_values[i].name);
        if (name_length > max_name) max_name = name_length;
        if (g_values[i].len > max_data) max_data = g_values[i].len;
    }
    pthread_mutex_unlock(&g_lock);

    write_u32(subkeys_out, child_count);
    write_u32(max_subkey_out, max_subkey);
    write_u32(max_class_out, 0u);
    write_u32(values_out, values);
    write_u32(max_name_out, max_name);
    write_u32(max_data_out, max_data);
    write_u32(security_out, 0u);
    if (last_write && wc3_legacy_guest_ok(last_write, 8u))
        wc3_legacy_guest_zero(last_write, 8u);

    if (class_length && wc3_legacy_guest_ok(class_length, 4u)) {
        const uint32_t class_capacity = wc3_legacy_rd32g(class_length);
        if (class_out && class_capacity > 0u &&
            wc3_legacy_guest_ok(class_out, 1u))
            wc3_legacy_wr8g(class_out, 0u);
        wc3_legacy_wr32g(class_length, 0u);
    }

    c->eax = REG_SUCCESS;
}

static void seed_dword_locked(const char *path, const char *name,
                              uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    ensure_key_and_parents_locked(path);
    set_value_locked(path, name, REG_DWORD, bytes, 4u);
}

#if defined(WC3_W3L_GATEWAY_RUBATTLE)
/* Same value that gate.reg writes on Windows: an ANSI REG_MULTI_SZ list of
 * "version", "default gateway index", then host/zone/display-name triples.
 * The game asks Storm's SRegQueryValueEx for this value under
 * Software\Blizzard Entertainment\Warcraft III before it tries
 * BattleNet\bnserver-WAR3.ini and realms.ini, so seeding it here is the
 * Android equivalent of importing gate.reg.  The seed stays in memory and is
 * not written to wc3_registry.dat by itself. */
static const char g_rubattle_gateways[] =
    /* The PC 1.27.1.7085 registry value observed on the working
     * rubattle.net setup is "1014", then the default gateway index "00".
     * The default index is zero-based: "00" selects the first triple
     * (rubattle.net); the old "01" selected playground.ru. */
    "1014" "\0"
    "00" "\0"
    "rubattle.net" "\0"
    "-3" "\0"
    "Rubattle.net" "\0"
    "playground.ru" "\0"
    "-3" "\0"
    "PlayGround.ru" "\0"
    "eu.rubattle.net" "\0"
    "-1" "\0"
    "Eu.Rubattle.net" "\0"
    "uswest.battle.net" "\0"
    "8" "\0"
    "Lordaeron (U.S. West)" "\0"
    "useast.battle.net" "\0"
    "6" "\0"
    "Azeroth (U.S. East)" "\0"
    "asia.battle.net" "\0"
    "-9" "\0"
    "Kalimdor (Asia)" "\0"
    "europe.battle.net" "\0"
    "-1" "\0"
    "Northrend (Europe)" "\0";

static void seed_rubattle_gateways_locked(void)
{
    ensure_key_and_parents_locked(REG_ROOT);
    set_value_locked(REG_ROOT, "Battle.net Gateways", REG_MULTI_SZ,
                     (const uint8_t *)g_rubattle_gateways,
                     (uint32_t)sizeof g_rubattle_gateways);
}
#endif

static void seed_defaults_locked(void)
{
    ensure_key_and_parents_locked(REG_ROOT);
    ensure_key_and_parents_locked(REG_SOUND);
    ensure_key_and_parents_locked(REG_VIDEO);

    static const struct {
        const char *name;
        uint32_t value;
    } sound[] = {
        { "music",        1u },
        { "musicvolume",  100u },
        { "sfx",          1u },
        { "sfxvolume",    100u },
        { "ambient",      1u },
        { "movement",     1u },
        { "unit",         1u },
        { "positional",   1u },
        { "environmental", 1u },
    };
    for (unsigned i = 0; i < sizeof sound / sizeof sound[0]; i++)
        seed_dword_locked(REG_SOUND, sound[i].name, sound[i].value);

#if WC3_DEBUG && WC3_DEBUG_HLE
    const char *no_sound = getenv("WC3_NOSOUND");
    if (no_sound && *no_sound && strcmp(no_sound, "0") != 0)
        seed_dword_locked(REG_SOUND, "nosoundwarn", 1u);
#endif

    uint32_t width = 0u, height = 0u;
    if (wc3_legacy_default_display_mode(&width, &height)) {
        static const struct {
            const char *name;
            uint32_t value;
        } video[] = {
            { "reswidth",     0u },
            { "resheight",    0u },
            { "colordepth",   32u },
            { "refreshrate",  60u },
            { "adapter",      0u },
            { "lockfb",       0u },
        };
        for (unsigned i = 0; i < sizeof video / sizeof video[0]; i++) {
            uint32_t value = video[i].value;
            if (strcmp(video[i].name, "reswidth") == 0) value = width;
            if (strcmp(video[i].name, "resheight") == 0) value = height;
            seed_dword_locked(REG_VIDEO, video[i].name, value);
        }
    }
}

typedef struct RegOverride {
    const char *name;
    void (*impl)(CPU *c);
} RegOverride;

static const RegOverride g_overrides[] = {
    { "RegOpenKeyExA", h_open_key },
    { "RegQueryValueExA", h_query_value },
    { "RegSetValueExA", h_set_value },
    { "RegCreateKeyExA", h_create_key },
    { "RegCloseKey", h_close_key },
};

typedef struct RegAddition {
    const char *name;
    uint16_t argc;
    void (*impl)(CPU *c);
} RegAddition;

static const RegAddition g_additions[] = {
    { "RegDeleteKeyA", 2u, h_delete_key },
    { "RegDeleteValueA", 2u, h_delete_value },
    { "RegFlushKey", 1u, h_flush_key },
    { "RegEnumKeyExA", 8u, h_enum_key },
    { "RegQueryInfoKeyA", 12u, h_query_info },
};

int wc3_hle_advapi32_registry_install(void)
{
    if (pthread_once(&g_open_locks_once, init_open_locks) != 0 ||
        !g_open_locks_ready)
        return -1;

    pthread_mutex_lock(&g_lock);
    memset(g_keys, 0, sizeof g_keys);
    memset(g_values, 0, sizeof g_values);
    /* Install is bootstrap-only; taking the full table order also excludes
     * any slot user while the open table is cleared. */
    pthread_mutex_lock(&g_open_table_lock);
    for (unsigned i = 0; i < REG_OPEN_MAX; i++)
        pthread_mutex_lock(&g_open_locks[i]);
    memset(g_open, 0, sizeof g_open);
    for (unsigned i = REG_OPEN_MAX; i > 0u; i--)
        pthread_mutex_unlock(&g_open_locks[i - 1u]);
    pthread_mutex_unlock(&g_open_table_lock);
    copy_text(g_current_key, sizeof g_current_key, REG_ROOT);
    seed_defaults_locked();
#if WC3_DEBUG && WC3_DEBUG_HLE
    const int loaded = load_persisted_locked();
#else
    load_persisted_locked();
#endif
#if defined(WC3_W3L_GATEWAY_RUBATTLE)
    /* Restore the rubattle gateway list after persisted values, so a stale
     * save cannot hide the server from the Battle.net gateway picker. */
    seed_rubattle_gateways_locked();
#endif
    pthread_mutex_unlock(&g_lock);
#if WC3_DEBUG && WC3_DEBUG_HLE
    if (loaded >= 0)
        fprintf(stderr, "[registry] loaded %u persisted values\n",
                (unsigned)loaded);
#endif

    for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
        if (wc3_legacy_hle_override("ADVAPI32.dll", g_overrides[i].name,
                                    g_overrides[i].impl) != 0)
            return -1;
    }
    for (unsigned i = 0; i < sizeof g_additions / sizeof g_additions[0]; i++) {
        if (wc3_legacy_hle_add("ADVAPI32.dll", g_additions[i].name,
                               (int)g_additions[i].argc, 0,
                               g_additions[i].impl) != 0)
            return -1;
    }
    return (int)(sizeof g_overrides / sizeof g_overrides[0] +
                 sizeof g_additions / sizeof g_additions[0]);
}
