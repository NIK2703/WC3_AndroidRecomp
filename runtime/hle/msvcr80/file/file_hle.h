/*
 * file_hle.h - target-side MSVCR80 low-level file descriptors.
 *
 * The as-built table has no _open/_read/_write/_lseeki64/_close or
 * _stat64i32/_stat64/_fstat64/_fstat64i32 rows, so the resolver leaves soft
 * stubs in their place.  This slice backs them with Wc3FileApi and the same
 * path translation the as-built host used: "Z:\a\b" becomes "/a/b",
 * "C:\a\b" becomes the relative "a/b", and relative paths stay relative to
 * the process working directory (the game data directory).
 *
 * Text-mode CRLF translation is not performed; the target is byte-oriented,
 * matching the as-built Android file path.  _open mode bits are ignored because
 * Wc3FileApi does not carry permissions.
 *
 * The descriptor helpers below are also the storage behind the target stdio
 * streams: fopen() allocates one of these descriptors, so _fileno() returns a
 * number that _read/_write/_lseeki64/_close can use on the same handle.
 */
#ifndef WC3_HLE_MSVCR80_FILE_H
#define WC3_HLE_MSVCR80_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the low-level descriptor rows.  Returns the number of rows
 * installed, or -1 when the registry rejects one. */
int wc3_hle_msvcr80_file_install(void);

/* Read a bounded guest C string.  Returns 1 when the terminator was found,
 * 0 for an unmapped range or a missing terminator. */
int wc3_hle_msvcr80_read_guest_path(uint32_t address, char *buffer,
                                    size_t capacity);

/* Translate a guest path through the as-built Z:/C:/relative rules. */
void wc3_hle_msvcr80_map_path(const char *input, char *output, size_t capacity);

/* Descriptor operations shared with the stdio slice.  `fd` is a guest-visible
 * descriptor returned by fd_open or _open.  Each function returns 0 on success
 * and -1 on failure; fd_read/fd_write report byte counts. */
int wc3_hle_msvcr80_fd_open(const char *host_path, int flags, int *fd_out);
int wc3_hle_msvcr80_fd_read(int fd, void *buffer, uint32_t count,
                            uint32_t *read_out);
int wc3_hle_msvcr80_fd_write(int fd, const void *buffer, uint32_t count,
                             uint32_t *written_out);
int wc3_hle_msvcr80_fd_seek(int fd, int64_t offset, int whence,
                            uint64_t *position_out);
int wc3_hle_msvcr80_fd_size(int fd, uint64_t *size_out);
int wc3_hle_msvcr80_fd_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_FILE_H */
