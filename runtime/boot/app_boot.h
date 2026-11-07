/*
 * app_boot.h - Android product entry into the lifted corpus.
 *
 * This is the boundary between GameActivity's lifecycle/window and the guest
 * execution path.  It is deliberately thin: the boot probe owns guest memory,
 * PE loading, HLE binding and the dispatcher; this layer only publishes the
 * real ANativeWindow and the licensed game directory before handing control
 * over.
 */
#ifndef WC3_APP_BOOT_H
#define WC3_APP_BOOT_H

#include <stdint.h>

struct ANativeWindow;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runs on a dedicated thread.  data_dir must contain war3.exe, Storm.dll,
 * game.dll, ijl15.dll, Mss32.dll and the four MPQs; it becomes the guest's
 * working directory.  state_dir is the app-private directory for run.txt,
 * audio dumps and other diagnostic state.  game_mode is "roc" for Reign of
 * Chaos and "tft" (or NULL) for The Frozen Throne.  Returns a negative errno
 * before guest start; after the guest starts the function does not return
 * while the game is alive.
 */
int wc3_app_boot_start(const char *data_dir, const char *state_dir,
                       struct ANativeWindow *window, uint32_t width,
                       uint32_t height, const char *game_mode);

#ifdef __cplusplus
}
#endif

#endif /* WC3_APP_BOOT_H */
