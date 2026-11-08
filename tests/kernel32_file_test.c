/*
 * kernel32_file_test.c - T0 contract for the O-26/O-27 chained file rows.
 *
 * The fake shim keeps the as-built argument stack shape, so the wrapper's
 * temporary arg0 rewrite is exercised through the same path the real thunk
 * uses.  The fake previous handlers only record the spelling they receive and
 * answer from the host filesystem; they do not duplicate the as-built file
 * implementation.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#define MKDIR(p) mkdir((p), 0775)
#define RMDIR(p) rmdir(p)
#endif

#include "cpu.h"
#include "kernel32_file_hle.h"
#include "legacy_shim.h"

#define PATH_MAX_LOCAL 512u

/* The host test runs on NTFS, where access() is case-insensitive.  Wrap it for
 * the temporary tree so the test exercises the same leaf rule as Android:
 * an exact directory entry succeeds, a case-only mismatch fails. */
extern int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode);

int __wrap_access(const char *path, int mode)
{
    const char *slash;
    const char *leaf;
    char dir[PATH_MAX_LOCAL];
    DIR *d;
    struct dirent *entry;
    int any_case_match = 0;

    if (mode != F_OK || !path) return __real_access(path, mode);
    slash = strrchr(path, '/');
    leaf = slash ? slash + 1 : path;
    if (!leaf[0]) return __real_access(path, mode);
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof dir) return __real_access(path, mode);
        memcpy(dir, path, n);
        dir[n] = '\0';
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    d = opendir(dir);
    if (!d) return __real_access(path, mode);
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, leaf)) {
            closedir(d);
            return __real_access(path, mode);
        }
        if (!strcasecmp(entry->d_name, leaf)) any_case_match = 1;
    }
    closedir(d);
    return any_case_match ? -1 : __real_access(path, mode);
}

#define GUEST_BASE 0x10000000u
#define GUEST_SIZE 0x00020000u
#define STACK_ADDR 0x10010000u
#define STR_ADDR 0x10011000u
#define HEAP_ADDR 0x10012000u

typedef struct Row {
    const char *dll;
    const char *name;
    void (*impl)(CPU *c);
    void (*previous)(CPU *c);
} Row;

static void previous_create_file(CPU *c);
static void previous_get_attributes(CPU *c);
static void previous_set_cwd(CPU *c);

static Row g_rows[4];
static unsigned g_row_count;
static uint8_t g_guest[GUEST_SIZE];
static uint32_t g_heap_next = HEAP_ADDR;
static char g_last_path[PATH_MAX_LOCAL];
static unsigned g_create_calls;
static unsigned g_attr_calls;

static void *guest_ptr(uint32_t address, uint32_t size)
{
    if (address < GUEST_BASE) return NULL;
    const uint32_t offset = address - GUEST_BASE;
    if (offset > GUEST_SIZE || size > GUEST_SIZE - offset) return NULL;
    return &g_guest[offset];
}

int wc3_legacy_guest_ok(uint32_t address, uint32_t size)
{
    return guest_ptr(address, size) != NULL;
}

uint8_t wc3_legacy_rd8g(uint32_t address)
{
    const uint8_t *p = (const uint8_t *)guest_ptr(address, 1u);
    return p ? *p : 0;
}

void wc3_legacy_wr8g(uint32_t address, uint8_t value)
{
    uint8_t *p = (uint8_t *)guest_ptr(address, 1u);
    if (p) *p = value;
}

uint32_t wc3_legacy_rd32g(uint32_t address)
{
    const uint8_t *p = (const uint8_t *)guest_ptr(address, 4u);
    uint32_t value = 0;
    if (p) memcpy(&value, p, sizeof value);
    return value;
}

void wc3_legacy_wr32g(uint32_t address, uint32_t value)
{
    uint8_t *p = (uint8_t *)guest_ptr(address, 4u);
    if (p) memcpy(p, &value, sizeof value);
}

uint32_t wc3_legacy_arg32(const CPU *cpu, unsigned index)
{
    if (!cpu) return 0;
    return wc3_legacy_rd32g(cpu->esp + 4u + index * 4u);
}

uint32_t wc3_legacy_guest_alloc(uint32_t size)
{
    uint32_t address = g_heap_next;
    uint32_t aligned = (size + 7u) & ~7u;
    if (aligned > GUEST_SIZE || address - GUEST_BASE > GUEST_SIZE - aligned)
        return 0;
    g_heap_next += aligned;
    return address;
}

void wc3_legacy_guest_free(uint32_t address)
{
    (void)address;
}

int wc3_hle_msvcr80_read_guest_path(uint32_t address, char *buffer,
                                    size_t capacity)
{
    size_t i = 0;
    if (!buffer || capacity < 2u) return 0;
    while (i + 1u < capacity) {
        char ch = (char)wc3_legacy_rd8g(address + (uint32_t)i);
        buffer[i++] = ch;
        if (!ch) return 1;
    }
    return 0;
}

void wc3_hle_msvcr80_map_path(const char *input, char *output, size_t capacity)
{
    size_t out = 0;
    int keep_root = 1;

    if (!input || capacity == 0u) return;
    if (isalpha((unsigned char)input[0]) && input[1] == ':') {
        keep_root = (input[0] == 'Z' || input[0] == 'z') &&
                    (input[2] == '\\' || input[2] == '/');
        input += 2;
    }
    while (*input == '\\' || *input == '/') {
        if (keep_root && out == 0u && out + 1u < capacity)
            output[out++] = '/';
        input++;
    }
    for (; *input && out + 1u < capacity; input++)
        output[out++] = (*input == '\\') ? '/' : *input;
    output[out] = '\0';
    if (out == 0u && capacity >= 2u) {
        output[0] = '.';
        output[1] = '\0';
    }
}

int wc3_legacy_hle_chain(const char *dll, const char *name,
                         void (*impl)(CPU *c),
                         void (**previous)(CPU *c))
{
    if (!dll || !name || !impl || !previous || g_row_count >= 4u) return -1;
    g_rows[g_row_count].dll = dll;
    g_rows[g_row_count].name = name;
    g_rows[g_row_count].impl = impl;
    g_rows[g_row_count].previous = NULL;
    *previous = NULL;
    if (!strcmp(name, "CreateFileA")) {
        g_rows[g_row_count].previous = previous_create_file;
        *previous = previous_create_file;
    } else if (!strcmp(name, "GetFileAttributesA")) {
        g_rows[g_row_count].previous = previous_get_attributes;
        *previous = previous_get_attributes;
    } else if (!strcmp(name, "SetCurrentDirectoryA")) {
        g_rows[g_row_count].previous = previous_set_cwd;
        *previous = previous_set_cwd;
    }
    g_row_count++;
    return 0;
}

static Row *find_row(const char *name)
{
    for (unsigned i = 0; i < g_row_count; i++)
        if (!strcmp(g_rows[i].name, name)) return &g_rows[i];
    return NULL;
}

static int setup_previous(Row *row, void (*previous)(CPU *c))
{
    if (!row) return 0;
    row->previous = previous;
    return 1;
}

static void write_guest_string(uint32_t address, const char *text)
{
    size_t i;
    for (i = 0; text[i]; i++)
        wc3_legacy_wr8g(address + (uint32_t)i, (uint8_t)text[i]);
    wc3_legacy_wr8g(address + (uint32_t)i, 0);
}

static void set_stack_args(CPU *c, uint32_t arg0, uint32_t arg1,
                           uint32_t arg4, uint32_t arg5)
{
    c->esp = STACK_ADDR;
    wc3_legacy_wr32g(STACK_ADDR + 4u, arg0);
    wc3_legacy_wr32g(STACK_ADDR + 8u, arg1);
    wc3_legacy_wr32g(STACK_ADDR + 12u, 0);
    wc3_legacy_wr32g(STACK_ADDR + 16u, 0);
    wc3_legacy_wr32g(STACK_ADDR + 20u, arg4);
    wc3_legacy_wr32g(STACK_ADDR + 24u, arg5);
}

static void record_path(CPU *c)
{
    uint32_t address = wc3_legacy_arg32(c, 0);
    size_t i;
    for (i = 0; i + 1u < sizeof g_last_path; i++) {
        char ch = (char)wc3_legacy_rd8g(address + (uint32_t)i);
        g_last_path[i] = ch;
        if (!ch) return;
    }
    g_last_path[sizeof g_last_path - 1u] = '\0';
}

static void previous_create_file(CPU *c)
{
    char host[PATH_MAX_LOCAL];
    record_path(c);
    g_create_calls++;
    wc3_hle_msvcr80_map_path(g_last_path, host, sizeof host);
    c->eax = access(host, F_OK) == 0 ? 0x100u + g_create_calls : 0xFFFFFFFFu;
}

static void previous_get_attributes(CPU *c)
{
    char host[PATH_MAX_LOCAL];
    struct stat st;
    record_path(c);
    g_attr_calls++;
    wc3_hle_msvcr80_map_path(g_last_path, host, sizeof host);
    if (access(host, F_OK) != 0) {
        c->eax = 0xFFFFFFFFu;
        return;
    }
    c->eax = stat(host, &st) == 0 ? 0x20u : 0xFFFFFFFFu;
}

static void previous_set_cwd(CPU *c)
{
    char host[PATH_MAX_LOCAL];
    record_path(c);
    wc3_hle_msvcr80_map_path(g_last_path, host, sizeof host);
    c->eax = chdir(host) == 0 ? 1u : 0u;
}

static int write_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (fputs("x", f) < 0) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

/* The exact-spelling precedence case needs two directory entries that differ
 * only by case.  NTFS cannot represent that, so only exercise it on a
 * case-sensitive host; the live access() check is still in the production
 * path when the branch is skipped. */
static int host_is_case_sensitive(void)
{
    return __real_access("Data/other.txt", F_OK) == 0 &&
           __real_access("Data/OTHER.TXT", F_OK) != 0;
}

static int expect_path(const char *expected, const char *label)
{
    if (strcmp(g_last_path, expected) == 0) return 1;
    fprintf(stderr, "kernel32_file: %s: got \"%s\", expected \"%s\"\n",
            label, g_last_path, expected);
    return 0;
}

static int cleanup_tree(void)
{
    remove("build/host/kernel32_file_tmp/Data/war3.mpq");
    remove("build/host/kernel32_file_tmp/Data/other.txt");
    remove("build/host/kernel32_file_tmp/Other/War3.MPQ");
    RMDIR("build/host/kernel32_file_tmp/Data");
    RMDIR("build/host/kernel32_file_tmp/Other");
    RMDIR("build/host/kernel32_file_tmp");
    return 1;
}

int main(void)
{
    CPU cpu;
    char saved_cwd[PATH_MAX_LOCAL];
    Row *create_row;
    Row *attr_row;
    Row *cwd_row;

    memset(&cpu, 0, sizeof cpu);
    if (!getcwd(saved_cwd, sizeof saved_cwd)) return 1;
    cleanup_tree();
    MKDIR("build");
    MKDIR("build/host");
    if (MKDIR("build/host/kernel32_file_tmp") != 0 && errno != EEXIST) {
        fprintf(stderr, "kernel32_file: cannot create temp dir: %s\n",
                strerror(errno));
        return 1;
    }
    MKDIR("build/host/kernel32_file_tmp/Data");
    MKDIR("build/host/kernel32_file_tmp/Other");
    if (!write_file("build/host/kernel32_file_tmp/Data/war3.mpq") ||
        !write_file("build/host/kernel32_file_tmp/Data/other.txt") ||
        !write_file("build/host/kernel32_file_tmp/Other/War3.MPQ")) {
        fprintf(stderr, "kernel32_file: cannot create fixtures\n");
        return 1;
    }
    if (chdir("build/host/kernel32_file_tmp") != 0) return 1;

    if (wc3_hle_kernel32_file_install() != 3) {
        fprintf(stderr, "kernel32_file: install did not chain three rows\n");
        return 1;
    }
    create_row = find_row("CreateFileA");
    attr_row = find_row("GetFileAttributesA");
    cwd_row = find_row("SetCurrentDirectoryA");
    if (!setup_previous(create_row, previous_create_file) ||
        !setup_previous(attr_row, previous_get_attributes) ||
        !setup_previous(cwd_row, previous_set_cwd)) {
        fprintf(stderr, "kernel32_file: missing chained row\n");
        return 1;
    }

    write_guest_string(STR_ADDR, "Data\\War3.mpq");
    set_stack_args(&cpu, STR_ADDR, 0x80000000u, 3u, 0u);
    create_row->impl(&cpu);
    if (!expect_path("Data\\war3.mpq", "first case resolution"))
        return 1;
    if (wc3_legacy_rd8g(STR_ADDR + 5u) != 'W')
        return 1; /* the caller's string stays untouched */

    set_stack_args(&cpu, STR_ADDR, 0x80000000u, 3u, 0u);
    create_row->impl(&cpu);
    if (!expect_path("Data\\war3.mpq", "cached case resolution"))
        return 1;

    set_stack_args(&cpu, STR_ADDR, 0, 0, 0);
    attr_row->impl(&cpu);
    if (!expect_path("Data\\war3.mpq", "attribute case resolution"))
        return 1;
    if (cpu.eax != 0x20u) return 1;

    /* On a case-sensitive host both spellings can coexist.  The exact
     * requested spelling must win over the cached case variant; otherwise a
     * newly created file could be shadowed by the old resolution. */
    if (host_is_case_sensitive()) {
        if (!write_file("Data/War3.mpq")) return 1;
        set_stack_args(&cpu, STR_ADDR, 0x80000000u, 3u, 0u);
        create_row->impl(&cpu);
        if (!expect_path("Data\\War3.mpq", "exact spelling precedence"))
            return 1;
        remove("Data/War3.mpq");
        /* Rebuild the case-variant entry for the deletion test. */
        write_guest_string(STR_ADDR, "Data\\WAR3.MPQ");
        set_stack_args(&cpu, STR_ADDR, 0x80000000u, 3u, 0u);
        create_row->impl(&cpu);
        if (!expect_path("Data\\war3.mpq", "case-variant cache rebuild"))
            return 1;
    }

    /* A deleted cached spelling must fall back to the requested spelling and
     * let the as-built handler report not-found. */
    write_guest_string(STR_ADDR, "Data\\War3.mpq");
    remove("Data/war3.mpq");
    set_stack_args(&cpu, STR_ADDR, 0, 0, 0);
    attr_row->impl(&cpu);
    if (!expect_path("Data\\War3.mpq", "live invalidation"))
        return 1;
    if (cpu.eax != 0xFFFFFFFFu) return 1;
    if (!write_file("Data/war3.mpq")) return 1;

    /* Relative names are tied to the current directory; the chained
     * SetCurrentDirectoryA must flush the old resolution. */
    write_guest_string(STR_ADDR, "Other");
    set_stack_args(&cpu, STR_ADDR, 0, 0, 0);
    cwd_row->impl(&cpu);
    if (cpu.eax != 1u) return 1;

    write_guest_string(STR_ADDR, "war3.mpq");
    set_stack_args(&cpu, STR_ADDR, 0, 0, 0);
    attr_row->impl(&cpu);
    if (!expect_path("War3.MPQ", "cwd cache flush"))
        return 1;
    if (cpu.eax != 0x20u) return 1;

    /* A rooted non-Z drive keeps the as-built mapping and must not be rewritten
     * through the target cache. */
    write_guest_string(STR_ADDR, "C:\\Data\\War3.mpq");
    set_stack_args(&cpu, STR_ADDR, 0, 0, 0);
    attr_row->impl(&cpu);
    if (!expect_path("C:\\Data\\War3.mpq", "foreign drive passthrough"))
        return 1;

    if (chdir(saved_cwd) != 0) return 1;
    cleanup_tree();
    printf("kernel32_file: path cache, live invalidation and cwd flush OK\n");
    return 0;
}
