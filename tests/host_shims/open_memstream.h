#ifndef WC3_TEST_OPEN_MEMSTREAM_H
#define WC3_TEST_OPEN_MEMSTREAM_H

#include <stddef.h>
#include <stdio.h>

/* MinGW has no open_memstream().  The vendored JSON serializer references it,
 * but the touch test never serializes a profile. */
FILE *wc3_host_open_memstream(char **ptr, size_t *sizeloc);

#endif /* WC3_TEST_OPEN_MEMSTREAM_H */
