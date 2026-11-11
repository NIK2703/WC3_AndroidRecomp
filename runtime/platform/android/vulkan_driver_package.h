/*
 * vulkan_driver_package.h - ADPkg validation and app-private registry.
 *
 * This module has no Android dependency.  The Kotlin installer owns the ZIP
 * container, JSON and SHA-256; this module is the loader-side authority for
 * the extracted directory: driver.ini, the ELF header and the registry path.
 *
 * Product-visible numeric codes are 4xxx.  They are not debug diagnostics and
 * stay in release builds.  A non-zero code is a startup/diagnostic refusal,
 * never a silent substitution of a different driver.
 */
#ifndef WC3_VULKAN_DRIVER_PACKAGE_H
#define WC3_VULKAN_DRIVER_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC3_DRIVER_ID_MAX 32
#define WC3_DRIVER_LIBRARY_MAX 128
#define WC3_DRIVER_DIR_MAX 1024

/* Package/loader failures.  Keep 4001..4009 reserved for this feature. */
enum {
    WC3_DRIVER_OK = 0,
    WC3_DRIVER_ERR_ID = 4001,
    WC3_DRIVER_ERR_PATH = 4002,
    WC3_DRIVER_ERR_INI = 4003,
    WC3_DRIVER_ERR_ELF = 4004,
    WC3_DRIVER_ERR_IO = 4005,
    WC3_DRIVER_ERR_LOAD = 4006,
    WC3_DRIVER_ERR_PROBE = 4007,
    WC3_DRIVER_ERR_API = 4008,
    WC3_DRIVER_ERR_CRASH_LOOP = 4009
};

typedef struct Wc3DriverPackage {
    char id[WC3_DRIVER_ID_MAX];
    char library[WC3_DRIVER_LIBRARY_MAX];
    uint32_t min_api;
    uint32_t size;
    uint64_t installed_at;
    int page16k;
} Wc3DriverPackage;

/* Exactly 16 lowercase hex characters: the driver id is the first 16 hex of
 * the main library SHA-256 (DRV-10).  This also makes the id a safe single
 * path component. */
int wc3_driver_id_valid(const char *id);

/* Validate one already-extracted directory.  Requires .installed, driver.ini
 * and the main library.  Rejects symlinked directories/files and a library
 * name containing a path separator or traversal.  On success fills out and
 * lib_path (root + "/" + library). */
int wc3_driver_check_dir(const char *dir, Wc3DriverPackage *out,
                         char *lib_path, size_t lib_path_cap);

/* Registry lookup: root/<id>, with out->id required to match id. */
int wc3_driver_registry_get(const char *root, const char *id,
                            Wc3DriverPackage *out,
                            char *lib_path, size_t lib_path_cap);

/* ELF header check used by wc3_driver_check_dir and by the install JNI. */
int wc3_driver_elf_check(const char *path, uint32_t *size, int *page16k);

#ifdef __cplusplus
}
#endif

#endif /* WC3_VULKAN_DRIVER_PACKAGE_H */
