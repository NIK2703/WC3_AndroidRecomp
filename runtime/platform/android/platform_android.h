#ifndef WC3_PLATFORM_ANDROID_H
#define WC3_PLATFORM_ANDROID_H

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fill the core Android platform table (memory, file, time, threads, sync,
 * surface, audio, config, asset roots and logging).  The activity layer
 * installs the remaining window/input/clipboard/Vulkan entries before guest
 * startup.
 *
 * Returns 0 on success.  The caller owns the table and passes it to
 * wc3_platform_install() exactly once.
 */
int wc3_android_platform_init(Wc3Platform *platform);

/* Install the socket half of the platform table.  Kept separate from the core
 * init so the net translation unit has no dependency on activity state. */
void wc3_android_net_install(Wc3NetApi *net);

/*
 * Read the optional app-private config file once.  The format is documented
 * at the implementation.  A missing file is an empty configuration and
 * returns 0; a second call returns -1 to preserve ConfigApi pointer stability.
 */
int wc3_android_config_load(const char *path);

struct ANativeWindow;

/*
 * Publish the activity's current ANativeWindow to SurfaceApi.  The platform
 * does not take ownership; passing NULL on TERM marks the surface lost.  This
 * is called from the GameActivity callback thread, not from guest code.
 */
void wc3_android_surface_set_window(struct ANativeWindow *window);

/*
 * Input mailbox consumer.  Returns 1 and fills *event when one event is
 * available, 0 when the queue is empty, or -1 on an invalid argument.  The
 * queue has exactly one producer (the GameActivity callback thread) and one
 * consumer (the owning guest thread); the consumer must be the only caller.
 * The platform test and future guest loop use this function.
 */
int wc3_android_input_pop(Wc3InputEvent *event);

/* Number of input events rejected because the mailbox was full. */
uint32_t wc3_android_input_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* WC3_PLATFORM_ANDROID_H */
