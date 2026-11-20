/*
 * PGO training writer for the release profile-generation build.
 *
 * Android force-stop does not run atexit handlers, so the normal LLVM profile
 * runtime shutdown path cannot be relied on for a device training run.  This
 * file is compiled only when WC3_PGO_MODE=generate and is never part of the
 * release or final profile-use product.  The writer snapshots the counters
 * periodically so a long game session is covered and a force-stop loses at
 * most one interval; two slots are alternated so a kill during a write does
 * not destroy the previous complete snapshot.
 *
 * The filename is set at runtime instead of relying on the compile-time
 * -fprofile-instr-generate=<path> or -fprofile-generate=<dir> value: the
 * Windows build is driven through MSYS2, which rewrites an absolute /data/...
 * argument into a C:/msys64/... path before clang embeds it.
 * __llvm_profile_set_filename() takes the POSIX string from this source file,
 * so the device sees the intended path for both front-end PGO and IRPGO.
 */
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* CMake generates this header for the training build.  Do not pass the
 * device path as -DWC3_PGO_DIR: MSYS2 rewrites /data/... to C:/msys64/... and
 * strips the C-string quoting from the compiler argument. */
#if defined(__has_include)
# if __has_include("wc3_pgo_config.h")
#  include "wc3_pgo_config.h"
# endif
#endif

extern int __llvm_profile_write_file(void);
extern void __llvm_profile_set_filename(const char *name);

#ifndef WC3_PGO_DIR
#define WC3_PGO_DIR "/data/data/wc3.recomp/files/pgo"
#endif

#ifndef WC3_PGO_INTERVAL_SEC
#define WC3_PGO_INTERVAL_SEC 30
#endif

#define WC3_PGO_PATH_MAX 320

static char g_profile_path[2][WC3_PGO_PATH_MAX];
static char g_temp_path[2][WC3_PGO_PATH_MAX];

static void *wc3_pgo_writer(void *unused)
{
    unsigned slot = 0;
    (void)unused;
    for (;;) {
        int rc;
        int write_errno;
        int rename_rc = -1;
        int rename_errno = 0;

        sleep(WC3_PGO_INTERVAL_SEC);
        /* Write a complete snapshot beside the destination and rename it
         * into place.  A force-stop during the write then leaves either the
         * previous complete slot or this snapshot, never a truncated file
         * that llvm-profdata would reject. */
        __llvm_profile_set_filename(g_temp_path[slot]);
        errno = 0;
        rc = __llvm_profile_write_file();
        write_errno = errno;
        if (rc == 0) {
            rename_rc = rename(g_temp_path[slot], g_profile_path[slot]);
            rename_errno = (rename_rc == 0) ? 0 : errno;
            if (rename_rc != 0)
                (void)unlink(g_temp_path[slot]);
        } else {
            (void)unlink(g_temp_path[slot]);
        }
        __android_log_print(ANDROID_LOG_INFO, "wc3-pgo",
                            "profile %s rc=%d errno=%d rename_rc=%d "
                            "rename_errno=%d",
                            g_profile_path[slot], rc, write_errno,
                            rename_rc, rename_errno);
        slot ^= 1;
    }
    return NULL;
}

__attribute__((constructor))
static void wc3_pgo_start_writer(void)
{
    pthread_t thread;
    unsigned slot;

    if (mkdir(WC3_PGO_DIR, 0700) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_WARN, "wc3-pgo",
                            "mkdir %s failed errno=%d", WC3_PGO_DIR, errno);
    }
    for (slot = 0; slot < 2; ++slot) {
        snprintf(g_profile_path[slot], sizeof(g_profile_path[slot]),
                 "%s/wc3-%d-%c.profraw", WC3_PGO_DIR, (int)getpid(),
                 slot == 0 ? 'a' : 'b');
        snprintf(g_temp_path[slot], sizeof(g_temp_path[slot]),
                 "%s.tmp", g_profile_path[slot]);
        (void)unlink(g_temp_path[slot]);
    }
    __llvm_profile_set_filename(g_profile_path[0]);
    if (pthread_create(&thread, NULL, wc3_pgo_writer, NULL) == 0)
        (void)pthread_detach(thread);
}
