/*
 * dxvk_win32.cpp - POSIX bodies for the Win32 surface DXVK compiles against.
 *
 * DXVK is a Win32 library: it opens files with CreateFileW, times frames with
 * QueryPerformanceCounter, waits on events with WaitForSingleObject and reads
 * the display mode with EnumDisplaySettings.  runtime/android/dxvk_shim is a
 * header-only stand-in for <windows.h>; this file is the other half - the
 * actual behaviour.
 *
 * Two rules govern everything here:
 *
 *   - Anything DXVK needs to *work* is implemented for real (file I/O, the
 *     synchronisation primitives, UTF-8 <-> UTF-16 conversion, time, module
 *     loading).  A stub that silently returns FALSE in one of those turns into
 *     a mystery hang or an empty log rather than a clear failure.
 *   - Anything that exists only because D3D9 talks to a window system (cursor,
 *     GDI, window messages, registry, display-mode changes) is a benign no-op
 *     that returns the value a headless process should see.  Those paths are
 *     unreachable once the swapchain is offscreen, so a stub cannot hurt.
 *
 * It is C++ rather than C because the shim header uses templates and
 * operator overloads (the __uuidof emulation and the enum flag operators),
 * which are exactly what makes it acceptable to C++ source in the first
 * place.
 */

#include "windows.h"
#include "wc3_lowmem.h"

#include <android/native_window.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>

/* ------------------------------------------------------------------ misc */

namespace {

/* Pseudo-handles.  Windows uses (HANDLE)-1 for the current process; the real
 * value only has to be distinguishable from every allocated handle, and must
 * not be freed by CloseHandle. */
HANDLE const kCurrentProcess = (HANDLE)(intptr_t)-2;
HANDLE const kCurrentThread  = (HANDLE)(intptr_t)-3;

/* A module handle that means "resolve against the process image".  Android
 * links Vulkan directly, so there is no loader DLL to dlopen. */
HMODULE const kProcessImage = (HMODULE)(intptr_t)1;

thread_local DWORD t_lastError = 0;

uint64_t monotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

timespec realtimeDeadline(DWORD ms) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000u;
    ts.tv_nsec += long(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec  += 1;
    }
    return ts;
}

/* The real window, published by the hosting activity before the guest starts.
 * Everything that asks the "window" for its size - GetClientRect, which feeds
 * both DXVK's present rect and EnumDisplaySettings - reads it here, so the
 * Android build sees the same single source of truth the Windows host does
 * (there: the HWND). */
static std::mutex g_androidWindowMutex;
static ANativeWindow* g_androidWindow = nullptr;
static uint32_t g_androidWindowWidth  = 0;
static uint32_t g_androidWindowHeight = 0;
/* Incremented whenever a different ANativeWindow is published.  DXVK needs
 * this because its swap chain is tied to the old VkSurfaceKHR: the fake guest
 * HWND never changes, so DXVK cannot notice the Android surface replacement on
 * its own.  The generation is read together with the pointer so a presenter
 * never observes a new window with an old surface generation. */
static uint32_t g_androidWindowGeneration;

extern "C" void wc3_android_set_window(ANativeWindow* window,
                                       uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    ANativeWindow* previous = g_androidWindow;
    g_androidWindow = window;
    g_androidWindowWidth = width;
    g_androidWindowHeight = height;
    if (window && window != previous)
        g_androidWindowGeneration++;
}

/* Set the physical Android surface buffer geometry and publish that same size
 * to the Win32 shim.  This is the present size, not the guest display mode:
 * DXVK's presenter reads GetClientRect as its destination rectangle, so it
 * must stay at the native surface size while the guest renders a smaller
 * back buffer.  The swap-chain blitter then scales the guest image to fill
 * the surface.  Passing a guest mode here would leave the present rectangle
 * smaller than the surface and move the image into a corner. */
extern "C" int wc3_android_set_surface_size(uint32_t width, uint32_t height) {
    if (!width || !height) return -EINVAL;
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    int rc = 0;
    if (g_androidWindow) {
        rc = ANativeWindow_setBuffersGeometry(g_androidWindow,
                                              (int32_t)width,
                                              (int32_t)height,
                                              WINDOW_FORMAT_RGBA_8888);
    }
    /* Publish even when the geometry call failed: the caller logs the error
     * and DXVK should still see the requested extent instead of the stale one. */
    g_androidWindowWidth = width;
    g_androidWindowHeight = height;
    return rc;
}

/* The Android surface is owned by the activity.  A destroyed ANativeWindow
 * must never reach DXVK: its Surface holds a mutex that bionic reports as
 * destroyed, and the first ANativeWindow_* call then aborts.  Clear only the
 * exact pointer the activity destroyed so an already-created replacement is
 * not lost. */
extern "C" void wc3_android_clear_window(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    if (g_androidWindow == window) {
        g_androidWindow = nullptr;
        g_androidWindowWidth = 0;
        g_androidWindowHeight = 0;
    }
}

extern "C" ANativeWindow* wc3_android_window(void) {
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    return g_androidWindow;
}

/* Atomic snapshot for DXVK's Present path.  Reading the pointer and the
 * generation under one lock avoids a frame that sees the replacement window
 * but still believes the old VkSurfaceKHR is current. */
extern "C" ANativeWindow* wc3_android_window_state(uint32_t* generation) {
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    if (generation)
        *generation = g_androidWindowGeneration;
    return g_androidWindow;
}

/* Physical surface size the fake monitor and DXVK's presenter report.  Once
 * the activity has created the window this is the window's native size, not
 * the guest display mode; DXVK uses it as the swap-chain/present extent and
 * scales the guest back buffer to it.  The fallback only covers the
 * windowless case (the standalone dxvk_probe) and the guest asking before the
 * activity is up. */
void surfaceSize(uint32_t& w, uint32_t& h) {
    std::lock_guard<std::mutex> lock(g_androidWindowMutex);
    if (g_androidWindow && g_androidWindowWidth && g_androidWindowHeight) {
        w = g_androidWindowWidth;
        h = g_androidWindowHeight;
        return;
    }
    w = 1280;
    h = 720;
    if (const char* e = getenv("WC3_DXVK_WIDTH"))  { if (atoi(e) > 0) w = uint32_t(atoi(e)); }
    if (const char* e = getenv("WC3_DXVK_HEIGHT")) { if (atoi(e) > 0) h = uint32_t(atoi(e)); }
}

std::string wideToUtf8(const WCHAR* s) {
    std::string out;
    if (!s) return out;
    while (*s) {
        uint32_t cp = *s++;
        if (cp >= 0xD800 && cp <= 0xDBFF && *s >= 0xDC00 && *s <= 0xDFFF) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (uint32_t(*s++) - 0xDC00u);
        }
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

void appendUtf8(std::u16string& out, uint32_t cp) {
    if (cp < 0x10000) {
        out += char16_t(cp);
    } else {
        cp -= 0x10000u;
        out += char16_t(0xD800u + (cp >> 10));
        out += char16_t(0xDC00u + (cp & 0x3FFu));
    }
}

} // namespace

/* ------------------------------------------------------------- last error */

DWORD GetLastError(void) { return t_lastError; }
void  SetLastError(DWORD err) { t_lastError = err; }

/* ------------------------------------------------------------------- time */

void Sleep(DWORD ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    /* A Windows sleep of INFINITE never returns on its own; DXVK does not use
     * it, but clamping would silently turn a bug into a busy loop. */
    timespec ts;
    ts.tv_sec  = time_t(ms / 1000u);
    ts.tv_nsec = long(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

DWORD GetTickCount(void) { return DWORD(monotonicNs() / 1000000ull); }
ULONGLONG GetTickCount64(void) { return ULONGLONG(monotonicNs() / 1000000ull); }

BOOL QueryPerformanceCounter(LARGE_INTEGER* v) {
    if (!v) return FALSE;
    v->QuadPart = LONGLONG(monotonicNs());
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* v) {
    if (!v) return FALSE;
    v->QuadPart = 1000000000ll;   /* the counter ticks in nanoseconds */
    return TRUE;
}

BOOL SwitchToThread(void) { return sched_yield() == 0 ? TRUE : FALSE; }

/* ------------------------------------------------------- process / thread */

DWORD  GetCurrentProcessId(void) { return DWORD(getpid()); }
DWORD  GetCurrentThreadId(void) { return DWORD(syscall(SYS_gettid)); }
HANDLE GetCurrentProcess(void) { return kCurrentProcess; }
HANDLE GetCurrentThread(void) { return kCurrentThread; }

DWORD GetActiveProcessorCount(WORD /*group*/) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? DWORD(n) : 1u;
}

/* ----------------------------------------------------------- system info */

void GetSystemInfo(SYSTEM_INFO* si) {
    if (!si) return;
    memset(si, 0, sizeof(*si));
    long page = sysconf(_SC_PAGESIZE);
    si->dwPageSize = page > 0 ? DWORD(page) : 4096u;
    si->dwNumberOfProcessors = GetActiveProcessorCount(0);
    si->dwAllocationGranularity = 65536u;
    si->lpMinimumApplicationAddress = (void*)(uintptr_t)0x00010000u;
    si->lpMaximumApplicationAddress = (void*)(uintptr_t)0x00007FFFFFFEFFFFull;
    si->dwActiveProcessorMask = ~DWORD_PTR(0);
    si->u.s.wProcessorArchitecture = 12 /* PROCESSOR_ARCHITECTURE_ARM64 */;
}

void GetNativeSystemInfo(SYSTEM_INFO* si) { GetSystemInfo(si); }

/* --------------------------------------------------------------- memory
 *
 * DXVK 1.10.3 and 3.1.1 do not call these; they exist because the header
 * declares them and a future caller should get real behaviour rather than a
 * silent FALSE. */

void* VirtualAlloc(void* addr, size_t size, DWORD type, DWORD protect) {
    if (!size) { SetLastError(87); return nullptr; }

    int prot = PROT_READ | PROT_WRITE;
    if (type & 0x00002000u /* MEM_RESERVE */ && !(type & 0x00001000u /* MEM_COMMIT */))
        prot = PROT_NONE;

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (addr)
        flags |= MAP_FIXED_NOREPLACE;

    void* p = mmap(addr, size, prot, flags, -1, 0);
    if (p == MAP_FAILED) {
        SetLastError(errno == EEXIST ? 487 /* ERROR_INVALID_ADDRESS */ : 8);
        return nullptr;
    }
    return p;
}

BOOL VirtualFree(void* addr, size_t size, DWORD type) {
    if (!addr) return TRUE;
    /* MEM_RELEASE ignores the size and frees the whole reservation, which is
     * what munmap does when the address is the mapping base. */
    if (munmap(addr, size) != 0) {
        SetLastError(8);
        return FALSE;
    }
    return TRUE;
}

/* The one thing in this port that genuinely needs to know what is mapped
 * where: hle_d3d9_dxvk.cpp walks the low 2 GB looking for a free region to
 * stage 32-bit-visible locks in.  /proc/self/maps is the only description of
 * the address space available here, and it answers exactly the question
 * VirtualQuery asks - "what is at this address, and how far does it go". */
size_t VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, size_t len) {
    if (!mbi || len < sizeof *mbi) { SetLastError(87); return 0; }

    const uintptr_t a = uintptr_t(addr);
    uintptr_t regionStart = 0, regionEnd = 0, nextStart = 0;
    int prot = 0;
    bool found = false;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) { SetLastError(1); return 0; }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = { 0 };
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (a >= lo && a < hi) {
            regionStart = lo;
            regionEnd   = hi;
            prot = (perms[0] == 'r' ? 1 : 0)
                 | (perms[1] == 'w' ? 2 : 0)
                 | (perms[2] == 'x' ? 4 : 0);
            found = true;
            break;
        }
        if (lo > a && (!nextStart || lo < nextStart)) nextStart = lo;
    }
    fclose(f);

    memset(mbi, 0, sizeof *mbi);
    if (found) {
        mbi->BaseAddress  = (void*)regionStart;
        mbi->AllocationBase = (void*)regionStart;
        /* Windows reports the region containing addr, and its RegionSize is
         * measured from BaseAddress; the caller here adds the two, so a
         * mapping that starts below addr still advances past itself. */
        mbi->RegionSize = regionEnd - a;
        mbi->State      = MEM_COMMIT;
        mbi->Protect    = (prot & 1) ? ((prot & 2) ? PAGE_READWRITE : PAGE_READONLY)
                                     : PAGE_NOACCESS;
    } else {
        mbi->BaseAddress = (void*)a;
        mbi->RegionSize  = (nextStart ? nextStart : 0x7FFFFFFFull) - a;
        mbi->State       = MEM_FREE;
        mbi->Protect     = PAGE_NOACCESS;
    }
    return sizeof *mbi;
}

BOOL VirtualProtect(void* addr, size_t size, DWORD newProt, DWORD* oldProt) {
    int prot = 0;
    switch (newProt & 0xFFu) {
        case 0x01: prot = PROT_READ; break;                       /* PAGE_NOACCESS-ish */
        case 0x02: prot = PROT_READ; break;                       /* PAGE_READONLY */
        case 0x04: prot = PROT_READ | PROT_WRITE; break;          /* PAGE_READWRITE */
        case 0x20: prot = PROT_READ | PROT_EXEC; break;           /* PAGE_EXECUTE_READ */
        case 0x40: prot = PROT_READ | PROT_WRITE | PROT_EXEC; break;
        default:   prot = PROT_READ | PROT_WRITE; break;
    }
    if (oldProt) *oldProt = 0x04;
    return mprotect(addr, size, prot) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------- TLS */

DWORD TlsAlloc(void) {
    pthread_key_t key;
    if (pthread_key_create(&key, nullptr) != 0) {
        SetLastError(1450);   /* ERROR_NO_SYSTEM_RESOURCES */
        return 0xFFFFFFFFu;
    }
    return DWORD(key);
}

BOOL TlsFree(DWORD idx) {
    return pthread_key_delete(pthread_key_t(idx)) == 0 ? TRUE : FALSE;
}

void* TlsGetValue(DWORD idx) {
    return pthread_getspecific(pthread_key_t(idx));
}

BOOL TlsSetValue(DWORD idx, void* v) {
    return pthread_setspecific(pthread_key_t(idx), v) == 0 ? TRUE : FALSE;
}

/* --------------------------------------------------------- critical section
 *
 * A Windows critical section is recursive, so the backing mutex must be too -
 * DXVK's RecursiveMutex relies on it and would otherwise deadlock on the
 * first re-entrant lock. */

void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

void DeleteCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutex_destroy(&cs->mtx);
}

void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(&cs->mtx); }
void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(&cs->mtx); }

BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) {
    return pthread_mutex_trylock(&cs->mtx) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------- condition variable */

void InitializeConditionVariable(CONDITION_VARIABLE* cv) {
    pthread_cond_init(&cv->cond, nullptr);
}

void WakeConditionVariable(CONDITION_VARIABLE* cv) { pthread_cond_signal(&cv->cond); }
void WakeAllConditionVariable(CONDITION_VARIABLE* cv) { pthread_cond_broadcast(&cv->cond); }

BOOL SleepConditionVariableCS(CONDITION_VARIABLE* cv, CRITICAL_SECTION* cs, DWORD ms) {
    if (ms == INFINITE) {
        pthread_cond_wait(&cv->cond, &cs->mtx);
        return TRUE;
    }
    timespec dl = realtimeDeadline(ms);
    if (pthread_cond_timedwait(&cv->cond, &cs->mtx, &dl) == ETIMEDOUT) {
        SetLastError(1460);   /* ERROR_TIMEOUT */
        return FALSE;
    }
    return TRUE;
}

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* cv, SRWLOCK* l, DWORD ms, ULONG /*flags*/) {
    if (ms == INFINITE) {
        pthread_cond_wait(&cv->cond, &l->mtx);
        return TRUE;
    }
    timespec dl = realtimeDeadline(ms);
    if (pthread_cond_timedwait(&cv->cond, &l->mtx, &dl) == ETIMEDOUT) {
        SetLastError(1460);
        return FALSE;
    }
    return TRUE;
}

/* -------------------------------------------------------------- SRW locks
 *
 * Backed by a plain mutex, so the shared forms serialise readers.  That is a
 * throughput choice, not a correctness one: DXVK uses these to protect its
 * command queues and never relies on two readers running concurrently. */

void InitializeSRWLock(SRWLOCK* l) { pthread_mutex_init(&l->mtx, nullptr); }
void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_mutex_lock(&l->mtx); }
void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_mutex_unlock(&l->mtx); }
void AcquireSRWLockShared(SRWLOCK* l) { pthread_mutex_lock(&l->mtx); }
void ReleaseSRWLockShared(SRWLOCK* l) { pthread_mutex_unlock(&l->mtx); }
BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) { return pthread_mutex_trylock(&l->mtx) == 0 ? TRUE : FALSE; }
BOOL TryAcquireSRWLockShared(SRWLOCK* l) { return pthread_mutex_trylock(&l->mtx) == 0 ? TRUE : FALSE; }

/* ------------------------------------------------------ kernel objects
 *
 * One tagged allocation covers files, events, semaphores, threads and file
 * mappings, so CloseHandle can dispatch on the kind instead of guessing. */

namespace {

enum HkKind { HK_FILE, HK_EVENT, HK_SEMAPHORE, HK_THREAD, HK_MAPPING };

struct Hk {
    HkKind kind;

    union {
        int fd;                       /* HK_FILE, HK_MAPPING */

        struct {
            pthread_mutex_t mtx;
            pthread_cond_t  cv;
            int             signaled;
            int             manual;
        } ev;

        struct {
            pthread_mutex_t mtx;
            pthread_cond_t  cv;
            long            count;
            long            maximum;
        } sem;

        struct {
            pthread_mutex_t mtx;
            pthread_cond_t  cv;
            pthread_t       thread;
            int             finished;
            DWORD           exitCode;
        } thr;
    };

    /* Mappings need the size to unmap on close. */
    size_t mapLength;
};

Hk* hkNew(HkKind kind) {
    Hk* h = static_cast<Hk*>(calloc(1, sizeof(Hk)));
    if (h) h->kind = kind;
    return h;
}

bool hkIsPseudo(HANDLE h) {
    return h == kCurrentProcess || h == kCurrentThread;
}

} // namespace

HANDLE CreateEventA(void* /*sa*/, BOOL manualReset, BOOL initialState, LPCSTR /*name*/) {
    Hk* h = hkNew(HK_EVENT);
    if (!h) { SetLastError(8); return nullptr; }
    pthread_mutex_init(&h->ev.mtx, nullptr);
    pthread_cond_init(&h->ev.cv, nullptr);
    h->ev.manual   = manualReset ? 1 : 0;
    h->ev.signaled = initialState ? 1 : 0;
    return (HANDLE)h;
}

HANDLE CreateEventW(void* sa, BOOL manualReset, BOOL initialState, LPCWSTR name) {
    std::string narrow = wideToUtf8(name);
    return CreateEventA(sa, manualReset, initialState, name ? narrow.c_str() : nullptr);
}

BOOL SetEvent(HANDLE h) {
    if (!h || hkIsPseudo(h)) { SetLastError(6); return FALSE; }
    Hk* k = (Hk*)h;
    if (k->kind != HK_EVENT) { SetLastError(6); return FALSE; }

    pthread_mutex_lock(&k->ev.mtx);
    k->ev.signaled = 1;
    if (k->ev.manual)
        pthread_cond_broadcast(&k->ev.cv);
    else
        pthread_cond_signal(&k->ev.cv);
    pthread_mutex_unlock(&k->ev.mtx);
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    if (!h || hkIsPseudo(h)) { SetLastError(6); return FALSE; }
    Hk* k = (Hk*)h;
    if (k->kind != HK_EVENT) { SetLastError(6); return FALSE; }

    pthread_mutex_lock(&k->ev.mtx);
    k->ev.signaled = 0;
    pthread_mutex_unlock(&k->ev.mtx);
    return TRUE;
}

HANDLE CreateSemaphoreA(void* /*sa*/, LONG initial, LONG max, LPCSTR /*name*/) {
    Hk* h = hkNew(HK_SEMAPHORE);
    if (!h) { SetLastError(8); return nullptr; }
    pthread_mutex_init(&h->sem.mtx, nullptr);
    pthread_cond_init(&h->sem.cv, nullptr);
    h->sem.count   = initial;
    h->sem.maximum = max;
    return (HANDLE)h;
}

BOOL ReleaseSemaphore(HANDLE h, LONG count, LONG* prev) {
    if (!h || hkIsPseudo(h)) { SetLastError(6); return FALSE; }
    Hk* k = (Hk*)h;
    if (k->kind != HK_SEMAPHORE) { SetLastError(6); return FALSE; }

    pthread_mutex_lock(&k->sem.mtx);
    if (prev) *prev = k->sem.count;
    k->sem.count += count;
    if (k->sem.maximum && k->sem.count > k->sem.maximum) {
        k->sem.count = k->sem.maximum;
        pthread_mutex_unlock(&k->sem.mtx);
        SetLastError(298);    /* ERROR_TOO_MANY_POSTS */
        return FALSE;
    }
    pthread_cond_broadcast(&k->sem.cv);
    pthread_mutex_unlock(&k->sem.mtx);
    return TRUE;
}

/* Thread entry trampoline.  A plain pthread_create cannot report the DWORD
 * result, and WaitForSingleObject needs one. */
namespace {

struct ThreadStart {
    DWORD (__stdcall *fn)(void*);
    void*  arg;
    Hk*    owner;
};

void* threadTrampoline(void* p) {
    ThreadStart* s = static_cast<ThreadStart*>(p);
    DWORD rc = s->fn(s->arg);

    Hk* owner = s->owner;
    free(s);

    pthread_mutex_lock(&owner->thr.mtx);
    owner->thr.exitCode = rc;
    owner->thr.finished = 1;
    pthread_cond_broadcast(&owner->thr.cv);
    pthread_mutex_unlock(&owner->thr.mtx);
    return nullptr;
}

} // namespace

HANDLE CreateThread(void* /*sa*/, size_t stackSize, DWORD (__stdcall *fn)(void*),
                    void* arg, DWORD /*flags*/, DWORD* tid) {
    Hk* h = hkNew(HK_THREAD);
    if (!h) { SetLastError(8); return nullptr; }
    pthread_mutex_init(&h->thr.mtx, nullptr);
    pthread_cond_init(&h->thr.cv, nullptr);

    ThreadStart* s = static_cast<ThreadStart*>(malloc(sizeof(ThreadStart)));
    if (!s) { free(h); SetLastError(8); return nullptr; }
    s->fn = fn;
    s->arg = arg;
    s->owner = h;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize) pthread_attr_setstacksize(&attr, stackSize);

    if (pthread_create(&h->thr.thread, &attr, threadTrampoline, s) != 0) {
        pthread_attr_destroy(&attr);
        free(s);
        free(h);
        SetLastError(8);
        return nullptr;
    }
    pthread_attr_destroy(&attr);

    if (tid) *tid = 0;
    return (HANDLE)h;
}

BOOL SetThreadPriority(HANDLE /*thread*/, int /*priority*/) { return TRUE; }
int  GetThreadPriority(HANDLE /*thread*/) { return 0; }

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h) { SetLastError(6); return 0xFFFFFFFFu; }   /* WAIT_FAILED */

    /* The process and the current thread are always signalled. */
    if (hkIsPseudo(h)) return 0u;                      /* WAIT_OBJECT_0 */

    Hk* k = (Hk*)h;

    switch (k->kind) {
        case HK_FILE:
        case HK_MAPPING:
            return 0u;                                 /* always ready */

        case HK_EVENT: {
            pthread_mutex_lock(&k->ev.mtx);
            while (!k->ev.signaled) {
                if (ms == INFINITE) {
                    pthread_cond_wait(&k->ev.cv, &k->ev.mtx);
                    continue;
                }
                timespec dl = realtimeDeadline(ms);
                if (pthread_cond_timedwait(&k->ev.cv, &k->ev.mtx, &dl) == ETIMEDOUT) {
                    if (!k->ev.signaled) {
                        pthread_mutex_unlock(&k->ev.mtx);
                        return 258u;                   /* WAIT_TIMEOUT */
                    }
                    break;
                }
            }
            /* Auto-reset events consume the signal for exactly one waiter. */
            if (!k->ev.manual)
                k->ev.signaled = 0;
            pthread_mutex_unlock(&k->ev.mtx);
            return 0u;
        }

        case HK_SEMAPHORE: {
            pthread_mutex_lock(&k->sem.mtx);
            while (k->sem.count <= 0) {
                if (ms == INFINITE) {
                    pthread_cond_wait(&k->sem.cv, &k->sem.mtx);
                    continue;
                }
                timespec dl = realtimeDeadline(ms);
                if (pthread_cond_timedwait(&k->sem.cv, &k->sem.mtx, &dl) == ETIMEDOUT) {
                    if (k->sem.count <= 0) {
                        pthread_mutex_unlock(&k->sem.mtx);
                        return 258u;
                    }
                    break;
                }
            }
            k->sem.count--;
            pthread_mutex_unlock(&k->sem.mtx);
            return 0u;
        }

        case HK_THREAD: {
            pthread_mutex_lock(&k->thr.mtx);
            while (!k->thr.finished) {
                if (ms == INFINITE) {
                    pthread_cond_wait(&k->thr.cv, &k->thr.mtx);
                    continue;
                }
                timespec dl = realtimeDeadline(ms);
                if (pthread_cond_timedwait(&k->thr.cv, &k->thr.mtx, &dl) == ETIMEDOUT) {
                    if (!k->thr.finished) {
                        pthread_mutex_unlock(&k->thr.mtx);
                        return 258u;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&k->thr.mtx);
            return 0u;
        }
    }
    return 0xFFFFFFFFu;
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL /*alertable*/) {
    return WaitForSingleObject(h, ms);
}

DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL waitAll, DWORD ms) {
    if (!hs || !n) { SetLastError(87); return 0xFFFFFFFFu; }

    uint64_t deadline = (ms == INFINITE) ? UINT64_MAX
                                         : monotonicNs() + uint64_t(ms) * 1000000ull;

    if (waitAll) {
        /* Not an atomic multi-wait: each object is taken in turn.  DXVK never
         * calls this, and the only correct general form would need a shared
         * waiter list; the sequential form is right for objects that stay
         * signalled once set, which is the case it is here for. */
        for (DWORD i = 0; i < n; ++i) {
            DWORD remaining = ms;
            if (ms != INFINITE) {
                uint64_t now = monotonicNs();
                if (now >= deadline) return 258u;
                remaining = DWORD((deadline - now) / 1000000ull);
            }
            DWORD r = WaitForSingleObject(hs[i], remaining);
            if (r != 0u) return r;
        }
        return 0u;
    }

    for (;;) {
        for (DWORD i = 0; i < n; ++i) {
            if (WaitForSingleObject(hs[i], 0) == 0u)
                return i;      /* WAIT_OBJECT_0 + index */
        }
        if (ms != INFINITE && monotonicNs() >= deadline)
            return 258u;       /* WAIT_TIMEOUT */

        /* Poll rather than block on one object: a real multi-wait needs the
         * objects to share a condition variable, which they do not. */
        Sleep(1);
    }
}

BOOL CloseHandle(HANDLE h) {
    if (!h || hkIsPseudo(h)) return TRUE;    /* pseudo-handles are never closed */

    Hk* k = (Hk*)h;
    switch (k->kind) {
        case HK_FILE:
            close(k->fd);
            break;

        case HK_MAPPING:
            /* The view is unmapped by UnmapViewOfFile; only the fd is ours. */
            close(k->fd);
            break;

        case HK_EVENT:
            pthread_mutex_destroy(&k->ev.mtx);
            pthread_cond_destroy(&k->ev.cv);
            break;

        case HK_SEMAPHORE:
            pthread_mutex_destroy(&k->sem.mtx);
            pthread_cond_destroy(&k->sem.cv);
            break;

        case HK_THREAD:
            /* Detach rather than join: a thread that is still running when its
             * handle is closed must not be waited for. */
            pthread_detach(k->thr.thread);
            pthread_mutex_destroy(&k->thr.mtx);
            pthread_cond_destroy(&k->thr.cv);
            break;
    }
    free(k);
    return TRUE;
}

/* ------------------------------------------------------------- modules
 *
 * DXVK asks for "vulkan-1.dll".  Android links Vulkan into the process, so a
 * failed dlopen falls back to a handle that resolves against the global
 * symbol table. */

HMODULE LoadLibraryA(LPCSTR name) {
    if (!name) { SetLastError(87); return nullptr; }

    if (void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL))
        return (HMODULE)h;

    if (strstr(name, "vulkan")) {
        if (void* h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))
            return (HMODULE)h;
        return kProcessImage;
    }
    return nullptr;
}

HMODULE LoadLibraryW(LPCWSTR name) {
    std::string narrow = wideToUtf8(name);
    return LoadLibraryA(narrow.c_str());
}

HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name)
        return (HMODULE)dlopen(nullptr, RTLD_NOW);
    void* h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    return (HMODULE)h;
}

HMODULE GetModuleHandleW(LPCWSTR name) {
    std::string narrow = wideToUtf8(name);
    return GetModuleHandleA(name ? narrow.c_str() : nullptr);
}

HMODULE GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* result) {
    if (!result) { SetLastError(87); return nullptr; }

    if (flags & 0x00000004u /* FROM_ADDRESS */) {
        *result = kProcessImage;
        return kProcessImage;
    }
    HMODULE m = GetModuleHandleA(name);
    *result = m;
    return m;
}

HMODULE GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* result) {
    std::string narrow = wideToUtf8(name);
    return GetModuleHandleExA(flags, name ? narrow.c_str() : nullptr, result);
}

void* GetProcAddress(HMODULE m, LPCSTR name) {
    if (!m || !name) return nullptr;
    if (m == kProcessImage)
        return dlsym(RTLD_DEFAULT, name);
    return dlsym((void*)m, name);
}

BOOL FreeLibrary(HMODULE m) {
    if (!m || m == kProcessImage) return TRUE;
    return dlclose((void*)m) == 0 ? TRUE : FALSE;
}

DWORD GetModuleFileNameA(HMODULE /*m*/, LPSTR buf, DWORD n) {
    if (!buf || !n) { SetLastError(87); return 0; }

    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len < 0) {
        SetLastError(8);
        return 0;
    }
    path[len] = '\0';

    if (DWORD(len) >= n) {
        SetLastError(122);   /* ERROR_INSUFFICIENT_BUFFER */
        memcpy(buf, path, n - 1);
        buf[n - 1] = '\0';
        return n;
    }
    memcpy(buf, path, size_t(len) + 1);
    return DWORD(len);
}

DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n) {
    if (!buf || !n) { SetLastError(87); return 0; }

    char narrow[4096];
    DWORD len = GetModuleFileNameA(m, narrow, sizeof(narrow));
    if (!len) return 0;

    std::u16string wide;
    for (DWORD i = 0; i < len; ++i)
        wide += char16_t(uint8_t(narrow[i]));

    if (DWORD(wide.size()) >= n) {
        SetLastError(122);
        memcpy(buf, wide.data(), (n - 1) * sizeof(WCHAR));
        buf[n - 1] = 0;
        return n;
    }
    memcpy(buf, wide.data(), wide.size() * sizeof(WCHAR));
    buf[wide.size()] = 0;
    return DWORD(wide.size());
}

/* ---------------------------------------------------- character conversion
 *
 * DXVK routes every string conversion through these two, so they have to be
 * real.  WCHAR is 16-bit (the shim is built with -fshort-wchar), which is
 * exactly the encoding Windows uses, so no translation layer is needed. */

int MultiByteToWideChar(UINT /*cp*/, DWORD /*flags*/, LPCSTR src, int srcLen,
                        LPWSTR dst, int dstLen) {
    if (!src) { SetLastError(87); return 0; }

    /* A negative length means "null-terminated, and the terminator counts". */
    size_t limit = srcLen < 0 ? strlen(src) + 1 : size_t(srcLen);
    std::u16string out;
    out.reserve(limit);

    size_t i = 0;
    while (i < limit) {
        uint8_t c = uint8_t(src[i]);
        if (c == 0) {
            out += char16_t(0);
            ++i;
            continue;
        }

        uint32_t cp;
        size_t extra;
        if (c < 0x80)                { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu;  extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu;  extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u;  extra = 3; }
        else                         { cp = 0xFFFDu;    extra = 0; }

        /* Truncated or malformed sequences become U+FFFD rather than reading
         * past the end - a shader path that is not valid UTF-8 should still
         * produce a usable string. */
        size_t consumed = 1;
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= limit) { cp = 0xFFFDu; break; }
            uint8_t cc = uint8_t(src[i + k]);
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFDu; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
            consumed = k + 1;
        }
        appendUtf8(out, cp);
        i += consumed;
    }

    int needed = int(out.size());
    if (dstLen == 0 || !dst)
        return needed;

    int n = needed < dstLen ? needed : dstLen;
    memcpy(dst, out.data(), size_t(n) * sizeof(WCHAR));
    return n;
}

int WideCharToMultiByte(UINT /*cp*/, DWORD /*flags*/, LPCWSTR src, int srcLen,
                        LPSTR dst, int dstLen, LPCSTR /*defChar*/, BOOL* usedDef) {
    if (!src) { SetLastError(87); return 0; }
    if (usedDef) *usedDef = FALSE;

    std::string out;
    int i = 0;
    while (srcLen < 0 ? src[i] != 0 : i < srcLen) {
        uint32_t cp = src[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF &&
            (srcLen < 0 ? src[i] != 0 : i < srcLen)) {
            uint32_t lo = src[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                ++i;
            }
        }

        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    int needed = int(out.size());
    if (srcLen < 0) ++needed;            /* the terminator is part of the count */
    if (dstLen == 0 || !dst)
        return needed;

    int n = needed < dstLen ? needed : dstLen;
    memcpy(dst, out.data(), size_t(n < int(out.size()) ? n : int(out.size())));
    if (n > int(out.size())) dst[out.size()] = '\0';
    return n;
}

/* ---------------------------------------------------------- environment */

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n) {
    if (!name) { SetLastError(87); return 0; }
    const char* v = getenv(name);
    if (!v) { SetLastError(203); return 0; }   /* ERROR_ENVVAR_NOT_FOUND */

    DWORD len = DWORD(strlen(v));
    if (len + 1 > n) {
        SetLastError(122);
        return len + 1;
    }
    memcpy(buf, v, size_t(len) + 1);
    return len;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD n) {
    std::string narrow = wideToUtf8(name);
    if (n == 0 || !buf) return GetEnvironmentVariableA(narrow.c_str(), nullptr, 0);

    char tmp[4096];
    DWORD len = GetEnvironmentVariableA(narrow.c_str(), tmp, sizeof(tmp));
    if (!len) return 0;

    if (len + 1 > n) { SetLastError(122); return len + 1; }
    for (DWORD i = 0; i <= len; ++i)
        buf[i] = WCHAR(uint8_t(tmp[i]));
    return len;
}

/* ------------------------------------------------------------- console */

void OutputDebugStringA(LPCSTR s) {
#ifdef WC3_DEBUG_GRAPHICS
    if (s) fprintf(stderr, "%s", s);
#else
    (void)s;
#endif
}

void OutputDebugStringW(LPCWSTR s) {
#ifdef WC3_DEBUG_GRAPHICS
    std::string narrow = wideToUtf8(s);
    OutputDebugStringA(narrow.c_str());
#else
    (void)s;
#endif
}

BOOL CreateDirectoryA(LPCSTR path, void* /*sa*/) {
    if (!path) { SetLastError(87); return FALSE; }
    if (mkdir(path, 0777) == 0) return TRUE;
    if (errno == EEXIST) { SetLastError(183); return FALSE; }
    SetLastError(3);   /* ERROR_PATH_NOT_FOUND */
    return FALSE;
}

BOOL CreateDirectoryW(LPCWSTR path, void* sa) {
    std::string narrow = wideToUtf8(path);
    return CreateDirectoryA(narrow.c_str(), sa);
}

/* ------------------------------------------------------------- file I/O
 *
 * DXVK 3 implements dxvk::io on top of these, so the state cache and the
 * shader dump would silently do nothing if they were stubs. */

namespace {

DWORD errnoToWin32(int e) {
    switch (e) {
        case ENOENT:  return 2;      /* ERROR_FILE_NOT_FOUND */
        case EACCES:
        case EPERM:   return 5;      /* ERROR_ACCESS_DENIED */
        case EEXIST:  return 183;    /* ERROR_ALREADY_EXISTS */
        case ENOSPC:  return 112;    /* ERROR_DISK_FULL */
        default:      return 1;      /* ERROR_INVALID_FUNCTION */
    }
}

} // namespace

/* Live file views, so UnmapViewOfFile can recover the length that Windows
 * does not require the caller to pass.  A fixed table is enough: DXVK maps
 * the shader cache and nothing else. */
namespace {

struct ViewRecord { const void* base; size_t length; };
ViewRecord g_views[16];
pthread_mutex_t g_viewLock = PTHREAD_MUTEX_INITIALIZER;

void rememberView(const void* base, size_t length) {
    pthread_mutex_lock(&g_viewLock);
    for (auto& v : g_views) {
        if (!v.base) { v.base = base; v.length = length; break; }
    }
    pthread_mutex_unlock(&g_viewLock);
}

size_t forgetView(const void* base) {
    size_t len = 0;
    pthread_mutex_lock(&g_viewLock);
    for (auto& v : g_views) {
        if (v.base == base) { len = v.length; v.base = nullptr; v.length = 0; break; }
    }
    pthread_mutex_unlock(&g_viewLock);
    return len;
}

} // namespace

/* Attribute test and delete, for the debug flag file hle_d3d9_dxvk.cpp reads
 * ("write this file to ask the next Present to dump a screenshot").  DXVK
 * itself only needs CreateFile/ReadFile/WriteFile. */
DWORD GetFileAttributesA(LPCSTR name) {
    struct stat st;
    if (!name || stat(name, &st) != 0) { SetLastError(2); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? 0x10u /* FILE_ATTRIBUTE_DIRECTORY */ : 0x80u /* NORMAL */;
}

BOOL DeleteFileA(LPCSTR name) {
    if (!name) { SetLastError(87); return FALSE; }
    if (unlink(name) != 0) { SetLastError(errnoToWin32(errno)); return FALSE; }
    return TRUE;
}

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD /*share*/, void* /*sa*/,
                   DWORD disposition, DWORD /*flags*/, HANDLE /*tmpl*/) {
    if (!name) { SetLastError(87); return INVALID_HANDLE_VALUE; }

    int flags = 0;
    bool read  = (access & 0x80000000u) != 0;
    bool write = (access & 0x40000000u) != 0;

    switch (disposition) {
        case 1: /* CREATE_NEW */
            flags = O_CREAT | O_EXCL | O_WRONLY;
            break;
        case 2: /* CREATE_ALWAYS */
            flags = O_CREAT | O_TRUNC | (read && write ? O_RDWR : O_WRONLY);
            break;
        case 3: /* OPEN_EXISTING */
            flags = read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY);
            break;
        case 4: /* OPEN_ALWAYS */
            flags = O_CREAT | (read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY));
            break;
        case 5: /* TRUNCATE_EXISTING */
            flags = O_TRUNC | (read && write ? O_RDWR : O_WRONLY);
            break;
        default:
            flags = O_RDONLY;
            break;
    }

    int fd = open(name, flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        SetLastError(errnoToWin32(errno));
        return INVALID_HANDLE_VALUE;
    }

    Hk* h = hkNew(HK_FILE);
    if (!h) {
        close(fd);
        SetLastError(8);
        return INVALID_HANDLE_VALUE;
    }
    h->fd = fd;
    return (HANDLE)h;
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, void* sa,
                   DWORD disposition, DWORD flags, HANDLE tmpl) {
    std::string narrow = wideToUtf8(name);
    return CreateFileA(name ? narrow.c_str() : nullptr, access, share, sa,
                       disposition, flags, tmpl);
}

BOOL ReadFile(HANDLE file, void* buf, DWORD count, DWORD* read, void* /*ov*/) {
    if (!file || ((Hk*)file)->kind != HK_FILE) { SetLastError(6); return FALSE; }

    ssize_t n = ::read(((Hk*)file)->fd, buf, count);
    if (n < 0) {
        SetLastError(errnoToWin32(errno));
        return FALSE;
    }
    if (read) *read = DWORD(n);
    return TRUE;
}

BOOL WriteFile(HANDLE file, const void* buf, DWORD count, DWORD* written, void* /*ov*/) {
    if (!file || ((Hk*)file)->kind != HK_FILE) { SetLastError(6); return FALSE; }

    const char* p = static_cast<const char*>(buf);
    DWORD total = 0;
    while (total < count) {
        ssize_t n = ::write(((Hk*)file)->fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            SetLastError(errnoToWin32(errno));
            return FALSE;
        }
        total += DWORD(n);
    }
    if (written) *written = total;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE file) {
    if (!file || ((Hk*)file)->kind != HK_FILE) { SetLastError(6); return FALSE; }
    return fsync(((Hk*)file)->fd) == 0 ? TRUE : FALSE;
}

BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER* size) {
    if (!file || !size || ((Hk*)file)->kind != HK_FILE) { SetLastError(6); return FALSE; }

    struct stat st;
    if (fstat(((Hk*)file)->fd, &st) != 0) {
        SetLastError(errnoToWin32(errno));
        return FALSE;
    }
    size->QuadPart = LONGLONG(st.st_size);
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, LARGE_INTEGER* newPos,
                      DWORD method) {
    if (!file || ((Hk*)file)->kind != HK_FILE) { SetLastError(6); return FALSE; }

    int whence = method == 0 ? SEEK_SET : (method == 1 ? SEEK_CUR : SEEK_END);
    off_t off = lseek(((Hk*)file)->fd, off_t(distance.QuadPart), whence);
    if (off == off_t(-1)) {
        SetLastError(errnoToWin32(errno));
        return FALSE;
    }
    if (newPos) newPos->QuadPart = LONGLONG(off);
    return TRUE;
}

/* File mappings.  An mmap of the backing fd is exactly right.  Anonymous
 * pagefile-backed mappings (INVALID_HANDLE_VALUE) are not supported: DXVK's
 * only caller is the 32-bit unmappable-texture allocator, which is compiled
 * out on LP64 targets.  Return an error rather than dereferencing -1. */

HANDLE CreateFileMappingA(HANDLE file, void* /*sa*/, DWORD /*protect*/,
                          DWORD maxHigh, DWORD maxLow, LPCSTR /*name*/) {
    if (!file || file == INVALID_HANDLE_VALUE || ((Hk*)file)->kind != HK_FILE) {
        SetLastError(6);
        return nullptr;
    }

    Hk* h = hkNew(HK_MAPPING);
    if (!h) { SetLastError(8); return nullptr; }

    /* A Windows mapping survives the file handle, so give it its own
     * descriptor instead of sharing one that CloseHandle will close. */
    h->fd = dup(((Hk*)file)->fd);
    if (h->fd < 0) {
        int err = errnoToWin32(errno);
        free(h);
        SetLastError(err);
        return nullptr;
    }
    h->mapLength = (size_t(maxHigh) << 32) | size_t(maxLow);

    /* Windows grows the file to the requested size on demand; do it now so a
     * subsequent mmap of the whole file is large enough. */
    if (h->mapLength) {
        struct stat st;
        if (fstat(h->fd, &st) == 0 && size_t(st.st_size) < h->mapLength)
            (void)ftruncate(h->fd, off_t(h->mapLength));
    }
    return (HANDLE)h;
}

HANDLE OpenFileMappingA(DWORD /*access*/, BOOL /*inherit*/, LPCSTR /*name*/) {
    /* Named shared memory has no POSIX equivalent without a broker. */
    SetLastError(2);
    return nullptr;
}

void* MapViewOfFile(HANDLE map, DWORD access, DWORD offHigh, DWORD offLow,
                    size_t viewSize) {
    if (!map || ((Hk*)map)->kind != HK_MAPPING) { SetLastError(6); return nullptr; }

    Hk* h = (Hk*)map;
    int prot = PROT_READ;
    if (access & (0x0002u | 0x000F001Fu)) prot |= PROT_WRITE;

    off_t offset = off_t((uint64_t(offHigh) << 32) | offLow);
    void* p = mmap(nullptr, viewSize, prot, MAP_SHARED, h->fd, offset);
    if (p == MAP_FAILED) {
        SetLastError(errnoToWin32(errno));
        return nullptr;
    }

    rememberView(p, viewSize);
    return p;
}

BOOL UnmapViewOfFile(const void* base) {
    if (!base) { SetLastError(87); return FALSE; }

    /* munmap needs the length, and Windows' UnmapViewOfFile does not take one,
     * so the length has to be remembered from MapViewOfFile. */
    size_t len = forgetView(base);
    if (!len) { SetLastError(487); return FALSE; }   /* ERROR_INVALID_ADDRESS */
    return munmap(const_cast<void*>(base), len) == 0 ? TRUE : FALSE;
}

BOOL DeviceIoControl(HANDLE /*file*/, DWORD /*code*/, void* /*inBuf*/, DWORD /*inSize*/,
                     void* /*outBuf*/, DWORD /*outSize*/, DWORD* /*returned*/,
                     void* /*overlapped*/) {
    /* Used only by the shared-GPU-resource path, which does not exist here. */
    SetLastError(1);
    return FALSE;
}

/* --------------------------------------------------------- display / GDI
 *
 * There is no window system.  A single synthetic monitor of the configured
 * size is enough for DXVK's adapter enumeration; everything that would move or
 * draw into a window is a no-op. */

namespace {

HMONITOR const kMonitor = (HMONITOR)(intptr_t)1;
HDC      const kDc      = (HDC)(intptr_t)1;
HWND     const kWindow  = (HWND)(intptr_t)1;

void fillMonitorInfo(RECT& rcMonitor, RECT& rcWork, DWORD& flags) {
    uint32_t w, h;
    surfaceSize(w, h);
    rcMonitor.left = 0; rcMonitor.top = 0;
    rcMonitor.right = LONG(w); rcMonitor.bottom = LONG(h);
    rcWork = rcMonitor;
    flags = 0x00000001u;   /* MONITORINFOF_PRIMARY */
}

} // namespace

HMONITOR MonitorFromPoint(POINT, DWORD) { return kMonitor; }
HMONITOR MonitorFromRect(const RECT*, DWORD) { return kMonitor; }
HMONITOR MonitorFromWindow(HWND, DWORD) { return kMonitor; }

BOOL GetMonitorInfoA(HMONITOR, LPMONITORINFO info) {
    if (!info) { SetLastError(87); return FALSE; }
    fillMonitorInfo(info->rcMonitor, info->rcWork, info->dwFlags);
    info->cbSize = sizeof(MONITORINFO);
    return TRUE;
}

BOOL GetMonitorInfoW(HMONITOR m, LPMONITORINFO info) { return GetMonitorInfoA(m, info); }

BOOL EnumDisplayMonitors(HDC, const RECT* clip, MONITORENUMPROC cb, LPARAM data) {
    if (!cb) { SetLastError(87); return FALSE; }

    RECT rc;
    DWORD flags;
    fillMonitorInfo(rc, rc, flags);
    if (clip) rc = *clip;
    return cb(kMonitor, kDc, &rc, data);
}

BOOL EnumDisplayDevicesA(LPCSTR /*device*/, DWORD devNum, DISPLAY_DEVICEA* info, DWORD) {
    if (!info) { SetLastError(87); return FALSE; }
    if (devNum != 0) return FALSE;          /* exactly one adapter */

    memset(info, 0, sizeof(*info));
    info->cb = sizeof(*info);
    strncpy(info->DeviceName, "\\\\.\\DISPLAY1", sizeof(info->DeviceName) - 1);
    strncpy(info->DeviceString, "Android Vulkan", sizeof(info->DeviceString) - 1);
    strncpy(info->DeviceID, "DXVK\\Android", sizeof(info->DeviceID) - 1);
    info->StateFlags = 0x00000001u | 0x00000004u;   /* ATTACHED | PRIMARY */
    return TRUE;
}

BOOL EnumDisplayDevicesW(LPCWSTR device, DWORD devNum, DISPLAY_DEVICEW* info, DWORD flags) {
    if (!info) { SetLastError(87); return FALSE; }

    DISPLAY_DEVICEA a;
    if (!EnumDisplayDevicesA(nullptr, devNum, &a, flags)) return FALSE;

    memset(info, 0, sizeof(*info));
    info->cb = sizeof(*info);
    for (int i = 0; i < 32; ++i)  info->DeviceName[i]   = WCHAR(uint8_t(a.DeviceName[i]));
    for (int i = 0; i < 128; ++i) info->DeviceString[i] = WCHAR(uint8_t(a.DeviceString[i]));
    for (int i = 0; i < 128; ++i) info->DeviceID[i]     = WCHAR(uint8_t(a.DeviceID[i]));
    info->StateFlags = a.StateFlags;
    return TRUE;
}

BOOL EnumDisplaySettingsA(LPCSTR /*device*/, DWORD /*mode*/, DEVMODEA* dm) {
    if (!dm) { SetLastError(87); return FALSE; }

    uint32_t w, h;
    surfaceSize(w, h);

    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    strncpy((char*)dm->dmDeviceName, "Android", sizeof(dm->dmDeviceName) - 1);
    dm->dmBitsPerPel = 32;
    dm->dmPelsWidth  = w;
    dm->dmPelsHeight = h;
    dm->dmDisplayFrequency = 60;
    dm->dmFields = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u;
    return TRUE;
}

BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm) {
    if (!dm) { SetLastError(87); return FALSE; }

    DEVMODEA a;
    if (!EnumDisplaySettingsA(nullptr, mode, &a)) return FALSE;

    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    for (int i = 0; i < 32; ++i) dm->dmDeviceName[i] = WCHAR(uint8_t(((char*)a.dmDeviceName)[i]));
    dm->dmBitsPerPel = a.dmBitsPerPel;
    dm->dmPelsWidth  = a.dmPelsWidth;
    dm->dmPelsHeight = a.dmPelsHeight;
    dm->dmDisplayFrequency = a.dmDisplayFrequency;
    dm->dmFields = a.dmFields;
    return TRUE;
}

LONG ChangeDisplaySettingsExA(LPCSTR, DEVMODEA*, HWND, DWORD, void*) {
    return 0;   /* DISP_CHANGE_SUCCESSFUL */
}

LONG ChangeDisplaySettingsExW(LPCWSTR, DEVMODEW*, HWND, DWORD, void*) {
    return 0;
}

BOOL SetProcessDPIAware(void) { return TRUE; }
BOOL SetProcessDpiAwarenessContext(void* /*ctx*/) { return TRUE; }

/* GDI: no device contexts, no blits.  Returning a non-null DC keeps callers
 * that only pass it around from failing early. */
HDC  CreateCompatibleDC(HDC) { return kDc; }
HDC  GetDCEx(HWND, HRGN, DWORD) { return kDc; }
int  ReleaseDC(HWND, HDC) { return 1; }
BOOL DeleteDC(HDC) { return TRUE; }
BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) { return TRUE; }
BOOL StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) { return TRUE; }
BOOL ClientToScreen(HWND, POINT* pt) { if (pt) { pt->x = 0; pt->y = 0; } return TRUE; }

BOOL DeleteObject(HGDIOBJ) { return TRUE; }

/* Cursor: D3D9 hands the hardware cursor to the window system, which does not
 * exist.  The calls must succeed so the game does not treat it as fatal. */
BOOL    GetCursorPos(LPPOINT pt) { if (pt) { pt->x = 0; pt->y = 0; } return TRUE; }
BOOL    SetCursorPos(int, int) { return TRUE; }
HCURSOR SetCursor(HCURSOR) { return nullptr; }

HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bitsPerPixel, const void* bits) {
    if (w <= 0 || h <= 0) { SetLastError(87); return nullptr; }

    /* The caller keeps the pixel data alive only for the CreateIconIndirect
     * call that follows, so the bits have to be copied to be safe. */
    size_t bytesPerRow = (size_t(w) * planes * bitsPerPixel + 31) / 32 * 4;
    size_t total = bytesPerRow * size_t(h);

    void* copy = malloc(total ? total : 1);
    if (!copy) { SetLastError(8); return nullptr; }
    if (bits) memcpy(copy, bits, total);
    else      memset(copy, 0, total);

    return (HBITMAP)copy;
}

HCURSOR CreateIconIndirect(PICONINFO info) {
    if (!info) { SetLastError(87); return nullptr; }
    /* No cursor exists; hand back a token so DestroyCursor has something to
     * free and the caller does not loop. */
    return (HCURSOR)calloc(1, 1);
}

BOOL DestroyCursor(HCURSOR cursor) { if (cursor) free(cursor); return TRUE; }

/* -------------------------------------------------------------- window */

BOOL IsWindow(HWND wnd) { return wnd == kWindow ? TRUE : FALSE; }
BOOL IsWindowVisible(HWND wnd) { return wnd == kWindow ? TRUE : FALSE; }
BOOL IsWindowUnicode(HWND) { return TRUE; }
BOOL IsIconic(HWND) { return FALSE; }
BOOL IsZoomed(HWND) { return FALSE; }

BOOL GetClientRect(HWND, RECT* rc) {
    if (!rc) { SetLastError(87); return FALSE; }
    uint32_t w, h;
    surfaceSize(w, h);
    rc->left = 0; rc->top = 0; rc->right = LONG(w); rc->bottom = LONG(h);
    return TRUE;
}

BOOL GetWindowRect(HWND, RECT* rc) { return GetClientRect(nullptr, rc); }

BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL SetForegroundWindow(HWND) { return TRUE; }

BOOL PostMessageA(HWND, UINT, WPARAM, LPARAM) { return TRUE; }
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return TRUE; }

LRESULT CallWindowProcA(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!proc) { SetLastError(87); return 0; }
    return proc(wnd, msg, wp, lp);
}

LRESULT CallWindowProcW(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    return CallWindowProcA(proc, wnd, msg, wp, lp);
}

LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }

/* Window longs: DXVK stores its HINSTANCE and user data there.  There is one
 * window, so a flat side table indexed by the GWLP_* constant is enough. */
namespace {

LONG_PTR g_windowLongs[64];

} // namespace

LONG GetWindowLongA(HWND, int index) { return LONG(g_windowLongs[index & 63]); }

LONG GetWindowLongW(HWND, int index) { return GetWindowLongA(nullptr, index); }

LONG SetWindowLongA(HWND, int index, LONG value) {
    LONG prev = LONG(g_windowLongs[index & 63]);
    g_windowLongs[index & 63] = value;
    return prev;
}

LONG SetWindowLongW(HWND wnd, int index, LONG value) { return SetWindowLongA(wnd, index, value); }

LONG_PTR GetWindowLongPtrA(HWND, int index) { return g_windowLongs[index & 63]; }
LONG_PTR GetWindowLongPtrW(HWND wnd, int index) { return GetWindowLongPtrA(wnd, index); }

LONG_PTR SetWindowLongPtrA(HWND, int index, LONG_PTR value) {
    LONG_PTR prev = g_windowLongs[index & 63];
    g_windowLongs[index & 63] = value;
    return prev;
}

LONG_PTR SetWindowLongPtrW(HWND wnd, int index, LONG_PTR value) {
    return SetWindowLongPtrA(wnd, index, value);
}

/* ------------------------------------------------------------ registry
 *
 * Only OpenVR's runtime detection reads the registry, and there is none on
 * Android, so every lookup reports "not found" - which is exactly the answer
 * that makes DXVK skip VR. */

LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, HKEY* result) {
    if (result) *result = nullptr;
    SetLastError(2);
    return 2;
}

LSTATUS RegQueryValueExA(HKEY, LPCSTR, DWORD*, DWORD*, BYTE*, DWORD* size) {
    if (size) *size = 0;
    SetLastError(2);
    return 2;
}

LSTATUS RegCloseKey(HKEY) { return 0; }

LSTATUS RegNotifyChangeKeyValue(HKEY, BOOL, DWORD, HANDLE, BOOL) {
    SetLastError(2);
    return 2;
}

/* ------------------------------------------------------- low memory window
 *
 * DXVK maps host-visible Vulkan memory with 64-bit pointers.  The thin D3D9
 * bridge can give the 32-bit guest that pointer directly only when the
 * mapping is below 4 GB, and VK_EXT_map_memory_placed is what lets the driver
 * map at an address we choose.
 *
 * The window is [0x01000000, 0x31000000).  It starts where war3.exe and its
 * 12 MB image arena end and stops at the main guest stack: every guest arena
 * (stack, thread stacks, Storm/MSS/Mp3dec images, TEB, VA2 and above) lives at
 * 0x31000000 or higher, so the guest's lazy VirtualAlloc bump allocator never
 * maps into this window and cannot clobber a placed mapping later.
 *
 * The window is not empty on Android: ART reserves 0x10980000-0x30980000 in
 * an app process (measured; runtime/guest/guest_layout.h).  That reservation
 * is itself a mapping, so the MAP_FIXED_NOREPLACE probe skips it and only the
 * gaps below it are used - in particular the space below boot.oat and the
 * ~65 MB hole between boot.oat's end and ART's reservation.  Starting the
 * window at 0x10000000 instead would leave only the 9.5 MB below ART, which
 * cannot hold a 64 MB mapped-pool chunk.
 *
 * The probe reserves the range with PROT_NONE and MAP_FIXED_NOREPLACE, then
 * unmaps it again; the driver maps the same range for real.  The thin D3D9
 * staging arena is the only other low-VA allocator and keeps this window in
 * its g_keep table, so nothing else takes the gap between probe and placed
 * map.  On kernels older than 4.17 the flag is ignored, the probe lands
 * elsewhere, and the function reports failure instead of handing out an
 * address the driver was not asked to use.
 */

namespace {

constexpr uintptr_t kLowMemBegin = WC3_LOWMEM_BEGIN;
constexpr uintptr_t kLowMemEnd   = WC3_LOWMEM_END;

std::mutex g_lowMemMutex;
uintptr_t  g_lowMemNext = kLowMemBegin;

uintptr_t lowMemAlign(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace

extern "C" uintptr_t wc3_lowmem_acquire(uint64_t size, uint64_t alignment) {
    if (!size || size > uint64_t(kLowMemEnd - kLowMemBegin))
        return 0;

    if (!alignment)
        alignment = 4096u;
    if (alignment < 4096u || (alignment & (alignment - 1u)) != 0)
        return 0;

    std::lock_guard<std::mutex> lock(g_lowMemMutex);

    /* A 4 KB step over a fragmented window costs tens of thousands of probes;
     * DXVK chunks are at least 4 MB and smaller dedicated mappings can fall
     * back to the high path without losing correctness. */
    const uintptr_t step = alignment < (1u << 20) ? (1u << 20) : alignment;

    for (unsigned pass = 0; pass < 2; pass++) {
        uintptr_t addr = (pass == 0)
            ? lowMemAlign(g_lowMemNext, alignment)
            : kLowMemBegin;
        if (addr < kLowMemBegin)
            addr = kLowMemBegin;

        while (addr + size <= kLowMemEnd) {
            void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
            if (p != MAP_FAILED) {
                munmap(p, size);
                if (reinterpret_cast<uintptr_t>(p) == addr) {
                    g_lowMemNext = addr + size;
                    return addr;
                }
            }
            addr += step;
        }
    }

    g_lowMemNext = kLowMemBegin;
    return 0;
}
