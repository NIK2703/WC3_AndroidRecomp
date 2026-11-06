/* dirwalk.c - Win32 / POSIX directory walking. */
#include "dirwalk.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

int path_is_dir(const char *path)
{
#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void path_slash(char *path)
{
    char *p;
    for (p = path; *p; p++)
        if (*p == '\\') *p = '/';
}

int path_ends_with(const char *path, const char *suffix)
{
    size_t a = strlen(path), b = strlen(suffix);
    return a >= b && strcmp(path + a - b, suffix) == 0;
}

#if defined(_WIN32) && !defined(WC3_FORCE_POSIX)

static int walk_dir(const char *dir, dir_visitor visit, void *user)
{
    WIN32_FIND_DATAA ent;
    HANDLE h;
    char pattern[DIR_MAX_PATH];
    int rc = 0;

    if (snprintf(pattern, sizeof pattern, "%s/*", dir) >= (int)sizeof pattern)
        return -1;
    h = FindFirstFileA(pattern, &ent);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char child[DIR_MAX_PATH];
        if (strcmp(ent.cFileName, ".") == 0 || strcmp(ent.cFileName, "..") == 0)
            continue;
        if (snprintf(child, sizeof child, "%s/%s", dir, ent.cFileName)
            >= (int)sizeof child) { rc = -1; break; }
        path_slash(child);
        if (ent.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rc = walk_dir(child, visit, user);
        } else {
            rc = visit(child, user);
        }
    } while (rc == 0 && FindNextFileA(h, &ent));
    FindClose(h);
    return rc;
}

#else

static int walk_dir(const char *dir, dir_visitor visit, void *user)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int rc = 0;

    if (!d) return 0;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        char child[DIR_MAX_PATH];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof child, "%s/%s", dir, de->d_name)
            >= (int)sizeof child) { rc = -1; break; }
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            rc = walk_dir(child, visit, user);
        else if (S_ISREG(st.st_mode))
            rc = visit(child, user);
    }
    closedir(d);
    return rc;
}

#endif

int dir_walk(const char *root, dir_visitor visit, void *user)
{
    char copy[DIR_MAX_PATH];
    size_t n;

    if (snprintf(copy, sizeof copy, "%s", root) >= (int)sizeof copy)
        return -1;
    path_slash(copy);
    n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = 0;
    return walk_dir(copy, visit, user);
}
