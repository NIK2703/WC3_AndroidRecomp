/*
 * stdio_hle.h - target-side MSVCR80 stdio streams.
 *
 * The as-built CRT keeps host FILE* pointers in a private 64-slot table and
 * exposes opaque 32-byte guest records to the game.  The target keeps the same
 * record shape but backs file streams with Wc3FileApi descriptors, so _fileno
 * returns a descriptor that the low-level file slice can also use.  Standard
 * streams 0..2 stay connected to the host console.
 */
#ifndef WC3_HLE_MSVCR80_STDIO_H
#define WC3_HLE_MSVCR80_STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the stdio overrides and additions.  Returns the number of rows
 * installed, or -1 when the registry rejects one. */
int wc3_hle_msvcr80_stdio_install(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_HLE_MSVCR80_STDIO_H */
