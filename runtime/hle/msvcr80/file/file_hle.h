/*
 * file_hle.h - target-side MSVCR80 low-level file descriptors.
 *
 * The as-built table has no _open/_read/_write/_lseeki64/_close rows, so the
 * resolver leaves soft stubs in their place.  This slice backs them with
 * Wc3FileApi and the same path translation the as-built host used: "Z:\a\b"
 * becomes "/a/b", "C:\a\b" becomes the relative "a/b", and relative paths stay
 * relative to the process working directory (the game data directory).
 *
 * Text-mode CRLF translation is not performed; the target is byte-oriented,
 * matching the as-built Android file path.  _open mode bits are ignored because
 * Wc3FileApi does not carry permissions.
 */
#ifndef WC3_HLE_MSVCR80_FILE_H
#define WC3_HLE_MSVCR80_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the low-level descriptor rows.  Returns the number of rows
 * installed, or -1 when the registry rejects one. */
int wc3_hle_msvcr80_file_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_FILE_H */
