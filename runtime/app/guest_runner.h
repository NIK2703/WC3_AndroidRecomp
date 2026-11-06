/*
 * guest_runner.h - Android lifecycle owner for the lifted guest.
 *
 * The runner is not a second scheduler and does not create an extra host
 * runtime thread.  Existing guest threads stay where they are; this object
 * only tracks the Android lifecycle and routes UI/surface events to the guest
 * thread that owns them.
 */
#ifndef WC3_GUEST_RUNNER_H
#define WC3_GUEST_RUNNER_H

#include <stdint.h>

#include "mailbox.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Wc3RunnerState {
    WC3_RUNNER_CREATED = 0,
    WC3_RUNNER_RUNNING,
    WC3_RUNNER_PAUSED,
    WC3_RUNNER_SURFACE_LOST,
    WC3_RUNNER_AUDIO_DEVICE_LOST,
    WC3_RUNNER_STOPPING,
    WC3_RUNNER_STOPPED
} Wc3RunnerState;

typedef struct Wc3RunnerConfig {
    const Wc3Platform *platform;
    /* Guest entry setup/teardown.  These run on the owner thread. */
    int (*guest_start)(void *opaque);
    void (*guest_stop)(void *opaque);
    void (*guest_pause)(void *opaque, int paused);
    void (*guest_surface)(void *opaque, int available);
    /* Audio device loss is independent of the surface: the guest may keep
     * simulating while the sink is reopened or switched to a silent sink. */
    void (*guest_audio)(void *opaque, int available);
    void *opaque;
} Wc3RunnerConfig;

typedef struct Wc3GuestRunner {
    Wc3RunnerState state;
    Wc3RunnerState resume_state;
    Wc3RunnerConfig config;
    Wc3Mailbox mailbox;
    uint64_t owner_thread;
    int owner_bound;
} Wc3GuestRunner;

enum {
    WC3_RUNNER_EV_START = 1,
    WC3_RUNNER_EV_PAUSE,
    WC3_RUNNER_EV_RESUME,
    WC3_RUNNER_EV_SURFACE_LOST,
    WC3_RUNNER_EV_SURFACE_RESTORED,
    WC3_RUNNER_EV_AUDIO_DEVICE_LOST,
    WC3_RUNNER_EV_AUDIO_DEVICE_RESTORED,
    WC3_RUNNER_EV_STOP,
    WC3_RUNNER_EV_FINISHED,
    WC3_RUNNER_EV_CANCEL
};

int wc3_runner_init(Wc3GuestRunner *runner, const Wc3RunnerConfig *config);
/* Same-thread transition.  Returns 0 or -1 for an invalid transition. */
int wc3_runner_apply(Wc3GuestRunner *runner, uint32_t event);
/* Producer side: Android callback posts, owner thread pumps. */
int wc3_runner_post(Wc3GuestRunner *runner, uint32_t event);
int wc3_runner_pump(Wc3GuestRunner *runner);
void wc3_runner_bind_owner(Wc3GuestRunner *runner, uint64_t thread_id);
Wc3RunnerState wc3_runner_state(const Wc3GuestRunner *runner);
const char *wc3_runner_state_name(Wc3RunnerState state);

#ifdef __cplusplus
}
#endif

#endif /* WC3_GUEST_RUNNER_H */
