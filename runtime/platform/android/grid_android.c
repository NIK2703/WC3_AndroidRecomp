#include "grid_android.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "grid_pattern.h"

/*
 * State is owned by the UI thread, except g_phase, which the cycle worker
 * advances.  The worker acquires the window under the lock and posts after
 * unlocking: ANativeWindow_lock can block on the BufferQueue, and that wait
 * must never extend an unrelated launch-settings or surface callback.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond;
static pthread_once_t g_cond_once = PTHREAD_ONCE_INIT;
static clockid_t g_cond_clock = CLOCK_REALTIME;

static ANativeWindow *g_window;
static int g_enabled;
static int g_darken_percent;
static int g_cycle_seconds;
static int g_phase;
/* False between onStop and onStart.  A background window may stop consuming
 * its BufferQueue, so posting there could block the UI thread in the join;
 * the cycle is meaningful only while the grid is actually visible. */
static int g_active = 1;
static int g_worker_started;
static int g_worker_stop;
static pthread_t g_worker;

static void grid_cond_init(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
        g_cond_clock = CLOCK_MONOTONIC;
    }
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
}

static void grid_cond_ensure(void)
{
    pthread_once(&g_cond_once, grid_cond_init);
}

/*
 * Rasterise one phase into the buffer and post it.  The pattern starts at the
 * buffer origin, so the cell anchor does not move with the surface origin.
 * Black is zero in premultiplied and straight alpha alike; only the alpha byte
 * changes.
 */
static void grid_post(ANativeWindow *window, int darken_percent, int phase)
{
    if (!window) return;

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) return;
    if (buffer.bits && buffer.stride > 0 && buffer.width > 0 &&
        buffer.height > 0) {
        const uint32_t dark =
            (uint32_t)grid_pattern_alpha(darken_percent) << 24;
        const int hole_x = grid_pattern_hole_x(phase);
        const int hole_y = grid_pattern_hole_y(phase);

        for (int y = 0; y < buffer.height; y++) {
            uint32_t *row = (uint32_t *)buffer.bits +
                            (size_t)y * (size_t)buffer.stride;
            for (int x = 0; x < buffer.width; x++) row[x] = dark;
            if ((y & 1) == hole_y) {
                for (int x = hole_x; x < buffer.width; x += 2) row[x] = 0;
            }
        }
    }
    ANativeWindow_unlockAndPost(window);
}

static void *grid_worker_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    while (!g_worker_stop) {
        const int cycle = g_cycle_seconds;
        if (!g_active || !g_enabled || g_darken_percent <= 0 || cycle <= 0 ||
            !g_window)
            break;

        struct timespec deadline;
        clock_gettime(g_cond_clock, &deadline);
        deadline.tv_sec += cycle;

        const int rc = pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
        if (g_worker_stop) break;
        /* Any wake that is not the interval boundary is a settings change;
         * recompute the cycle from the shared state. */
        if (rc != ETIMEDOUT) continue;

        g_phase = (g_phase + 1) % GRID_PATTERN_PHASES;
        ANativeWindow *window = g_window;
        const int darken = g_darken_percent;
        const int phase = g_phase;
        if (window) ANativeWindow_acquire(window);
        pthread_mutex_unlock(&g_lock);

        if (window) {
            grid_post(window, darken, phase);
            ANativeWindow_release(window);
        }

        pthread_mutex_lock(&g_lock);
    }
    g_worker_started = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void grid_worker_start_locked(void)
{
    if (g_worker_started || !g_active || !g_enabled || g_darken_percent <= 0 ||
        g_cycle_seconds <= 0 || !g_window) {
        return;
    }
    grid_cond_ensure();
    g_worker_stop = 0;
    if (pthread_create(&g_worker, NULL, grid_worker_main, NULL) == 0) {
        g_worker_started = 1;
    }
}

/* The UI thread is the only caller, so two joins cannot race. */
static void grid_worker_stop(void)
{
    pthread_t thread;
    int started;

    pthread_mutex_lock(&g_lock);
    started = g_worker_started;
    if (started) {
        g_worker_stop = 1;
        grid_cond_ensure();
        pthread_cond_broadcast(&g_cond);
    }
    thread = g_worker;
    pthread_mutex_unlock(&g_lock);

    if (started) pthread_join(thread, NULL);

    pthread_mutex_lock(&g_lock);
    g_worker_stop = 0;
    pthread_mutex_unlock(&g_lock);
}

void grid_android_configure(int enabled, int darken_percent, int cycle_seconds)
{
    if (darken_percent < 0) darken_percent = 0;
    if (darken_percent > 100) darken_percent = 100;
    if (cycle_seconds < 0) cycle_seconds = 0;
    if (cycle_seconds > 3600) cycle_seconds = 3600;
    enabled = enabled != 0;

    grid_cond_ensure();
    grid_worker_stop();

    ANativeWindow *window = NULL;
    int active;
    int phase;
    int darken;
    int changed;

    pthread_mutex_lock(&g_lock);
    changed = g_enabled != enabled ||
              g_darken_percent != darken_percent ||
              g_cycle_seconds != cycle_seconds;
    g_enabled = enabled;
    g_darken_percent = darken_percent;
    g_cycle_seconds = cycle_seconds;
    if (changed) g_phase = 0;
    active = g_active;
    if (changed && active && g_window) {
        window = g_window;
        ANativeWindow_acquire(window);
    }
    phase = g_phase;
    darken = g_darken_percent;
    pthread_mutex_unlock(&g_lock);

    if (window) {
        grid_post(window, darken, phase);
        ANativeWindow_release(window);
    }

    pthread_mutex_lock(&g_lock);
    grid_worker_start_locked();
    pthread_mutex_unlock(&g_lock);
}

/*
 * Start/stop presentation for the activity's visible lifetime.  The source
 * Handler keeps ticking in the background because an invalidated View costs
 * nothing there; a native Surface producer cannot: a window that stops being
 * consumed fills its BufferQueue and the next lock blocks.  Resuming reposts
 * the current phase before restarting the interval.
 */
void grid_android_set_active(int active)
{
    active = active != 0;
    grid_cond_ensure();

    if (!active) {
        grid_worker_stop();
        pthread_mutex_lock(&g_lock);
        g_active = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    ANativeWindow *window = NULL;
    int enabled;
    int darken;
    int phase;

    pthread_mutex_lock(&g_lock);
    g_active = 1;
    enabled = g_enabled;
    darken = g_darken_percent;
    phase = g_phase;
    if (enabled && darken > 0 && g_window) {
        window = g_window;
        ANativeWindow_acquire(window);
    }
    pthread_mutex_unlock(&g_lock);

    if (window) {
        grid_post(window, darken, phase);
        ANativeWindow_release(window);
    }

    pthread_mutex_lock(&g_lock);
    grid_worker_start_locked();
    pthread_mutex_unlock(&g_lock);
}

int grid_android_set_surface(ANativeWindow *window, int width, int height)
{
    if (!window) return -1;
    if (width <= 0) width = ANativeWindow_getWidth(window);
    if (height <= 0) height = ANativeWindow_getHeight(window);
    if (width <= 0 || height <= 0) {
        ANativeWindow_release(window);
        return -1;
    }

    grid_cond_ensure();
    grid_worker_stop();

    if (ANativeWindow_setBuffersGeometry(window, width, height,
                                         WINDOW_FORMAT_RGBA_8888) != 0) {
        ANativeWindow_release(window);
        return -1;
    }

    ANativeWindow *previous;
    int active;
    int enabled;
    int darken;
    int phase;

    pthread_mutex_lock(&g_lock);
    previous = g_window;
    g_window = window;
    active = g_active;
    enabled = g_enabled;
    darken = g_darken_percent;
    phase = g_phase;
    pthread_mutex_unlock(&g_lock);
    if (previous) ANativeWindow_release(previous);

    if (active && enabled && darken > 0) {
        grid_post(window, darken, phase);
        pthread_mutex_lock(&g_lock);
        grid_worker_start_locked();
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

void grid_android_clear_surface(void)
{
    grid_worker_stop();

    ANativeWindow *previous;
    pthread_mutex_lock(&g_lock);
    previous = g_window;
    g_window = NULL;
    pthread_mutex_unlock(&g_lock);
    if (previous) ANativeWindow_release(previous);
}
