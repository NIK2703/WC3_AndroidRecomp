#include "platform.h"

static const Wc3Platform *g_platform;

int wc3_platform_install(const Wc3Platform *platform)
{
    if (!platform || g_platform) return -1;
    g_platform = platform;
    return 0;
}

const Wc3Platform *wc3_platform(void)
{
    return g_platform;
}
