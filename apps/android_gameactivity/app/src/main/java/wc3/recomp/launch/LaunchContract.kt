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

    /* The product default is DXVK 1.10.3 ("1").  An empty version is the
     * explicit "Auto" choice and keeps the driver-based compatibility gate. */
    const val DEFAULT_DXVK_VERSION = "1"

    /* DXVK defaults to an uncapped D3D9 swapchain.  The product starts with a
     * 60 FPS cap so the first launch is not a battery/thermal stress test; the
     * settings table can remove or override this row. */
    const val DEFAULT_DXVK_CONFIG = "d3d9.maxFrameRate = 60"
}
