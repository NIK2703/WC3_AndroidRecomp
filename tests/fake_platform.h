/*
 * fake_platform.h - host-only platform used by the static tests.
 *
 * The fake implements guest thread creation with real host threads so the
 * runner test catches a fake handle that never runs.  It is not part of the
 * Android link graph.
 */
#ifndef WC3_FAKE_PLATFORM_H
#define WC3_FAKE_PLATFORM_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

int wc3_fake_platform_init(Wc3Platform *platform);

#ifdef __cplusplus
}
#endif

#endif /* WC3_FAKE_PLATFORM_H */
