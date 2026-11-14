package wc3.recomp.launch

/**
 * Intent protocol shared by the launcher/settings layer and the Java
 * GameActivity host.  These are protocol constants, not user-facing strings.
 */
object LaunchContract {
    const val ACTION_OPEN_SETTINGS = "wc3.recomp.action.OPEN_SETTINGS"

    const val EXTRA_MODE = "wc3.recomp.extra.MODE"
    const val EXTRA_LAUNCH = "wc3.recomp.extra.LAUNCH"

    const val KEY_GAME_DIR = "game_dir"
    const val KEY_WIDTH = "width"
    const val KEY_HEIGHT = "height"
    const val KEY_SOUND = "sound"
    const val KEY_EXTRA_ARGS = "args"
    const val KEY_DRIVER_ID = "vulkan_driver_id"
    const val KEY_DXVK_VERSION = "dxvk_version"
    const val KEY_DXVK_CONFIG = "dxvk_config"
    const val KEY_DOUBLE_TAP_GESTURES = "double_tap_gestures"
    const val KEY_DOUBLE_TAP_TIMEOUT_MS = "double_tap_timeout_ms"
    const val KEY_LONG_PRESS_TIMEOUT_MS = "long_press_timeout_ms"
    const val KEY_TAP_CLICK_DELAY_MS = "tap_click_delay_ms"

    /* The authored .icp profile uses 120 ms for both gesture timers.  The
     * settings screen exposes only the product range 100..200 ms, step 10. */
    const val DEFAULT_DOUBLE_TAP_TIMEOUT_MS = 120
    const val DEFAULT_LONG_PRESS_TIMEOUT_MS = 120

    /* Wall-clock delay between a pointer move and the LMB press in the
     * simplified (non-gameplay) overlay.  The settings screen exposes the
     * native clamp range 8..40 ms in 1 ms steps. */
    const val DEFAULT_TAP_CLICK_DELAY_MS = 10
    const val MIN_TAP_CLICK_DELAY_MS = 8
    const val MAX_TAP_CLICK_DELAY_MS = 40

    /* The product default is DXVK 1.10.3 ("1").  An empty version is the
     * explicit "Auto" choice and keeps the driver-based compatibility gate. */
    const val DEFAULT_DXVK_VERSION = "1"

    /* DXVK defaults to an uncapped D3D9 swapchain.  The product starts with a
     * 60 FPS cap so the first launch is not a battery/thermal stress test; the
     * settings table can remove or override this row. */
    const val DEFAULT_DXVK_CONFIG = "d3d9.maxFrameRate = 60"
}
