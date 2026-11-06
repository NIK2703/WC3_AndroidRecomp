#include "fake_platform.h"
#include "guest_runner.h"
#include "mailbox.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_starts;
static int g_stops;
static int g_pauses;
static int g_surfaces;
static int g_audios;

static int guest_start(void *opaque)
{
    (void)opaque;
    g_starts++;
    return 0;
}

static void guest_stop(void *opaque)
{
    (void)opaque;
    g_stops++;
}

static void guest_pause(void *opaque, int paused)
{
    (void)opaque;
    (void)paused;
    g_pauses++;
}

static void guest_surface(void *opaque, int available)
{
    (void)opaque;
    (void)available;
    g_surfaces++;
}

static void guest_audio(void *opaque, int available)
{
    (void)opaque;
    (void)available;
    g_audios++;
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

static int worker(void *arg)
{
    int *value = (int *)arg;
    (*value)++;
    return 7;
}

int main(void)
{
    Wc3Platform platform;
    if (wc3_fake_platform_init(&platform) != 0) return fail("fake platform init");
    if (wc3_platform_install(&platform) != 0) return fail("platform install");
    if (wc3_platform() != &platform) return fail("platform get");

    Wc3RunnerConfig cfg = {
        .platform = &platform,
        .guest_start = guest_start,
        .guest_stop = guest_stop,
        .guest_pause = guest_pause,
        .guest_surface = guest_surface,
        .guest_audio = guest_audio,
        .opaque = NULL,
    };
    Wc3GuestRunner runner;
    if (wc3_runner_init(&runner, &cfg) != 0) return fail("runner init");
    if (wc3_runner_state(&runner) != WC3_RUNNER_CREATED) return fail("initial state");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_PAUSE) == 0)
        return fail("pause before start accepted");

    uint64_t self = platform.thread.self(platform.thread.ctx);
    wc3_runner_bind_owner(&runner, self);

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_START) != 0) return fail("start");
    if (wc3_runner_state(&runner) != WC3_RUNNER_RUNNING) return fail("running state");
    if (g_starts != 1) return fail("start callback");

    if (wc3_runner_post(&runner, WC3_RUNNER_EV_PAUSE) != 0) return fail("post pause");
    if (wc3_runner_pump(&runner) != 1) return fail("pump pause");
    if (wc3_runner_state(&runner) != WC3_RUNNER_PAUSED) return fail("paused state");
    if (g_pauses != 1) return fail("pause callback");

    if (wc3_runner_post(&runner, WC3_RUNNER_EV_RESUME) != 0) return fail("post resume");
    if (wc3_runner_pump(&runner) != 1) return fail("pump resume");
    if (wc3_runner_state(&runner) != WC3_RUNNER_RUNNING) return fail("resumed state");
    if (g_pauses != 2) return fail("resume callback");

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_SURFACE_LOST) != 0)
        return fail("surface lost");
    if (wc3_runner_state(&runner) != WC3_RUNNER_SURFACE_LOST)
        return fail("surface lost state");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_SURFACE_RESTORED) != 0)
        return fail("surface restored");
    if (wc3_runner_state(&runner) != WC3_RUNNER_RUNNING)
        return fail("surface restored state");
    if (g_surfaces != 2) return fail("surface callbacks");

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_PAUSE) != 0) return fail("pause 2");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_SURFACE_LOST) != 0)
        return fail("surface lost while paused");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_RESUME) == 0)
        return fail("resume accepted while surface lost");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_SURFACE_RESTORED) != 0)
        return fail("surface restored to pause");
    if (wc3_runner_state(&runner) != WC3_RUNNER_PAUSED)
        return fail("restore did not preserve pause");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_RESUME) != 0) return fail("resume 2");

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_AUDIO_DEVICE_LOST) != 0)
        return fail("audio device lost");
    if (wc3_runner_state(&runner) != WC3_RUNNER_AUDIO_DEVICE_LOST)
        return fail("audio device lost state");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_AUDIO_DEVICE_RESTORED) != 0)
        return fail("audio device restored");
    if (wc3_runner_state(&runner) != WC3_RUNNER_RUNNING)
        return fail("audio device restored state");
    if (g_audios != 2) return fail("audio callbacks");

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_PAUSE) != 0) return fail("pause for audio");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_AUDIO_DEVICE_LOST) != 0)
        return fail("audio lost while paused");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_AUDIO_DEVICE_RESTORED) != 0)
        return fail("audio restored while paused");
    if (wc3_runner_state(&runner) != WC3_RUNNER_PAUSED)
        return fail("audio restore did not preserve pause");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_RESUME) != 0) return fail("resume after audio");

    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_STOP) != 0) return fail("stop");
    if (wc3_runner_state(&runner) != WC3_RUNNER_STOPPING) return fail("stopping state");
    if (g_stops != 1) return fail("stop callback");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_FINISHED) != 0) return fail("finished");
    if (wc3_runner_state(&runner) != WC3_RUNNER_STOPPED) return fail("stopped state");
    if (wc3_runner_apply(&runner, WC3_RUNNER_EV_START) == 0)
        return fail("start after stop accepted");

    Wc3GuestRunner guarded;
    if (wc3_runner_init(&guarded, &cfg) != 0) return fail("guarded init");
    wc3_runner_bind_owner(&guarded, self + 1u);
    if (wc3_runner_post(&guarded, WC3_RUNNER_EV_START) != 0) return fail("guarded post");
    if (wc3_runner_pump(&guarded) != -2) return fail("wrong owner accepted");
    if (wc3_runner_state(&guarded) != WC3_RUNNER_CREATED)
        return fail("wrong owner changed state");
    wc3_runner_bind_owner(&guarded, self);
    if (wc3_runner_pump(&guarded) != 1) return fail("owner retry pump");
    if (wc3_runner_state(&guarded) != WC3_RUNNER_RUNNING) return fail("owner retry state");

    Wc3Mailbox mb;
    wc3_mailbox_init(&mb);
    Wc3MailboxMsg msg = {1u, 2u, 3u};
    for (uint32_t i = 0; i < WC3_MAILBOX_CAP - 1u; i++) {
        if (!wc3_mailbox_push(&mb, &msg)) return fail("mailbox fill");
    }
    if (wc3_mailbox_push(&mb, &msg)) return fail("mailbox overflow accepted");
    if (wc3_mailbox_count(&mb) != WC3_MAILBOX_CAP - 1u) return fail("mailbox count");
    for (uint32_t i = 0; i < WC3_MAILBOX_CAP - 1u; i++) {
        if (!wc3_mailbox_pop(&mb, &msg)) return fail("mailbox drain");
    }
    if (wc3_mailbox_pop(&mb, &msg)) return fail("mailbox not empty");
    if (wc3_mailbox_count(&mb) != 0) return fail("mailbox count after drain");

    int value = 0;
    Wc3ThreadHandle thread = 0;
    Wc3ThreadDesc desc = {"worker", 0, worker, &value};
    if (platform.thread.create(platform.thread.ctx, &desc, &thread) != 0)
        return fail("thread create");
    uint32_t exit_code = 0;
    if (platform.thread.join(platform.thread.ctx, thread, &exit_code) != 0)
        return fail("thread join");
    if (value != 1) return fail("thread did not run");
    if (exit_code != 7u) return fail("thread exit code");

    if (strcmp(wc3_runner_state_name(WC3_RUNNER_SURFACE_LOST), "SurfaceLost") != 0)
        return fail("state name");
    if (strcmp(wc3_runner_state_name(WC3_RUNNER_AUDIO_DEVICE_LOST),
               "AudioDeviceLost") != 0)
        return fail("audio state name");

    puts("guest runner OK");
    return 0;
}
