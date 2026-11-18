/* windows.h - POSIX shim for the Win32 subset used by the pinned upstream
 * tools in tools/lift/csrc (BP-8.3, M8 POSIX portability).
 *
 * The sources in csrc are byte-pinned by config/build_upstream.json, so they
 * cannot be edited.  Instead, this directory is put FIRST in the include path
 * only for the POSIX syntax/compat check; the real Windows build never sees
 * it.  WC3_FORCE_POSIX selects this branch even on a Windows GCC toolchain,
 * which is how the check runs on this machine.
 *
 * Two modes:
 *   - real POSIX (!_WIN32): dlfcn.h / readlink / usleep are used, so the
 *     tools can actually be built and run on a Linux CI host;
 *   - WC3_FORCE_POSIX on a Windows toolchain: dlfcn.h does not exist there,
 *     so dlopen/dlsym are declared (not called) and the check is
 *     `gcc -fsyntax-only` only.  Type/signature compatibility is what is
 *     being proved; execution stays on the real Windows binaries.
 *
 * Scope is deliberately narrow: the pinned tools enumerate directories with
 * "<dir>\\*" and load capstone dynamically.  Any other Win32 use must fail.
 */
#ifndef WC3_POSIX_WINDOWS_H
#define WC3_POSIX_WINDOWS_H

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#error "posix_shim/windows.h reached on a Windows build: only add -I for the POSIX check"
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

#if defined(_WIN32) && defined(WC3_FORCE_POSIX)
#  define WC3_SHIM_FAKE_DL 1
#endif

#ifdef WC3_SHIM_FAKE_DL
/* UCRT has no dlfcn.h.  Declarations are enough for -fsyntax-only. */
extern void *dlopen(const char *path, int flags);
extern void *dlsym(void *handle, const char *name);
extern int dlclose(void *handle);
#  define RTLD_NOW   2
#  define RTLD_LOCAL 0
#else
#  include <dlfcn.h>
#  include <strings.h>
#  include <unistd.h>
#endif

#ifndef __cdecl
#define __cdecl
#endif
#ifndef WINAPI
#define WINAPI
#endif

typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HMODULE;
typedef int (*FARPROC)(void);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFu)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u

typedef struct {
    DWORD dwFileAttributes;
    DWORD nFileSizeLow;
    DWORD nFileSizeHigh;
    char cFileName[MAX_PATH];
    /* POSIX-only bookkeeping; Win32 callers never look past cFileName. */
    char shim_dir[MAX_PATH];
} WIN32_FIND_DATAA;

/* --- CRT spellings ------------------------------------------------------- */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _getcwd getcwd

/* The pinned tools build some paths with Win32 separators even when the
 * corpus root came in as a POSIX path (closure_pipeline/corpusclosure pass
 * "a/b" and the callee appends "\\*").  POSIX treats the backslash literally,
 * so every path that reaches a syscall is normalised here. */
static inline size_t shim_norm_path(const char *path, char *out, size_t cap)
{
    size_t i;
    if (!path) path = "";
    for (i = 0; path[i] && i + 1 < cap; i++)
        out[i] = (path[i] == '\\') ? '/' : path[i];
    out[i] = 0;
    return i;
}

/* The pinned tools append "\\seed.c" to a directory argument that may itself
 * be POSIX-style, then call fopen().  Route every fopen through the same
 * normalisation; this is the POSIX host only, the Windows build never sees
 * this header. */
static inline FILE *shim_fopen(const char *path, const char *mode)
{
    char norm[MAX_PATH * 2];
    shim_norm_path(path, norm, sizeof norm);
    return fopen(norm, mode);
}
#define fopen(path, mode) shim_fopen((path), (mode))

/* --- directory enumeration ---------------------------------------------- */
/* Every pinned caller uses a trailing backslash-star (or slash-star), so
 * readdir is an exact equivalent.  A different pattern is not guessed. */
static inline size_t shim_pattern_dir(const char *pattern, char *out, size_t cap)
{
    char norm[MAX_PATH * 2];
    const char *slash = NULL, *p;
    size_t n;
    shim_norm_path(pattern, norm, sizeof norm);
    for (p = norm; *p; p++)
        if (*p == '/') slash = p;
    if (slash && (slash[1] == '*' || slash[1] == 0)) n = (size_t)(slash - norm);
    else n = strlen(norm);
    if (n == 0) { if (cap > 1) { out[0] = '.'; out[1] = 0; } return 1; }
    if (n >= cap) n = cap - 1;
    memcpy(out, norm, n);
    out[n] = 0;
    return n;
}

static inline BOOL FindClose(HANDLE h)
{
    return h ? closedir((DIR *)h) == 0 : FALSE;
}

static inline BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    DIR *d = (DIR *)h;
    struct dirent *de;
    struct stat st;
    char path[MAX_PATH * 2];
    if (!d || !fd) return FALSE;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", de->d_name);
        fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        fd->nFileSizeLow = fd->nFileSizeHigh = 0;
        /* stat is best-effort: an unreadable entry is still returned, as
         * FindFirstFileA does, with normal-file attributes. */
        snprintf(path, sizeof path, "%s/%s", fd->shim_dir, de->d_name);
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            fd->nFileSizeLow = (DWORD)st.st_size;
        }
        return TRUE;
    }
    return FALSE;
}

static inline HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd)
{
    HANDLE h;
    if (!pattern || !fd) return INVALID_HANDLE_VALUE;
    memset(fd, 0, sizeof *fd);
    shim_pattern_dir(pattern, fd->shim_dir, sizeof fd->shim_dir);
    h = (HANDLE)opendir(fd->shim_dir);
    if (!h) return INVALID_HANDLE_VALUE;
    if (!FindNextFileA(h, fd)) { FindClose(h); return INVALID_HANDLE_VALUE; }
    return h;
}

static inline DWORD GetFileAttributesA(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

/* lift.c cmd_split() creates its output directory this way. */
static inline BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    if (!path) return FALSE;
#ifdef WC3_SHIM_FAKE_DL
    return mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0775) == 0 || errno == EEXIST;
#endif
}

/* ledger.c db_save() writes a temp file and atomically replaces the ledger.
 * POSIX rename() already replaces an existing destination, which is exactly
 * MOVEFILE_REPLACE_EXISTING. */
#define MOVEFILE_REPLACE_EXISTING 0x00000001u
static inline BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    (void)flags;
    return (from && to && rename(from, to) == 0) ? TRUE : FALSE;
}

/* --- dynamic loading ----------------------------------------------------- */
static inline HMODULE LoadLibraryA(const char *path)
{
    char norm[MAX_PATH];
    const char *base;
    size_t i;
    void *h;
    if (!path) return NULL;
    for (i = 0; path[i] && i + 1 < sizeof norm; i++)
        norm[i] = (path[i] == '\\') ? '/' : path[i];
    norm[i] = 0;
    h = dlopen(norm, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        base = strrchr(norm, '/');
        base = base ? base + 1 : norm;
        h = dlopen(base, RTLD_NOW | RTLD_LOCAL);
    }
    /* capstone.dll on Windows, libcapstone.so on POSIX.  Try the sibling
     * libcapstone.so first so build/lift-tools works without LD_LIBRARY_PATH. */
    if (!h && strstr(norm, "capstone")) {
        char alt[MAX_PATH];
        if (base && base > norm) {
            snprintf(alt, sizeof alt, "%.*slibcapstone.so",
                     (int)(base - norm), norm);
            h = dlopen(alt, RTLD_NOW | RTLD_LOCAL);
        }
        if (!h)
            h = dlopen("libcapstone.so", RTLD_NOW | RTLD_LOCAL);
    }
    return (HMODULE)h;
}

static inline FARPROC GetProcAddress(HMODULE lib, const char *name)
{
    void *p = dlsym(lib, name);
    FARPROC fn = NULL;
    /* ISO C forbids void* -> function pointer; POSIX blesses this copy. */
    memcpy(&fn, &p, sizeof p > sizeof fn ? sizeof fn : sizeof p);
    return fn;
}

static inline BOOL FreeLibrary(HMODULE lib)
{
    return lib ? dlclose(lib) == 0 : FALSE;
}

static inline DWORD GetLastError(void) { return (DWORD)errno; }

static inline DWORD GetModuleFileNameA(HMODULE mod, char *out, DWORD cap)
{
    ssize_t n = -1;
    (void)mod;
    if (!out || !cap) return 0;
#if !defined(WC3_SHIM_FAKE_DL) && defined(__linux__)
    n = readlink("/proc/self/exe", out, (size_t)cap - 1);
#elif !defined(WC3_SHIM_FAKE_DL) && defined(__APPLE__)
    extern int _NSGetExecutablePath(char *, unsigned int *);
    unsigned int size = (unsigned int)cap;
    n = _NSGetExecutablePath(out, &size) == 0 ? (ssize_t)strlen(out) : -1;
#endif
    if (n <= 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return (DWORD)n;
}

#ifdef WC3_SHIM_FAKE_DL
static inline void Sleep(DWORD ms) { (void)ms; }
#else
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
#endif

#endif /* POSIX branch */
#endif /* WC3_POSIX_WINDOWS_H */
