#ifndef WC3_DEBUG
#define WC3_DEBUG 0
#endif

#include "guest_runner.h"

#include <stddef.h>

#if WC3_DEBUG
#include <stdio.h>
#endif

static int notify_pause(Wc3GuestRunner *r, int paused)
{
    if (r->config.guest_pause) r->config.guest_pause(r->config.opaque, paused);
    return 0;
}

static int notify_surface(Wc3GuestRunner *r, int available)
{
    if (r->config.guest_surface) r->config.guest_surface(r->config.opaque, available);
    return 0;
}

static int notify_audio(Wc3GuestRunner *r, int available)
{
    if (r->config.guest_audio) r->config.guest_audio(r->config.opaque, available);
    return 0;
}

int wc3_runner_init(Wc3GuestRunner *runner, const Wc3RunnerConfig *config)
{
    if (!runner || !config) return -1;
    runner->state = WC3_RUNNER_CREATED;
    runner->resume_state = WC3_RUNNER_CREATED;
    runner->config = *config;
    wc3_mailbox_init(&runner->mailbox);
    runner->owner_thread = 0;
    runner->owner_bound = 0;
    return 0;
}

static int transition(Wc3GuestRunner *r, uint32_t event)
{
    switch (event) {
    case WC3_RUNNER_EV_START:
        if (r->state != WC3_RUNNER_CREATED) return -1;
        if (r->config.guest_start && r->config.guest_start(r->config.opaque) != 0) {
            r->state = WC3_RUNNER_STOPPED;
            return -1;
        }
        r->state = WC3_RUNNER_RUNNING;
        return 0;

    case WC3_RUNNER_EV_PAUSE:
        if (r->state != WC3_RUNNER_RUNNING) return -1;
        r->state = WC3_RUNNER_PAUSED;
        notify_pause(r, 1);
        return 0;

    case WC3_RUNNER_EV_RESUME:
        if (r->state != WC3_RUNNER_PAUSED) return -1;
        r->state = WC3_RUNNER_RUNNING;
        notify_pause(r, 0);
        return 0;

    case WC3_RUNNER_EV_SURFACE_LOST:
        if (r->state != WC3_RUNNER_RUNNING && r->state != WC3_RUNNER_PAUSED)
            return -1;
        r->resume_state = r->state;
        r->state = WC3_RUNNER_SURFACE_LOST;
        notify_surface(r, 0);
        return 0;

    case WC3_RUNNER_EV_SURFACE_RESTORED:
        if (r->state != WC3_RUNNER_SURFACE_LOST) return -1;
        r->state = (r->resume_state == WC3_RUNNER_PAUSED)
                       ? WC3_RUNNER_PAUSED
                       : WC3_RUNNER_RUNNING;
        notify_surface(r, 1);
        return 0;

    case WC3_RUNNER_EV_AUDIO_DEVICE_LOST:
        if (r->state != WC3_RUNNER_RUNNING && r->state != WC3_RUNNER_PAUSED)
            return -1;
        r->resume_state = r->state;
        r->state = WC3_RUNNER_AUDIO_DEVICE_LOST;
        notify_audio(r, 0);
        return 0;

    case WC3_RUNNER_EV_AUDIO_DEVICE_RESTORED:
        if (r->state != WC3_RUNNER_AUDIO_DEVICE_LOST) return -1;
        r->state = (r->resume_state == WC3_RUNNER_PAUSED)
                       ? WC3_RUNNER_PAUSED
                       : WC3_RUNNER_RUNNING;
        notify_audio(r, 1);
        return 0;

    case WC3_RUNNER_EV_STOP:
        if (r->state != WC3_RUNNER_RUNNING &&
            r->state != WC3_RUNNER_PAUSED &&
            r->state != WC3_RUNNER_SURFACE_LOST &&
            r->state != WC3_RUNNER_AUDIO_DEVICE_LOST)
            return -1;
        r->state = WC3_RUNNER_STOPPING;
        if (r->config.guest_stop) r->config.guest_stop(r->config.opaque);
        return 0;

    case WC3_RUNNER_EV_FINISHED:
        if (r->state != WC3_RUNNER_STOPPING) return -1;
        r->state = WC3_RUNNER_STOPPED;
        return 0;

    case WC3_RUNNER_EV_CANCEL:
        if (r->state != WC3_RUNNER_CREATED) return -1;
        r->state = WC3_RUNNER_STOPPED;
        return 0;

    default:
        return -1;
    }
}

int wc3_runner_apply(Wc3GuestRunner *runner, uint32_t event)
{
    if (!runner) return -1;
    int rc = transition(runner, event);
#if WC3_DEBUG
    if (rc != 0)
        fprintf(stderr, "[runner] rejected event %u in state %s\n",
                (unsigned)event, wc3_runner_state_name(runner->state));
#endif
    return rc;
}

int wc3_runner_post(Wc3GuestRunner *runner, uint32_t event)
{
    if (!runner) return -1;
    Wc3MailboxMsg msg = {event, 0u, 0u};
    return wc3_mailbox_push(&runner->mailbox, &msg) ? 0 : -1;
}

int wc3_runner_pump(Wc3GuestRunner *runner)
{
    if (!runner) return -1;
    if (runner->owner_bound && runner->config.platform &&
        runner->config.platform->thread.self) {
        uint64_t self = runner->config.platform->thread.self(
            runner->config.platform->thread.ctx);
        if (self != runner->owner_thread) return -2;
    }

    int applied = 0;
    int rejected = 0;
    Wc3MailboxMsg msg;
    while (wc3_mailbox_pop(&runner->mailbox, &msg)) {
        if (transition(runner, msg.kind) == 0) {
            applied++;
        } else {
            rejected++;
        }
    }
    return rejected ? -1 : applied;
}

void wc3_runner_bind_owner(Wc3GuestRunner *runner, uint64_t thread_id)
{
    if (!runner) return;
    runner->owner_thread = thread_id;
    runner->owner_bound = 1;
}

Wc3RunnerState wc3_runner_state(const Wc3GuestRunner *runner)
{
    return runner ? runner->state : WC3_RUNNER_STOPPED;
}

const char *wc3_runner_state_name(Wc3RunnerState state)
{
    switch (state) {
    case WC3_RUNNER_CREATED: return "Created";
    case WC3_RUNNER_RUNNING: return "Running";
    case WC3_RUNNER_PAUSED: return "Paused";
    case WC3_RUNNER_SURFACE_LOST: return "SurfaceLost";
    case WC3_RUNNER_AUDIO_DEVICE_LOST: return "AudioDeviceLost";
    case WC3_RUNNER_STOPPING: return "Stopping";
    case WC3_RUNNER_STOPPED: return "Stopped";
    default: return "?";
    }
}
