/*
 * asset_snapshot_test.c - T0 test for the O-45 name snapshot.
 *
 * The device platform test owns the AssetApi semantics (case-insensitive
 * present, enumerate fallback, retry after a missing root).  This host test
 * isolates the storage contract: growth by observed count, the 1024-entry
 * truncation limit, release on rebuild and the open-failure marker.
 */
#include "asset_snapshot.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#define MKDIR(path) mkdir((path), 0700)
#define RMDIR(path) rmdir(path)
#endif

#define TEST_DIR "build/host/asset_snapshot_tmp"

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "FAIL: %s\n", message);                       \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int create_file(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return 0;
    return close(fd) == 0;
}

static void clear_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        char path[512];
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            int n = snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
            if (n > 0 && (size_t)n < sizeof path) (void)remove(path);
        }
        (void)closedir(d);
    }
    (void)RMDIR(dir);
}

static int make_tree(const char *dir)
{
    if (MKDIR(dir) == 0) return 1;
    return errno == EEXIST;
}

static int has_name(const Wc3AssetSnapshot *s, const char *name)
{
    for (uint32_t i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) return 1;
    }
    return 0;
}

static int create_many(const char *dir, uint32_t count)
{
    char path[512];
    for (uint32_t i = 0; i < count; i++) {
        int n = snprintf(path, sizeof path, "%s/f%04u.mpq", dir, i);
        if (n <= 0 || (size_t)n >= sizeof path) return 0;
        if (!create_file(path)) return 0;
    }
    return 1;
}

int main(void)
{
    clear_tree(TEST_DIR);
    CHECK(make_tree(TEST_DIR), "create test directory");

    CHECK(create_file(TEST_DIR "/War3.mpq"), "create War3.mpq");
    CHECK(create_file(TEST_DIR "/war3patch.mpq"), "create war3patch.mpq");
    CHECK(create_file(TEST_DIR "/readme.txt"), "create readme.txt");
    CHECK(create_file(TEST_DIR "/notes.MPQ"), "create notes.MPQ");

    Wc3AssetSnapshot s;
    memset(&s, 0, sizeof s);
    wc3_asset_snapshot_build(&s, TEST_DIR);
    CHECK(s.open_failed == 0u, "snapshot opens directory");
    CHECK(s.truncated == 0u, "small snapshot is not truncated");
    CHECK(s.count == 4u, "snapshot count follows directory entries");
    CHECK(s.capacity >= s.count && s.capacity < WC3_ASSET_SNAPSHOT_MAX,
          "snapshot does not reserve the full 1024-entry table");
    CHECK(has_name(&s, "War3.mpq"), "snapshot keeps War3.mpq");
    CHECK(has_name(&s, "war3patch.mpq"), "snapshot keeps war3patch.mpq");
    CHECK(has_name(&s, "readme.txt"), "snapshot keeps non-MPQ names");
    CHECK(has_name(&s, "notes.MPQ"), "snapshot keeps case-varied names");

    /* A missing directory is reported, not cached as a valid empty snapshot. */
    clear_tree(TEST_DIR);
    wc3_asset_snapshot_build(&s, TEST_DIR);
    CHECK(s.open_failed == 1u, "missing directory reports open failure");
    CHECK(s.count == 0u, "missing directory has no names");
    CHECK(s.names == NULL, "missing directory releases the name table");
    CHECK(s.capacity == 0u, "missing directory resets capacity");

    /* Rebuild after the directory appears; this is the retry contract the
     * platform exercises on the next lookup. */
    CHECK(make_tree(TEST_DIR), "recreate test directory");
    CHECK(create_file(TEST_DIR "/War3.mpq"), "recreate War3.mpq");
    wc3_asset_snapshot_build(&s, TEST_DIR);
    CHECK(s.open_failed == 0u, "retry opens directory");
    CHECK(s.count == 1u, "retry sees the new entry");
    CHECK(has_name(&s, "War3.mpq"), "retry keeps the new entry");

    /* Cross the 64-entry growth step. */
    clear_tree(TEST_DIR);
    CHECK(make_tree(TEST_DIR), "create growth directory");
    CHECK(create_many(TEST_DIR, 70u), "create 70 entries");
    wc3_asset_snapshot_build(&s, TEST_DIR);
    CHECK(s.open_failed == 0u, "growth snapshot opens");
    CHECK(s.truncated == 0u, "growth snapshot is complete");
    CHECK(s.count == 70u, "growth snapshot count");
    CHECK(s.capacity >= s.count && s.capacity < WC3_ASSET_SNAPSHOT_MAX,
          "growth snapshot does not reserve the full table");

    /* Cross the retained 1024-entry cap.  The platform falls back to a live
     * scan when this flag is set, so the snapshot may stop early. */
    clear_tree(TEST_DIR);
    CHECK(make_tree(TEST_DIR), "create truncation directory");
    CHECK(create_many(TEST_DIR, 1100u), "create 1100 entries");
    wc3_asset_snapshot_build(&s, TEST_DIR);
    CHECK(s.open_failed == 0u, "truncation snapshot opens");
    CHECK(s.truncated == 1u, "oversized directory is truncated");
    CHECK(s.count == WC3_ASSET_SNAPSHOT_MAX, "truncation keeps the cap");

    clear_tree(TEST_DIR);
    printf("asset snapshot OK\n");
    return 0;
}
