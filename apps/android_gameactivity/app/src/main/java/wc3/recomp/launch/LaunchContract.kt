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
}
