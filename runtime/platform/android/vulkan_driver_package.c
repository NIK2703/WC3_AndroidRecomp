/*
 * vulkan_driver_package.c - ADPkg directory validation and registry lookup.
 *
 * Deliberately free of Android headers so the same rules run in a host T0
 * test.  JSON, ZIP and SHA-256 belong to the Kotlin importer; this file reads
 * the small machine record it writes and refuses anything that could escape
 * the app-private driver root.
 */
#define _GNU_SOURCE 1

#include "vulkan_driver_package.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define INI_MAX 4096u
#define SHA256_HEX 64u

/* The host T0 toolchain does not declare lstat(), and the host test does not
 * create symlinks.  Android always uses lstat so a symlinked driver directory
 * is rejected instead of followed. */
#if defined(__ANDROID__) || defined(__linux__)
#define DRV_LSTAT lstat
#else
#define DRV_LSTAT stat
#endif

/* ELF64 on-disk layouts.  <elf.h> is not available in every host toolchain
 * that runs the T0 test, and the generated driver is always little-endian
 * AArch64 anyway. */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} DrvElf64Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} DrvElf64Phdr;

typedef char drv_ehdr_size_check[(sizeof(DrvElf64Ehdr) == 64u) ? 1 : -1];
typedef char drv_phdr_size_check[(sizeof(DrvElf64Phdr) == 56u) ? 1 : -1];

enum {
    DRV_ELF_CLASS64 = 2,
    DRV_ELF_DATA_LSB = 1,
    DRV_ELF_ET_DYN = 3,
    DRV_ELF_EM_AARCH64 = 183,
    DRV_PT_LOAD = 1,
    DRV_PT_DYNAMIC = 2,
    DRV_PAGE_16K = 16384
};

static int all_zero(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] != '\0') return 0;
    return 1;
}

int wc3_driver_id_valid(const char *id)
{
    size_t i;
    if (!id || strlen(id) != 16u) return 0;
    for (i = 0; i < 16u; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int library_valid(const char *name)
{
    size_t i, len;
    if (!name) return 0;
    len = strlen(name);
    if (len < 4u || len >= WC3_DRIVER_LIBRARY_MAX) return 0;
    if (name[0] == '.' || strstr(name, "..") != NULL) return 0;
    if (strcmp(name + len - 3u, ".so") != 0) return 0;
    for (i = 0; i < len; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > 1000000ul) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* A driver library is larger than the 1,000,000-byte cap that is right for
 * min_api and page16k; the recorded size is the actual ELF file size. */
static int parse_u32_size(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !*s) return 0;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > 0xFFFFFFFFull) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !*s) return 0;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    *out = (uint64_t)v;
    return 1;
}

static int sha256_valid(const char *s)
{
    size_t i;
    if (!s || strlen(s) != SHA256_HEX) return 0;
    for (i = 0; i < SHA256_HEX; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int read_ini(const char *dir, Wc3DriverPackage *out)
{
    char path[WC3_DRIVER_DIR_MAX];
    char buf[INI_MAX + 1u];
    int fd;
    ssize_t n;
    size_t used = 0;
    char *line;
    char *save = NULL;

    if (!dir || !out) return WC3_DRIVER_ERR_INI;
    if (snprintf(path, sizeof path, "%s/driver.ini", dir) <= 0 ||
        strlen(path) >= sizeof path)
        return WC3_DRIVER_ERR_PATH;

    fd = open(path, O_RDONLY);
    if (fd < 0) return WC3_DRIVER_ERR_IO;
    while (used < INI_MAX) {
        n = read(fd, buf + used, INI_MAX - used);
        if (n < 0) {
            close(fd);
            return WC3_DRIVER_ERR_IO;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (used == 0 || used >= INI_MAX) return WC3_DRIVER_ERR_INI;
    buf[used] = '\0';

    memset(out, 0, sizeof *out);
    {
        int have_id = 0, have_library = 0, have_min_api = 0;
        int have_sha = 0, have_size = 0, have_page = 0, have_installed = 0;

        for (line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *eq;
            char *key;
            char *value;
            size_t len = strlen(line);
            while (len && (line[len - 1u] == '\r' || line[len - 1u] == ' '))
                line[--len] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            eq = strchr(line, '=');
            if (!eq) return WC3_DRIVER_ERR_INI;
            *eq = '\0';
            key = line;
            value = eq + 1;
            if (!strcmp(key, "id")) {
                if (strlen(value) >= sizeof out->id) return WC3_DRIVER_ERR_INI;
                snprintf(out->id, sizeof out->id, "%s", value);
                have_id = 1;
            } else if (!strcmp(key, "library")) {
                if (strlen(value) >= sizeof out->library)
                    return WC3_DRIVER_ERR_INI;
                snprintf(out->library, sizeof out->library, "%s", value);
                have_library = 1;
            } else if (!strcmp(key, "min_api")) {
                if (!parse_u32(value, &out->min_api))
                    return WC3_DRIVER_ERR_INI;
                have_min_api = 1;
            } else if (!strcmp(key, "sha256")) {
                if (!sha256_valid(value)) return WC3_DRIVER_ERR_INI;
                have_sha = 1;
            } else if (!strcmp(key, "size")) {
                if (!parse_u32_size(value, &out->size))
                    return WC3_DRIVER_ERR_INI;
                have_size = 1;
            } else if (!strcmp(key, "page16k")) {
                uint32_t v;
                if (!parse_u32(value, &v) || v > 1u)
                    return WC3_DRIVER_ERR_INI;
                out->page16k = (int)v;
                have_page = 1;
            } else if (!strcmp(key, "installed_at")) {
                if (!parse_u64(value, &out->installed_at))
                    return WC3_DRIVER_ERR_INI;
                have_installed = 1;
            }
            /* Unknown keys are ignored so a newer installer can add fields. */
        }
        if (!have_id || !have_library || !have_min_api || !have_sha ||
            !have_size || !have_page || !have_installed)
            return WC3_DRIVER_ERR_INI;
    }

    if (!wc3_driver_id_valid(out->id)) return WC3_DRIVER_ERR_ID;
    if (!library_valid(out->library)) return WC3_DRIVER_ERR_INI;
    if (out->min_api < 1u || out->min_api > 1000u)
        return WC3_DRIVER_ERR_INI;
    return WC3_DRIVER_OK;
}

int wc3_driver_elf_check(const char *path, uint32_t *size, int *page16k)
{
    DrvElf64Ehdr eh;
    struct stat st;
    int fd;
    int have_load = 0;
    int have_dynamic = 0;
    uint64_t max_align = 0;
    uint64_t i;

    if (!path) return WC3_DRIVER_ERR_PATH;
    if (DRV_LSTAT(path, &st) != 0) return WC3_DRIVER_ERR_IO;
    if (!S_ISREG(st.st_mode)) return WC3_DRIVER_ERR_PATH;
    if (st.st_size < 0 || (uint64_t)st.st_size < sizeof eh ||
        (uint64_t)st.st_size > 0xFFFFFFFFu)
        return WC3_DRIVER_ERR_ELF;

    fd = open(path, O_RDONLY);
    if (fd < 0) return WC3_DRIVER_ERR_IO;
    if (read(fd, &eh, sizeof eh) != (ssize_t)sizeof eh) {
        close(fd);
        return WC3_DRIVER_ERR_ELF;
    }

    if (eh.e_ident[0] != 0x7Fu || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != DRV_ELF_CLASS64 ||
        eh.e_ident[5] != DRV_ELF_DATA_LSB ||
        eh.e_ident[6] != 1u ||
        eh.e_type != DRV_ELF_ET_DYN ||
        eh.e_machine != DRV_ELF_EM_AARCH64 ||
        eh.e_phentsize != (uint16_t)sizeof(DrvElf64Phdr) ||
        eh.e_phnum == 0u || eh.e_phnum > 256u ||
        eh.e_phoff > (uint64_t)st.st_size ||
        eh.e_phoff + (uint64_t)eh.e_phnum * sizeof(DrvElf64Phdr) >
            (uint64_t)st.st_size) {
        close(fd);
        return WC3_DRIVER_ERR_ELF;
    }

    if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) < 0) {
        close(fd);
        return WC3_DRIVER_ERR_IO;
    }
    for (i = 0; i < eh.e_phnum; i++) {
        DrvElf64Phdr ph;
        if (read(fd, &ph, sizeof ph) != (ssize_t)sizeof ph) {
            close(fd);
            return WC3_DRIVER_ERR_ELF;
        }
        if (ph.p_type == DRV_PT_LOAD) {
            have_load = 1;
            if (ph.p_align > max_align) max_align = ph.p_align;
        } else if (ph.p_type == DRV_PT_DYNAMIC) {
            have_dynamic = 1;
        }
    }
    close(fd);

    if (!have_load || !have_dynamic) return WC3_DRIVER_ERR_ELF;
    if (size) *size = (uint32_t)st.st_size;
    if (page16k) *page16k = max_align >= DRV_PAGE_16K;
    return WC3_DRIVER_OK;
}

int wc3_driver_check_dir(const char *dir, Wc3DriverPackage *out,
                         char *lib_path, size_t lib_path_cap)
{
    Wc3DriverPackage pkg;
    char path[WC3_DRIVER_DIR_MAX];
    struct stat st;
    uint32_t elf_size = 0;
    int elf_page16k = 0;
    int rc;

    if (!dir || !out || !lib_path || lib_path_cap == 0u)
        return WC3_DRIVER_ERR_PATH;
    if (DRV_LSTAT(dir, &st) != 0) return WC3_DRIVER_ERR_IO;
    if (!S_ISDIR(st.st_mode)) return WC3_DRIVER_ERR_PATH;

    if (snprintf(path, sizeof path, "%s/.installed", dir) <= 0 ||
        strlen(path) >= sizeof path)
        return WC3_DRIVER_ERR_PATH;
    if (DRV_LSTAT(path, &st) != 0) return WC3_DRIVER_ERR_IO;
    if (!S_ISREG(st.st_mode) || st.st_size != 0) return WC3_DRIVER_ERR_PATH;

    rc = read_ini(dir, &pkg);
    if (rc != WC3_DRIVER_OK) return rc;

    if (snprintf(lib_path, lib_path_cap, "%s/%s", dir, pkg.library) <= 0 ||
        strlen(lib_path) >= lib_path_cap)
        return WC3_DRIVER_ERR_PATH;
    if (DRV_LSTAT(lib_path, &st) != 0) return WC3_DRIVER_ERR_IO;
    if (!S_ISREG(st.st_mode)) return WC3_DRIVER_ERR_PATH;

    rc = wc3_driver_elf_check(lib_path, &elf_size, &elf_page16k);
    if (rc != WC3_DRIVER_OK) return rc;
    if (pkg.size != elf_size) return WC3_DRIVER_ERR_ELF;
    if (pkg.page16k != elf_page16k) return WC3_DRIVER_ERR_ELF;

    *out = pkg;
    return WC3_DRIVER_OK;
}

int wc3_driver_registry_get(const char *root, const char *id,
                            Wc3DriverPackage *out,
                            char *lib_path, size_t lib_path_cap)
{
    char dir[WC3_DRIVER_DIR_MAX];
    int rc;

    if (!root || !*root || !wc3_driver_id_valid(id)) return WC3_DRIVER_ERR_ID;
    if (all_zero(id, 16u)) return WC3_DRIVER_ERR_ID;
    if (snprintf(dir, sizeof dir, "%s/%s", root, id) <= 0 ||
        strlen(dir) >= sizeof dir)
        return WC3_DRIVER_ERR_PATH;

    rc = wc3_driver_check_dir(dir, out, lib_path, lib_path_cap);
    if (rc != WC3_DRIVER_OK) return rc;
    if (strcmp(out->id, id) != 0) return WC3_DRIVER_ERR_ID;
    return WC3_DRIVER_OK;
}
