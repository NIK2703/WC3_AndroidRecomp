/*
 * PGO training writer for the release_optimized profile-generation build.
 *
 * Android force-stop does not run atexit handlers, so the normal LLVM profile
 * runtime shutdown path cannot be relied on for a device training run.  This
 * file is compiled only when WC3_PGO_MODE=generate and is never part of the
 * release or final profile-use product.  The writer takes one snapshot after
 * startup has exercised the boot path; the resulting .profraw files are pulled
 * and merged into the .profdata consumed by the final ThinLTO build.
 */
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

extern int __llvm_profile_write_file(void);

static void *wc3_pgo_writer(void *unused)
{
    (void)unused;
    sleep(20);
    (void)__llvm_profile_write_file();
    return NULL;
}

__attribute__((constructor))
static void wc3_pgo_start_writer(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, wc3_pgo_writer, NULL) == 0)
        (void)pthread_detach(thread);
}
