/*
 * PGO training writer for the release_optimized profile-generation build.
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
 * -fprofile-instr-generate=<path> value: the Windows build is driven through
 * MSYS2, which rewrites an absolute /data/... argument into a C:/msys64/...
 * path before clang embeds it.  __llvm_profile_set_filename() takes the POSIX
 * string from this source file, so the device sees the intended path.
 */
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

extern int __llvm_profile_write_file(void);
extern void __llvm_profile_set_filename(const char *name);

#ifndef WC3_PGO_DIR
#define WC3_PGO_DIR "/data/data/wc3.recomp/files/pgo"
#endif

#define WC3_PGO_INTERVAL_SEC 30

static char g_profile_path[2][256];

static void *wc3_pgo_writer(void *unused)
{
    unsigned slot = 0;
    (void)unused;
    for (;;) {
        int rc;
        sleep(WC3_PGO_INTERVAL_SEC);
        __llvm_profile_set_filename(g_profile_path[slot]);
        errno = 0;
        rc = __llvm_profile_write_file();
        __android_log_print(ANDROID_LOG_INFO, "wc3-pgo",
                            "profile %s rc=%d errno=%d",
                            g_profile_path[slot], rc, errno);
        slot ^= 1;
    }
    return NULL;
}

__attribute__((constructor))
static void wc3_pgo_start_writer(void)
{
    pthread_t thread;
    if (mkdir(WC3_PGO_DIR, 0700) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_WARN, "wc3-pgo",
                            "mkdir %s failed errno=%d", WC3_PGO_DIR, errno);
    }
    snprintf(g_profile_path[0], sizeof(g_profile_path[0]),
             "%s/wc3-%d-a.profraw", WC3_PGO_DIR, (int)getpid());
    snprintf(g_profile_path[1], sizeof(g_profile_path[1]),
             "%s/wc3-%d-b.profraw", WC3_PGO_DIR, (int)getpid());
    __llvm_profile_set_filename(g_profile_path[0]);
    if (pthread_create(&thread, NULL, wc3_pgo_writer, NULL) == 0)
        (void)pthread_detach(thread);
}
