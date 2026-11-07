#include "input_wake.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct Wc3InputWake {
    HANDLE event;
};

Wc3InputWake *input_wake_create(void)
{
    Wc3InputWake *wake = (Wc3InputWake *)calloc(1, sizeof(*wake));
    if (!wake) return NULL;
    wake->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!wake->event) {
        free(wake);
        return NULL;
    }
    return wake;
}

void input_wake_destroy(Wc3InputWake *wake)
{
    if (!wake) return;
    if (wake->event) CloseHandle(wake->event);
    free(wake);
}

void input_wake_signal(Wc3InputWake *wake)
{
    if (wake && wake->event) SetEvent(wake->event);
}

int input_wake_wait(Wc3InputWake *wake, int timeout_ms)
{
    if (!wake || !wake->event) return -1;
    DWORD timeout = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    DWORD rc = WaitForSingleObject(wake->event, timeout);
    if (rc == WAIT_OBJECT_0) return 1;
    if (rc == WAIT_TIMEOUT) return 0;
    return -1;
}

void input_wake_drain(Wc3InputWake *wake)
{
    /* The auto-reset event is consumed by a successful wait. */
    (void)wake;
}

#elif defined(__linux__)

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct Wc3InputWake {
    int fd;
};

Wc3InputWake *input_wake_create(void)
{
    Wc3InputWake *wake = (Wc3InputWake *)calloc(1, sizeof(*wake));
    if (!wake) return NULL;
    wake->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake->fd < 0) {
        free(wake);
        return NULL;
    }
    return wake;
}

void input_wake_destroy(Wc3InputWake *wake)
{
    if (!wake) return;
    if (wake->fd >= 0) close(wake->fd);
    free(wake);
}

void input_wake_signal(Wc3InputWake *wake)
{
    if (!wake || wake->fd < 0) return;
    uint64_t one = 1;
    ssize_t n = write(wake->fd, &one, sizeof(one));
    (void)n; /* EAGAIN means the counter is already nonzero. */
}

int input_wake_wait(Wc3InputWake *wake, int timeout_ms)
{
    if (!wake || wake->fd < 0) return -1;
    struct pollfd pfd;
    pfd.fd = wake->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc > 0) {
        input_wake_drain(wake);
        return 1;
    }
    if (rc == 0) return 0;
    return -1;
}

void input_wake_drain(Wc3InputWake *wake)
{
    if (!wake || wake->fd < 0) return;
    uint64_t value;
    while (read(wake->fd, &value, sizeof(value)) > 0) {
    }
}

#else

#include <stdlib.h>

struct Wc3InputWake {
    int unused;
};

Wc3InputWake *input_wake_create(void)
{
    return (Wc3InputWake *)calloc(1, sizeof(Wc3InputWake));
}

void input_wake_destroy(Wc3InputWake *wake) { free(wake); }
void input_wake_signal(Wc3InputWake *wake) { (void)wake; }
int input_wake_wait(Wc3InputWake *wake, int timeout_ms)
{
    (void)wake;
    (void)timeout_ms;
    return 0;
}
void input_wake_drain(Wc3InputWake *wake) { (void)wake; }

#endif
