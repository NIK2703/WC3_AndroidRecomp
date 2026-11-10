/*
 * kernel32_file_hle.h - bounded target-side extension of the file HLE.
 *
 * O-26/O-27 stay on the M5 seam instead of editing the as-built monolith
 * directly: this module chains the as-built CreateFileA,
 * GetFileAttributesA and SetCurrentDirectoryA rows, keeps the as-built handle
 * table and read/write semantics, and only changes the path spelling and the
 * host read-ahead hint.
 *
 * The path cache is deliberately small and live-checked: the exact requested
 * spelling wins whenever access(host) succeeds, and a cached case-variant
 * spelling is used only while access(host) fails and access(exact) succeeds.
 * An exact-spelling hit is passed through for the as-built live access.  A
 * directory change flushes the cache, and a CreateFileA that may create or
 * modify a file invalidates its entry.  This keeps the FileApi contract
 * observable through the same access(F_OK) the as-built code uses.
 */
#ifndef WC3_HLE_WIN32_KERNEL32_FILE_H
#define WC3_HLE_WIN32_KERNEL32_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the chained rows.  Returns the number of rows installed, or -1 when
 * a mandatory as-built row is missing. */
int wc3_hle_kernel32_file_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_WIN32_KERNEL32_FILE_H */
