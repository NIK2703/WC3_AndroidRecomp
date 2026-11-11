/*
 * dxvk3_gate_test.c - T0 contract for the DXVK 3.1.1 Turnip policy.
 *
 * The R8 package reports Turnip 25.99.99 and has the ir3 descriptor-prefetch
 * bug.  The policy must classify that package for the nodescprefetch
 * workaround, remain neutral for the system driver, and leave newer packages
 * untouched.
 */
#include "dxvk3_gate.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "dxvk3_gate: FAIL: %s\n", msg); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* No active package id: the system driver is outside this policy. */
    CHECK(!wc3_dxvk3_package_needs_nodescprefetch(NULL, 1, 0x06463063u),
          "NULL active id needs no workaround");
    CHECK(!wc3_dxvk3_package_needs_nodescprefetch("", 1, 0x06463063u),
          "empty active id needs no workaround");

    /* A failed caps probe is conservative for an active package. */
    CHECK(wc3_dxvk3_package_needs_nodescprefetch("fdd378520022f88b", 0, 0),
          "active package without caps needs the workaround");

    /* The package that produced the crash reports Turnip 25.99.99. */
    CHECK(wc3_dxvk3_package_needs_nodescprefetch("fdd378520022f88b", 1,
                                                 0x06463063u),
          "Turnip 25.99.99 needs the workaround");

    /* Turnip 26.0.0 is the first known-good package boundary. */
    CHECK(!wc3_dxvk3_package_needs_nodescprefetch("fdd378520022f88b", 1,
                                                  0x06800000u),
          "Turnip 26.0.0 needs no workaround");
    CHECK(!wc3_dxvk3_package_needs_nodescprefetch("fdd378520022f88b", 1,
                                                  0x06826363u),
          "Turnip 26.2.99 needs no workaround");

    /* The workaround is an ir3 debug flag, not a DXVK version switch. */
    CHECK(strcmp(WC3_TURNIP_SHADER_DEBUG_ENV, "IR3_SHADER_DEBUG") == 0,
          "unexpected driver debug env name");
    CHECK(strcmp(WC3_TURNIP_NODESCPREFETCH, "nodescprefetch") == 0,
          "unexpected descriptor-prefetch flag");

    printf("dxvk3_gate: OK\n");
    return 0;
}
