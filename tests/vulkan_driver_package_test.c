/*
 * vulkan_driver_package_test.c - host T0 for the ADPkg directory contract.
 *
 * The fixtures are synthetic ELF64 AArch64 headers; no Android driver is
 * needed.  The real package (Turnip) is T2 evidence and is not committed.
 */
#include "vulkan_driver_package.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_GETPID() _getpid()
#else
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0777)
#define TEST_GETPID() getpid()
#endif

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "vulkan_driver_package: FAIL: %s\n", msg); \
        return 1; \
    } \
} while (0)

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
} TestEhdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} TestPhdr;

static int mkdir_ok(const char *path)
{
    if (TEST_MKDIR(path) == 0) return 1;
    return 0;
}

static int join_path(char *dst, size_t cap, const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    if (!dst || !cap || !a || !b || la + 1u + lb + 1u > cap) return 0;
    memcpy(dst, a, la);
    dst[la] = '/';
    memcpy(dst + la + 1u, b, lb + 1u);
    return 1;
}

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int write_elf(const char *path, uint16_t machine, uint16_t type,
                     uint64_t align, int with_dynamic)
{
    TestEhdr eh;
    TestPhdr ph[2];
    FILE *f;

    memset(&eh, 0, sizeof eh);
    memset(ph, 0, sizeof ph);
    eh.e_ident[0] = 0x7F;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; /* ELFCLASS64 */
    eh.e_ident[5] = 1; /* ELFDATA2LSB */
    eh.e_ident[6] = 1; /* EV_CURRENT */
    eh.e_type = type;
    eh.e_machine = machine;
    eh.e_version = 1;
    eh.e_phoff = sizeof eh;
    eh.e_ehsize = (uint16_t)sizeof eh;
    eh.e_phentsize = (uint16_t)sizeof(TestPhdr);
    eh.e_phnum = with_dynamic ? 2 : 1;

    ph[0].p_type = 1; /* PT_LOAD */
    ph[0].p_flags = 5;
    ph[0].p_align = align;
    ph[1].p_type = 2; /* PT_DYNAMIC */
    ph[1].p_align = 8;

    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(&eh, 1, sizeof eh, f) != sizeof eh ||
        fwrite(ph, 1, (size_t)eh.e_phnum * sizeof ph[0], f) !=
            (size_t)eh.e_phnum * sizeof ph[0]) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int write_ini(const char *dir, const char *id, const char *library,
                     uint32_t size, int page16k)
{
    char path[1024];
    char text[1024];
    int n;
    if (!join_path(path, sizeof path, dir, "driver.ini")) return 0;
    n = snprintf(text, sizeof text,
                 "id=%s\n"
                 "library=%s\n"
                 "min_api=28\n"
                 "sha256=%s\n"
                 "size=%u\n"
                 "page16k=%d\n"
                 "installed_at=1700000000\n",
                 id, library,
                 "0123456789abcdef0123456789abcdef"
                 "0123456789abcdef0123456789abcdef",
                 (unsigned)size, page16k);
    if (n <= 0 || (size_t)n >= sizeof text) return 0;
    return write_file(path, text, (size_t)n);
}

static int pad_file(const char *path, uint32_t total_size)
{
    FILE *f;
    long current;
    unsigned char zeros[4096] = {0};

    if (!path) return 0;
    f = fopen(path, "ab");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    current = ftell(f);
    if (current < 0) {
        fclose(f);
        return 0;
    }
    while ((uint32_t)current < total_size) {
        size_t chunk = (size_t)(total_size - (uint32_t)current);
        if (chunk > sizeof zeros) chunk = sizeof zeros;
        if (fwrite(zeros, 1, chunk, f) != chunk) {
            fclose(f);
            return 0;
        }
        current += (long)chunk;
    }
    fclose(f);
    return 1;
}

static int make_package(const char *root, const char *id, const char *library,
                        uint16_t machine, uint16_t type, uint64_t align,
                        int with_dynamic, uint32_t size_override,
                        int page_override, int installed)
{
    char dir[1024];
    char lib[1024];
    char path[1024];
    struct stat st;
    uint32_t actual = 0;
    int page16k = 0;

    if (!join_path(dir, sizeof dir, root, id)) return 0;
    if (!mkdir_ok(dir)) return 0;
    if (!join_path(lib, sizeof lib, dir, library)) return 0;
    if (!write_elf(lib, machine, type, align, with_dynamic)) return 0;
    if (stat(lib, &st) != 0) return 0;
    actual = (uint32_t)st.st_size;
    if (!write_ini(dir, id, library,
                   size_override ? size_override : actual,
                   page_override >= 0 ? page_override : (align >= 16384))) {
        return 0;
    }
    (void)page16k;
    if (installed) {
        if (!join_path(path, sizeof path, dir, ".installed")) return 0;
        if (!write_file(path, "", 0)) return 0;
    }
    return 1;
}

int main(void)
{
    char root[512];
    char id[64];
    char lib_path[1024];
    Wc3DriverPackage pkg;
    int rc;

    snprintf(root, sizeof root, "build/host/vdp_%d", (int)TEST_GETPID());
    (void)mkdir_ok("build");
    (void)mkdir_ok("build/host");
    (void)mkdir_ok(root);

    CHECK(!wc3_driver_id_valid(NULL), "null id");
    CHECK(!wc3_driver_id_valid("0123456789abcde"), "short id");
    CHECK(!wc3_driver_id_valid("0123456789ABCDEF"), "uppercase id");
    CHECK(!wc3_driver_id_valid("0123456789abcdeg"), "non-hex id");
    CHECK(wc3_driver_id_valid("0123456789abcdef"), "valid id");

    snprintf(id, sizeof id, "%s", "0123456789abcdef");
    CHECK(make_package(root, id, "vulkan.test.so", 183, 3, 16384, 1, 0, -1, 1),
          "fixture create");
    rc = wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path);
    CHECK(rc == WC3_DRIVER_OK, "valid registry entry");
    CHECK(strcmp(pkg.library, "vulkan.test.so") == 0, "library name");
    CHECK(pkg.page16k == 1, "16k page flag");
    CHECK(strstr(lib_path, "vulkan.test.so") != NULL, "library path");

    snprintf(id, sizeof id, "%s", "1111111111111111");
    CHECK(make_package(root, id, "vulkan.badarch.so", 62, 3, 4096, 1, 0, -1, 1),
          "bad arch fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_ELF, "x86 ELF rejected");

    snprintf(id, sizeof id, "%s", "2222222222222222");
    CHECK(make_package(root, id, "vulkan.exec.so", 183, 2, 4096, 1, 0, -1, 1),
          "ET_EXEC fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_ELF, "ET_EXEC rejected");

    snprintf(id, sizeof id, "%s", "3333333333333333");
    CHECK(make_package(root, id, "vulkan.nodynamic.so", 183, 3, 4096, 0, 0, -1, 1),
          "no PT_DYNAMIC fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_ELF, "missing PT_DYNAMIC rejected");

    snprintf(id, sizeof id, "%s", "4444444444444444");
    CHECK(make_package(root, id, "vulkan.size.so", 183, 3, 4096, 1, 123, -1, 1),
          "wrong size fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_ELF, "size mismatch rejected");

    snprintf(id, sizeof id, "%s", "5555555555555555");
    CHECK(make_package(root, id, "vulkan.page.so", 183, 3, 4096, 1, 0, 1, 1),
          "wrong page fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_ELF, "page16k mismatch rejected");

    {
        char dir[1024];
        char lib[1024];
        struct stat st;

        snprintf(id, sizeof id, "%s", "8888888888888888");
        CHECK(make_package(root, id, "vulkan.large.so", 183, 3, 16384, 1,
                           0, -1, 1),
              "large library fixture");
        CHECK(join_path(dir, sizeof dir, root, id), "large library dir");
        CHECK(join_path(lib, sizeof lib, dir, "vulkan.large.so"),
              "large library path");
        CHECK(pad_file(lib, 1000001u), "large library padding");
        CHECK(stat(lib, &st) == 0, "large library stat");
        CHECK(write_ini(dir, id, "vulkan.large.so", (uint32_t)st.st_size, 1),
              "large library ini");
        CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path,
                                      sizeof lib_path) == WC3_DRIVER_OK,
              "large library accepted");
        CHECK(pkg.size == (uint32_t)st.st_size, "large library size preserved");
    }

    snprintf(id, sizeof id, "%s", "6666666666666666");
    CHECK(make_package(root, id, "../escape.so", 183, 3, 4096, 1, 0, -1, 1),
          "path fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_INI, "path traversal rejected");

    snprintf(id, sizeof id, "%s", "7777777777777777");
    CHECK(make_package(root, id, "vulkan.noinstall.so", 183, 3, 4096, 1, 0, -1, 0),
          "missing marker fixture");
    CHECK(wc3_driver_registry_get(root, id, &pkg, lib_path, sizeof lib_path) ==
              WC3_DRIVER_ERR_IO, "missing .installed rejected");

    CHECK(wc3_driver_registry_get(root, "not-an-id", &pkg, lib_path,
                                  sizeof lib_path) == WC3_DRIVER_ERR_ID,
          "registry rejects bad id");
    CHECK(wc3_driver_registry_get(root, "9999999999999999", &pkg, lib_path,
                                  sizeof lib_path) == WC3_DRIVER_ERR_IO,
          "registry rejects missing entry");

    printf("vulkan_driver_package: OK\n");
    return 0;
}
