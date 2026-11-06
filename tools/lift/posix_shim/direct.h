/* direct.h - POSIX stand-in for the MSVC/MinGW header used by expand.c.
 *
 * expand.c calls `_mkdir(dir)` (one argument).  The pinned source is not
 * edited; the shim is only visible during the POSIX syntax check.
 * On a Windows toolchain mkdir() takes one argument (UCRT), on real POSIX
 * it takes two.  WC3_FORCE_POSIX selects the one-argument form there.
 */
#ifndef WC3_POSIX_DIRECT_H
#define WC3_POSIX_DIRECT_H

#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32) && defined(WC3_FORCE_POSIX)
static inline int _mkdir(const char *path) { return mkdir(path); }
#elif defined(_WIN32)
/* real Windows build never includes this shim */
#else
static inline int _mkdir(const char *path) { return mkdir(path, 0775); }
#endif

#endif /* WC3_POSIX_DIRECT_H */
