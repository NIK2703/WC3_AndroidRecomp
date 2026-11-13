#ifndef WC3_TEST_ANDROID_LOG_H
#define WC3_TEST_ANDROID_LOG_H

#include <stdarg.h>

#define ANDROID_LOG_INFO 4

static inline int __android_log_print(int prio, const char *tag,
                                      const char *fmt, ...) {
    (void)prio;
    (void)tag;
    (void)fmt;
    return 0;
}

#endif /* WC3_TEST_ANDROID_LOG_H */
