/*
 * dxvk_probe_cache_test.c - O-25 T0 contract for the Vulkan probe memo.
 *
 * The fake __real function counts calls and changes its answer.  The caller
 * uses the same undefined symbol and linker --wrap option as the product, so
 * a second call must return the first answer without entering the probe again.
 */
#include <stdio.h>

/* Caller-facing name: the linker redirects this to __wrap_... in the test.
 * The original probe lives in dxvk_probe_cache_real_stub.c so this call stays
 * undefined until the linker applies --wrap, as it does for the product. */
extern int wc3_vulkan_major_minor(int *major, int *minor);
extern unsigned g_probe_calls;

static int fail(const char *message)
{
    fprintf(stderr, "dxvk_probe_cache: %s\n", message);
    return 1;
}

int main(void)
{
    int major = -1;
    int minor = -1;

    if (!wc3_vulkan_major_minor(&major, &minor))
        return fail("first probe reported failure");
    if (g_probe_calls != 1u)
        return fail("first probe did not call the real probe exactly once");
    if (major != 3 || minor != 1)
        return fail("first probe lost the physical-device version");

    major = -1;
    minor = -1;
    if (!wc3_vulkan_major_minor(&major, &minor))
        return fail("cached probe reported failure");
    if (g_probe_calls != 1u)
        return fail("second probe called the real probe again");
    if (major != 3 || minor != 1)
        return fail("cached probe changed the version");

    /* The as-built probe tolerates null outputs; the wrapper must too. */
    if (!wc3_vulkan_major_minor(NULL, NULL))
        return fail("null outputs changed the result");
    if (g_probe_calls != 1u)
        return fail("null-output call re-ran the probe");

    printf("dxvk_probe_cache: cached after one physical-device probe\n");
    return 0;
}
