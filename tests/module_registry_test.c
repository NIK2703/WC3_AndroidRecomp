/*
 * module_registry_test.c - cross-module import binding without licensed PEs.
 *
 * The test fabricates a minimal PE image layout directly in a low-address
 * arena: an export directory with one named and one ordinal export, and an
 * import descriptor with a name import and an ordinal import.  The real
 * wc3_pe_export()/wc3_pe_export_ordinal()/wc3_pe_bind_imports() code runs
 * against it, so the test pins the actual lookup and IAT rewrite paths.
 *
 * Low memory matters: Wc3PeImage stores guest VAs as uint32_t and the product
 * maps images below 4 GiB.  On Windows the arena comes from VirtualAlloc at a
 * fixed hint; on POSIX from mmap.  A failure to map low is a test failure, not
 * a skip: otherwise the binding path would silently lose coverage.
 */
#include "guest_modules.h"
#include "pe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define ARENA_SIZE 0x10000u

static int fails;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static void put16(uint8_t *base, uint32_t off, uint16_t value)
{
    memcpy(base + off, &value, sizeof value);
}

static void put32(uint8_t *base, uint32_t off, uint32_t value)
{
    memcpy(base + off, &value, sizeof value);
}

static uint32_t get32(const uint8_t *base, uint32_t off)
{
    uint32_t value;
    memcpy(&value, base + off, sizeof value);
    return value;
}

static uint8_t *arena_map(uint32_t hint)
{
#ifdef _WIN32
    for (uintptr_t base = hint; base < (uintptr_t)hint + 0x08000000u;
         base += 0x00100000u) {
        void *p = VirtualAlloc((void *)base, ARENA_SIZE,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return (uint8_t *)p;
    }
    return NULL;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#else
    flags |= MAP_FIXED;
#endif
    void *p = mmap((void *)(uintptr_t)hint, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   flags, -1, 0);
    if (p == MAP_FAILED) {
#ifdef MAP_32BIT
        p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
        p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    }
    if (p == MAP_FAILED) return NULL;
    if ((uintptr_t)p + ARENA_SIZE > 0x100000000ull) {
        (void)munmap(p, ARENA_SIZE);
        return NULL;
    }
    return (uint8_t *)p;
#endif
}

static void arena_unmap(uint8_t *arena)
{
    if (!arena) return;
#ifdef _WIN32
    (void)VirtualFree(arena, 0, MEM_RELEASE);
#else
    (void)munmap(arena, ARENA_SIZE);
#endif
}

/*
 * Export directory at RVA 0x1000:
 *   ordinal 1 -> RVA 0x2000, name "Alpha"
 *   ordinal 2 -> RVA 0x3000, name "Beta"
 *   ordinal 3 -> RVA 0x4000, no name
 */
static Wc3PeImage make_export_image(uint32_t hint, uint8_t **arena_out)
{
    uint8_t *arena = arena_map(hint);
    Wc3PeImage image;
    memset(&image, 0, sizeof image);
    if (!arena) return image;

    pe_export_dir dir;
    memset(&dir, 0, sizeof dir);
    dir.Base = 1;
    dir.NumberOfFunctions = 3;
    dir.NumberOfNames = 2;
    dir.AddressOfFunctions = 0x1040;
    dir.AddressOfNames = 0x1050;
    dir.AddressOfNameOrdinals = 0x1060;
    memcpy(arena + 0x1000, &dir, sizeof dir);

    put32(arena, 0x1040, 0x2000);
    put32(arena, 0x1044, 0x3000);
    put32(arena, 0x1048, 0x4000);
    put32(arena, 0x1050, 0x1080);
    put32(arena, 0x1054, 0x1090);
    put16(arena, 0x1060, 0);
    put16(arena, 0x1062, 1);
    memcpy(arena + 0x1080, "Alpha", 6);
    memcpy(arena + 0x1090, "Beta", 5);

    image.base = (uint32_t)(uintptr_t)arena;
    image.image_base = image.base;
    image.span = ARENA_SIZE;
    image.export_rva = 0x1000;
    image.export_size = 0x100;
    image.stamp = 0x12345678u;
    image.path = "synthetic-export";
    *arena_out = arena;
    return image;
}

/*
 * Import descriptor at RVA 0x1100, INT 0x1200, DLL name 0x1300, IAT 0x1400.
 * Exactly one import followed by a zero terminator.
 */
static Wc3PeImage make_import_image(uint32_t hint, const char *dll,
                                    const char *name, uint16_t ordinal,
                                    uint8_t **arena_out)
{
    uint8_t *arena = arena_map(hint);
    Wc3PeImage image;
    memset(&image, 0, sizeof image);
    if (!arena) return image;

    pe_import_descriptor descriptor;
    memset(&descriptor, 0, sizeof descriptor);
    descriptor.OriginalFirstThunk = 0x1200;
    descriptor.Name = 0x1300;
    descriptor.FirstThunk = 0x1400;
    memcpy(arena + 0x1100, &descriptor, sizeof descriptor);

    memcpy(arena + 0x1300, dll, strlen(dll) + 1);
    if (name) {
        put32(arena, 0x1200, 0x1500);
        put16(arena, 0x1500, 0);
        memcpy(arena + 0x1502, name, strlen(name) + 1);
    } else {
        put32(arena, 0x1200, 0x80000000u | (uint32_t)ordinal);
    }
    put32(arena, 0x1204, 0);

    image.base = (uint32_t)(uintptr_t)arena;
    image.image_base = image.base;
    image.span = ARENA_SIZE;
    image.import_rva = 0x1100;
    image.path = "synthetic-import";
    *arena_out = arena;
    return image;
}

static int hle_calls;
static char hle_last_dll[64];
static char hle_last_name[64];
static uint16_t hle_last_ordinal;

static uint32_t fake_hle(void *user, const char *dll, const char *name,
                         uint16_t ordinal)
{
    (void)user;
    hle_calls++;
    if (dll) {
        strncpy(hle_last_dll, dll, sizeof hle_last_dll - 1u);
        hle_last_dll[sizeof hle_last_dll - 1u] = '\0';
    }
    if (name) {
        strncpy(hle_last_name, name, sizeof hle_last_name - 1u);
        hle_last_name[sizeof hle_last_name - 1u] = '\0';
    } else {
        hle_last_name[0] = '\0';
    }
    hle_last_ordinal = ordinal;
    return 0x7F000000u + (uint32_t)hle_calls * 0x10u;
}

static void test_exports(void)
{
    uint8_t *arena = NULL;
    Wc3PeImage image = make_export_image(0x50000000u, &arena);
    check(arena != NULL, "export arena mapped");
    if (!arena) return;

    check(wc3_pe_export(&image, "Alpha") == image.base + 0x2000u,
          "export by name Alpha");
    check(wc3_pe_export(&image, "Beta") == image.base + 0x3000u,
          "export by name Beta");
    check(wc3_pe_export(&image, "Gamma") == 0u, "missing export");
    check(wc3_pe_export_ordinal(&image, 1) == image.base + 0x2000u,
          "export ordinal 1");
    check(wc3_pe_export_ordinal(&image, 2) == image.base + 0x3000u,
          "export ordinal 2");
    check(wc3_pe_export_ordinal(&image, 3) == image.base + 0x4000u,
          "unnamed export ordinal 3");
    check(wc3_pe_export_ordinal(&image, 4) == 0u, "missing ordinal");

    Wc3ModuleRegistry registry;
    wc3_module_registry_init(&registry);
    check(registry.count == 0u, "registry starts empty");
    check(wc3_module_add(&registry, "One.dll", &image) == 0,
          "register One.dll");
    check(wc3_module_add(&registry, "ONE.DLL", &image) == -1,
          "duplicate is rejected");
    check(registry.count == 1u, "duplicate did not append");

    check(wc3_module_find(&registry, "One.dll") != NULL, "find exact name");
    check(wc3_module_find(&registry, "C:\\Games\\ONE.DLL") != NULL,
          "find path and case insensitive");
    check(wc3_module_find(&registry, "Two.dll") == NULL, "find absent");

    check(wc3_module_lookup(&registry, "one.dll", "Alpha", 0) ==
              image.base + 0x2000u,
          "lookup by name");
    check(wc3_module_lookup(&registry, "One.dll", NULL, 2) ==
              image.base + 0x3000u,
          "lookup by ordinal");
    check(wc3_module_lookup(&registry, "Two.dll", "Alpha", 0) == 0u,
          "lookup absent module");

    arena_unmap(arena);
}

static void test_binding(void)
{
    uint8_t *export_arena = NULL;
    uint8_t *import_arena = NULL;
    Wc3PeImage exports = make_export_image(0x51000000u, &export_arena);
    check(export_arena != NULL, "binding export arena mapped");
    if (!export_arena) return;

    Wc3ModuleRegistry registry;
    wc3_module_registry_init(&registry);
    check(wc3_module_add(&registry, "One.dll", &exports) == 0,
          "binding register One.dll");

    Wc3BindStats stats;
    wc3_module_stats_init(&stats);

    /* Name import served by the lifted module. */
    Wc3PeImage import_image =
        make_import_image(0x52000000u, "One.dll", "Alpha", 0, &import_arena);
    check(import_arena != NULL, "name import arena mapped");
    if (!import_arena) {
        arena_unmap(export_arena);
        return;
    }
    hle_calls = 0;
    check(wc3_module_bind(&registry, &import_image, fake_hle, NULL, &stats) == 0,
          "bind name import");
    check(stats.bound_module == 1u, "name import counted as module");
    check(stats.bound_hle == 0u, "name import did not reach HLE");
    check(stats.missing == 0u && stats.export_missing == 0u,
          "name import fully resolved");
    check(get32(import_arena, 0x1400) == exports.base + 0x2000u,
          "IAT points at lifted export");
    check(hle_calls == 0, "HLE resolver untouched by a lifted import");

    /* Ordinal import, including a path-qualified DLL name. */
    uint8_t *ordinal_arena = NULL;
    Wc3PeImage ordinal_image = make_import_image(
        0x53000000u, "C:\\Games\\ONE.DLL", NULL, 2, &ordinal_arena);
    check(ordinal_arena != NULL, "ordinal import arena mapped");
    if (ordinal_arena) {
        check(wc3_module_bind(&registry, &ordinal_image, fake_hle, NULL,
                              &stats) == 0,
              "bind ordinal import");
        check(stats.bound_module == 2u, "ordinal import counted as module");
        check(get32(ordinal_arena, 0x1400) == exports.base + 0x3000u,
              "IAT points at ordinal export");
    }

    /* OS DLL: no loaded module, so HLE owns the thunk. */
    uint8_t *hle_arena = NULL;
    Wc3PeImage hle_image =
        make_import_image(0x54000000u, "KERNEL32.dll", "Sleep", 0, &hle_arena);
    check(hle_arena != NULL, "HLE import arena mapped");
    if (hle_arena) {
        hle_calls = 0;
        check(wc3_module_bind(&registry, &hle_image, fake_hle, NULL, &stats) ==
                  0,
              "bind HLE import");
        check(stats.bound_hle == 1u, "HLE import counted");
        check(get32(hle_arena, 0x1400) == 0x7F000010u,
              "IAT points at HLE thunk");
        check(strcmp(hle_last_dll, "KERNEL32.dll") == 0,
              "HLE saw DLL name");
        check(strcmp(hle_last_name, "Sleep") == 0, "HLE saw symbol name");
    }

    /* A loaded module with a missing export is a lift defect.  The legacy
     * boot path falls back to HLE, so this path must do the same while
     * counting the mismatch. */
    uint8_t *mismatch_arena = NULL;
    Wc3PeImage mismatch_image =
        make_import_image(0x55000000u, "One.dll", "Gamma", 0, &mismatch_arena);
    check(mismatch_arena != NULL, "export-mismatch arena mapped");
    if (mismatch_arena) {
        check(wc3_module_bind(&registry, &mismatch_image, fake_hle, NULL,
                              &stats) == 0,
              "missing lifted export falls back to HLE");
        check(stats.export_missing == 1u, "export mismatch counted");
        check(stats.missing == 0u, "export mismatch is not unresolved");
#if WC3_DEBUG
        check(strcmp(stats.first_export_missing_dll, "One.dll") == 0,
              "first export mismatch DLL recorded");
        check(strcmp(stats.first_export_missing_name, "Gamma") == 0,
              "first export mismatch symbol recorded");
#endif
    }

    /* No module and no HLE implementation: unresolved, never invented. */
    uint8_t *missing_arena = NULL;
    Wc3PeImage missing_image =
        make_import_image(0x56000000u, "Three.dll", "Delta", 0, &missing_arena);
    check(missing_arena != NULL, "unresolved import arena mapped");
    if (missing_arena) {
        check(wc3_module_bind(&registry, &missing_image, NULL, NULL, &stats) ==
                  1,
              "unresolved import reported");
        check(stats.missing == 1u, "missing import counted");
        check(get32(missing_arena, 0x1400) == 0u,
              "unresolved IAT stays zero");
#if WC3_DEBUG
        check(strcmp(stats.first_missing_dll, "Three.dll") == 0,
              "first missing DLL recorded");
        check(strcmp(stats.first_missing_name, "Delta") == 0,
              "first missing symbol recorded");
#endif
    }

    /* bind_all over the catalog repeats the module pass without touching the
     * synthetic import images (they are not registered). */
    check(wc3_module_bind_all(&registry, fake_hle, NULL, &stats) == 0,
          "bind all over registry");

    arena_unmap(missing_arena);
    arena_unmap(mismatch_arena);
    arena_unmap(hle_arena);
    arena_unmap(ordinal_arena);
    arena_unmap(import_arena);
    arena_unmap(export_arena);
}

int main(void)
{
    test_exports();
    test_binding();
    if (fails) {
        printf("module registry: %d failure(s)\n", fails);
        return 1;
    }
    printf("module registry: OK\n");
    return 0;
}
