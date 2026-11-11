/* ccache_wrapper.c - normalize the compiler path for MSYS2 ccache (BP-12).
 *
 * CMake writes the NDK compiler as E:\Android\...\clang.exe.  MSYS2 ccache
 * treats backslashes as part of one executable name and fails with
 * "Could not find compiler ... in PATH".  This wrapper changes only the path
 * separators and then runs ccache with the original argument vector.
 *
 * Usage: ccache_wrapper <ccache> <compiler> [args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void path_slash(char *s)
{
    for (; *s; s++) {
        if (*s == '\\') *s = '/';
    }
}

#if defined(_WIN32)
/*
 * ccache 4.14.1 on Windows creates the compiler suspended when it does not
 * detect a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.  It then
 * assigns the child to its own job and resumes it.  That path leaves the
 * preprocessor suspended under the Android build shells used here, so the
 * compiler never starts.  Put this launcher (and therefore ccache and its
 * compiler children) in a kill-on-close job before exec'ing ccache.  ccache
 * then takes its no-suspend path and the compiler runs normally.  A job that
 * already has the flag is left untouched; Windows 8+ permits the nested job
 * fallback used when the caller supplied a job without the flag.
 */
static HANDLE wrapper_job;

static void ensure_job(void)
{
    BOOL in_job = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job)) return;
    if (in_job) {
        memset(&info, 0, sizeof(info));
        if (QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation,
                                      &info, sizeof(info), NULL) &&
            (info.BasicLimitInformation.LimitFlags &
             JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)) {
            return;
        }
    }

    wrapper_job = CreateJobObjectA(NULL, NULL);
    if (!wrapper_job) return;
    memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(wrapper_job, JobObjectExtendedLimitInformation,
                                 &info, sizeof(info)) ||
        !AssignProcessToJobObject(wrapper_job, GetCurrentProcess())) {
        CloseHandle(wrapper_job);
        wrapper_job = NULL;
    }
}

static int job_is_kill_on_close(void)
{
    BOOL in_job = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job) || !in_job) return 0;
    memset(&info, 0, sizeof(info));
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation,
                                   &info, sizeof(info), NULL)) {
        return 0;
    }
    return (info.BasicLimitInformation.LimitFlags &
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}
#endif

int main(int argc, char **argv)
{
    char *ccache;
    char *compiler;
    char **args;
    int i;
    int rc;

#if defined(_WIN32)
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        ensure_job();
        if (!job_is_kill_on_close()) {
            fprintf(stderr,
                    "ccache_wrapper self-test: no kill-on-close job\n");
            return 1;
        }
        printf("ccache_wrapper self-test: OK\n");
        return 0;
    }
#endif

    if (argc < 3) {
        fprintf(stderr,
                "usage: ccache_wrapper <ccache> <compiler> [args...]\n");
        return 2;
    }

#if defined(_WIN32)
    ensure_job();
#endif

    ccache = dup_str(argv[1]);
    compiler = dup_str(argv[2]);
    args = (char **)calloc((size_t)argc, sizeof(*args));
    if (!ccache || !compiler || !args) {
        free(ccache);
        free(compiler);
        free(args);
        fprintf(stderr, "ccache_wrapper: out of memory\n");
        return 2;
    }

    path_slash(ccache);
    path_slash(compiler);
    args[0] = ccache;
    args[1] = compiler;
    for (i = 3; i < argc; i++) args[i - 1] = argv[i];
    args[argc - 1] = NULL;

#if defined(_WIN32)
    rc = (int)_spawnv(_P_WAIT, ccache, (const char *const *)args);
    if (rc == -1) {
        perror("ccache_wrapper");
        rc = 127;
    }
#else
    execv(ccache, args);
    perror("ccache_wrapper");
    rc = 127;
#endif

    free(args);
    free(compiler);
    free(ccache);
#if defined(_WIN32)
    if (wrapper_job) CloseHandle(wrapper_job);
#endif
    return rc;
}
