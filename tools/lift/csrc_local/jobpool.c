/* jobpool.c - run independent lift-tool invocations with bounded parallelism.
 *
 * The relifted-corpus configure used to run every per-chunk pass through a
 * synchronous execute_process.  The chunks are independent, so the host was
 * idle between processes.  This driver reads a simple argument-list file and
 * runs up to --jobs children at once, capturing each child's output in a log
 * that is printed only when that child fails.
 *
 * Job file format (one record per invocation):
 *
 *   <argc>
 *   <argv[0]>
 *   ...
 *   <argv[argc-1]>
 *
 * Every argument occupies one line.  Newlines inside arguments are not
 * supported, which is fine for generated paths and flag values.
 *
 *   jobpool --file jobs.txt [--jobs N] [--log-dir DIR]
 *   jobpool --self-test
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **args;
    int argc;
} Job;

typedef struct {
    Job *jobs;
    size_t count;
    const char *log_dir;
#if defined(_WIN32)
    volatile LONG next;
    volatile LONG failed;
    unsigned retries;
    CRITICAL_SECTION create_lock;
#else
    _Atomic long next;
    _Atomic long failed;
    unsigned retries;
#endif
} Pool;

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("jobpool: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static void buf_add(Buf *b, const void *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        b->p = (char *)xrealloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static char *read_line(FILE *f)
{
    size_t cap = 128;
    size_t n = 0;
    char *p = (char *)xmalloc(cap);
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (n + 2 > cap) {
            cap *= 2;
            p = (char *)xrealloc(p, cap);
        }
        p[n++] = (char)c;
    }
    if (c == EOF && n == 0) {
        free(p);
        return NULL;
    }
    if (n > 0 && p[n - 1] == '\r') n--;
    p[n] = 0;
    return p;
}

static Job *read_jobs(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    Job *jobs = NULL;
    size_t n = 0;
    size_t cap = 0;
    char *line;
    if (!f) die("cannot open job file %s", path);
    while ((line = read_line(f)) != NULL) {
        long argc;
        char *end = NULL;
        int i;
        if (line[0] == 0) {
            free(line);
            continue;
        }
        argc = strtol(line, &end, 10);
        if (!end || *end != 0 || argc < 1 || argc > 64)
            die("bad argc in %s: %s", path, line);
        free(line);
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            jobs = (Job *)xrealloc(jobs, cap * sizeof(*jobs));
        }
        jobs[n].argc = (int)argc;
        jobs[n].args = (char **)xmalloc(((size_t)argc + 1) * sizeof(char *));
        for (i = 0; i < argc; i++) {
            jobs[n].args[i] = read_line(f);
            if (!jobs[n].args[i])
                die("truncated job %zu in %s", n, path);
        }
        /* execv/execvp require a NULL-terminated argv.  Without this slot the
         * kernel walks past the heap allocation and intermittently fails with
         * EFAULT ("Bad address"), which looks like a missing executable. */
        jobs[n].args[argc] = NULL;
        n++;
    }
    fclose(f);
    *out_n = n;
    return jobs;
}

static void append_quoted(Buf *b, const char *arg)
{
    int need_quote = (arg[0] == 0);
    const char *p;
    size_t backslashes = 0;
    for (p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') {
            need_quote = 1;
            break;
        }
    }
    if (!need_quote) {
        buf_add(b, arg, strlen(arg));
        return;
    }
    buf_add(b, "\"", 1);
    for (p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
        } else if (*p == '"') {
            size_t i;
            for (i = 0; i < backslashes * 2 + 1; i++) buf_add(b, "\\", 1);
            buf_add(b, "\"", 1);
            backslashes = 0;
        } else {
            size_t i;
            for (i = 0; i < backslashes; i++) buf_add(b, "\\", 1);
            backslashes = 0;
            buf_add(b, p, 1);
        }
    }
    {
        size_t i;
        for (i = 0; i < backslashes * 2; i++) buf_add(b, "\\", 1);
    }
    buf_add(b, "\"", 1);
}

static char *build_command(const Job *job)
{
    Buf b = {0};
    int i;
    for (i = 0; i < job->argc; i++) {
        if (i) buf_add(&b, " ", 1);
        append_quoted(&b, job->args[i]);
    }
    if (!b.p) return NULL;
    return b.p;
}

static void print_log(const char *path)
{
    FILE *f = fopen(path, "rb");
    char buf[4096];
    size_t n;
    if (!f) return;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        fwrite(buf, 1, n, stderr);
    fclose(f);
}

static int run_one(Pool *pool, size_t index, int report)
{
    const Job *job = &pool->jobs[index];
    char log_path[4096];
    char *cmd = build_command(job);
#if !defined(_WIN32)
    int log;
    pid_t pid;
    int status = 0;
    int code = 1;

    if (!cmd) die("empty command at job %zu", index);
    snprintf(log_path, sizeof log_path, "%s/job_%04zu.log",
             pool->log_dir, index);
    log = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log < 0) {
        if (report)
            fprintf(stderr, "jobpool: cannot create %s (%s)\n",
                    log_path, strerror(errno));
        free(cmd);
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        if (report)
            fprintf(stderr, "jobpool: job %zu fork failed (%s): %s\n",
                    index, strerror(errno), cmd);
        close(log);
        unlink(log_path);
        free(cmd);
        return 1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        close(log);
        execvp(job->args[0], job->args);
        fprintf(stderr, "jobpool: exec '%s' failed: %s\n",
                job->args[0], strerror(errno));
        _exit(127);
    }
    close(log);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = -1; break; }
    }
    if (status >= 0) code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    if (code != 0) {
        if (report) {
            fprintf(stderr, "jobpool: job %zu failed (exit %d): %s\n",
                    index, code, cmd);
            print_log(log_path);
        }
        free(cmd);
        return 1;
    }
    unlink(log_path);
    free(cmd);
    return 0;
#else
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE log;
    DWORD wait_rc;
    DWORD exit_code = 1;
    DWORD create_err = 0;
    BOOL ok;

    if (!cmd) die("empty command at job %zu", index);
    _snprintf(log_path, sizeof log_path, "%s\\job_%04zu.log",
              pool->log_dir, index);
    log_path[sizeof log_path - 1] = 0;

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    /* CreateProcess inherits every inheritable handle in this process.  Keep
     * log-handle creation and process creation under one lock, and close the
     * parent copy immediately, so a child cannot inherit another worker's
     * log or keep it open after that worker exits. */
    EnterCriticalSection(&pool->create_lock);
    log = CreateFileA(log_path, GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        create_err = GetLastError();
        LeaveCriticalSection(&pool->create_lock);
        if (report)
            fprintf(stderr, "jobpool: cannot create %s (error %lu)\n",
                    log_path, (unsigned long)create_err);
        free(cmd);
        return 1;
    }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    memset(&pi, 0, sizeof pi);
    ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    if (!ok) create_err = GetLastError();
    if (ok) CloseHandle(log);
    LeaveCriticalSection(&pool->create_lock);
    if (!ok) {
        CloseHandle(log);
        if (report)
            fprintf(stderr,
                    "jobpool: job %zu CreateProcess failed (error %lu): %s\n",
                    index, (unsigned long)create_err, cmd);
        DeleteFileA(log_path);
        free(cmd);
        return 1;
    }

    wait_rc = WaitForSingleObject(pi.hProcess, INFINITE);
    if (wait_rc != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(pi.hProcess, &exit_code)) {
        if (report)
            fprintf(stderr, "jobpool: job %zu wait failed (error %lu): %s\n",
                    index, (unsigned long)GetLastError(), cmd);
        exit_code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) {
        if (report) {
            fprintf(stderr, "jobpool: job %zu failed (exit %lu): %s\n",
                    index, (unsigned long)exit_code, cmd);
            print_log(log_path);
        }
        free(cmd);
        return 1;
    }
    DeleteFileA(log_path);
    free(cmd);
    return 0;
#endif /* _WIN32 */
}

#if defined(_WIN32)
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
    Pool *pool = (Pool *)arg;
    for (;;) {
#if defined(_WIN32)
        LONG k = InterlockedIncrement(&pool->next) - 1;
#else
        long k = atomic_fetch_add(&pool->next, 1);
#endif
        unsigned attempt = 0;
        if ((size_t)k >= pool->count) break;
        for (;;) {
            int final = (attempt >= pool->retries);
            int rc = run_one(pool, (size_t)k, final);
            if (rc == 0) break;
            if (final) {
#if defined(_WIN32)
                InterlockedIncrement(&pool->failed);
#else
                atomic_fetch_add(&pool->failed, 1);
#endif
                break;
            }
            attempt++;
#if defined(_WIN32)
            Sleep(1000);
#else
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
#endif
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
typedef HANDLE PoolThread;
static double now_seconds(void) { return (double)GetTickCount64() / 1000.0; }
#else
typedef pthread_t PoolThread;
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

static int run_pool(const char *file, const char *log_dir, unsigned jobs,
                    unsigned retries)
{
    Pool pool;
    Job *job_list;
    size_t count = 0;
    PoolThread *threads;
    unsigned nthreads;
    unsigned i;
    double started;

    job_list = read_jobs(file, &count);
    if (!count) {
        fprintf(stderr, "jobpool: no jobs in %s\n", file);
        return 0;
    }
    if (!log_dir || !*log_dir) log_dir = "jobpool_logs";
#if defined(_WIN32)
    if (!CreateDirectoryA(log_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        die("cannot create log directory %s (error %lu)", log_dir,
            (unsigned long)GetLastError());
#else
    if (mkdir(log_dir, 0775) != 0 && errno != EEXIST)
        die("cannot create log directory %s (%s)", log_dir, strerror(errno));
#endif

    if (jobs == 0) {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        jobs = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
#else
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        jobs = cpus > 0 ? (unsigned)cpus : 1;
#endif
        /* These passes are dominated by file I/O and process startup.  The
         * cold-corpus A/B on the four-core host was faster at 12-16 workers
         * than at four or six, while thirty-two workers only added contention.
         * Keep the queue deep enough to hide disk latency and cap the process
         * fan-out so a larger host does not spawn an unbounded number. */
        jobs *= 3;
        if (jobs < 4) jobs = 4;
        if (jobs > 16) jobs = 16;
    }
    if (jobs > 64) jobs = 64;
    nthreads = jobs;
    if ((size_t)nthreads > count) nthreads = (unsigned)count;

    memset(&pool, 0, sizeof pool);
    pool.jobs = job_list;
    pool.count = count;
    pool.log_dir = log_dir;
    pool.retries = retries;
#if defined(_WIN32)
    InitializeCriticalSection(&pool.create_lock);
#else
    atomic_init(&pool.next, 0);
    atomic_init(&pool.failed, 0);
#endif

    threads = (PoolThread *)xmalloc((size_t)nthreads * sizeof(*threads));
    started = now_seconds();
    fprintf(stderr, "jobpool: %zu jobs, %u worker(s)\n", count, nthreads);
    for (i = 0; i < nthreads; i++) {
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, worker, &pool, 0, NULL);
        if (!threads[i]) die("cannot create worker thread");
#else
        if (pthread_create(&threads[i], NULL, worker, &pool) != 0)
            die("cannot create worker thread");
#endif
    }
#if defined(_WIN32)
    WaitForMultipleObjects(nthreads, threads, TRUE, INFINITE);
    for (i = 0; i < nthreads; i++) CloseHandle(threads[i]);
#else
    for (i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
#endif
    free(threads);
#if defined(_WIN32)
    DeleteCriticalSection(&pool.create_lock);
#endif

    for (i = 0; i < count; i++) {
        int k;
        for (k = 0; k < job_list[i].argc; k++) free(job_list[i].args[k]);
        free(job_list[i].args);
    }
    free(job_list);
    if (pool.failed) {
        fprintf(stderr, "jobpool: %ld job(s) failed in %.1f s\n",
                (long)pool.failed, now_seconds() - started);
        return 1;
    }
    fprintf(stderr, "jobpool: all jobs passed in %.1f s\n",
            now_seconds() - started);
    return 0;
}

static int self_test(void)
{
#if defined(_WIN32)
    char dir[MAX_PATH];
    char file[MAX_PATH];
    FILE *f;
    int i;
    DWORD n;
    int rc;

    n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return 1;
    strncat(dir, "jobpool_selftest", sizeof(dir) - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    if (GetTempFileNameA(dir, "job", 0, file) == 0) return 1;
    f = fopen(file, "wb");
    if (!f) return 1;
    for (i = 0; i < 8; i++)
        fputs("4\ncmd.exe\n/c\nexit\n0\n", f);
    fclose(f);
    rc = run_pool(file, dir, 4, 0);
    DeleteFileA(file);
    if (rc == 0) puts("jobpool self-test: OK");
    return rc;
#else
    const char *tmp = getenv("TMPDIR");
    char dir[4096];
    char file[4096];
    FILE *f;
    int i;
    int rc;

    /* Termux has no /tmp; $HOME is the portable writable fallback. */
    if (!tmp || !*tmp) tmp = getenv("HOME");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(dir, sizeof dir, "%s/jobpool_selftest_XXXXXX", tmp);
    if (!mkdtemp(dir)) return 1;
    snprintf(file, sizeof file, "%s/jobs.txt", dir);
    f = fopen(file, "wb");
    if (!f) return 1;
    for (i = 0; i < 8; i++)
        fputs("3\n/bin/sh\n-c\nexit 0\n", f);
    fclose(f);
    rc = run_pool(file, dir, 4, 0);
    unlink(file);
    rmdir(dir);
    if (rc == 0) puts("jobpool self-test: OK");
    return rc;
#endif
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *log_dir = NULL;
    unsigned jobs = 0;
    unsigned retries = 0;
    int i;

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file = argv[++i];
        } else if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
            jobs = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            log_dir = argv[++i];
        } else if (strcmp(argv[i], "--retries") == 0 && i + 1 < argc) {
            retries = (unsigned)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr,
                    "usage: jobpool --file JOBS [--jobs N] [--log-dir DIR] "
                    "[--retries N]\n"
                    "       jobpool --self-test\n");
            return 2;
        }
    }
    if (!file) {
        fprintf(stderr, "jobpool: --file is required\n");
        return 2;
    }
    return run_pool(file, log_dir, jobs, retries);
}
