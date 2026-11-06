/* dirwalk.h - portable directory walking for host-side build tools.
 *
 * The pinned upstream tools use FindFirstFileA directly.  Tools written in
 * this tree must also compile on a Linux CI host (BP-8.3), so the walk is
 * isolated here instead of being copied into every translation unit.
 */
#ifndef DIRWALK_H
#define DIRWALK_H

#include <stddef.h>

#define DIR_MAX_PATH 4096

/* Visitor returns 0 to continue, non-zero to abort the walk. */
typedef int (*dir_visitor)(const char *path, void *user);

/* Recursively visits regular files under root.  Returns 0 on success. */
int dir_walk(const char *root, dir_visitor visit, void *user);
/* Normalise '\\' to '/' in place. */
void path_slash(char *path);
/* Suffix test, case-sensitive. */
int path_ends_with(const char *path, const char *suffix);
/* Returns 1 when path is a directory. */
int path_is_dir(const char *path);

#endif /* DIRWALK_H */
