package wc3.recomp.launch

import android.content.Context

/**
 * SharedPreferences is owned by the default process.  The RoC host lives in
 * :roc and therefore never reads this file: the gate passes the same values to
 * both hosts through the launch intent instead.
 */
class LaunchSettingsStore(context: Context) {
    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun load(mode: GameMode): LaunchSettings = LaunchSettings(
        mode = mode,
        gameDir = prefs.getString(KEY_GAME_DIR, "").orEmpty().trim(),
        width = prefs.getInt(KEY_WIDTH, 0),
        height = prefs.getInt(KEY_HEIGHT, 0),
        sound = prefs.getBoolean(KEY_SOUND, true),
        extraArgs = prefs.getString(KEY_EXTRA_ARGS, "").orEmpty().trim(),
        driverId = prefs.getString(KEY_DRIVER_ID, "").orEmpty().trim(),
        /* An absent preference is the first launch and gets DXVK 1.10.3.  A
         * present empty string is the explicit "Auto" choice and stays auto. */
        dxvkVersion = prefs.getString(
            KEY_DXVK_VERSION, LaunchContract.DEFAULT_DXVK_VERSION
        ).orEmpty().trim(),
        /* An absent preference is the first launch and gets the 60 FPS
         * default.  A present empty string is an explicit "no overrides" and
         * must stay empty. */
        dxvkConfig = prefs.getString(KEY_DXVK_CONFIG, null)
            ?: LaunchContract.DEFAULT_DXVK_CONFIG
    )

    fun save(settings: LaunchSettings): Boolean =
        prefs.edit()
            .putString(KEY_GAME_DIR, settings.gameDir)
            .putInt(KEY_WIDTH, settings.width)
            .putInt(KEY_HEIGHT, settings.height)
            .putBoolean(KEY_SOUND, settings.sound)
            .putString(KEY_EXTRA_ARGS, settings.extraArgs)
            .putString(KEY_DRIVER_ID, settings.driverId)
            .putString(KEY_DXVK_VERSION, settings.dxvkVersion)
            .putString(KEY_DXVK_CONFIG, settings.dxvkConfig)
            .commit()

    private companion object {
        const val PREFS_NAME = "wc3_launch"
        const val KEY_GAME_DIR = "game_dir"
        const val KEY_WIDTH = "width"
        const val KEY_HEIGHT = "height"
        const val KEY_SOUND = "sound"
        const val KEY_EXTRA_ARGS = "args"
        const val KEY_DRIVER_ID = "vulkan_driver_id"
        const val KEY_DXVK_VERSION = "dxvk_version"
        const val KEY_DXVK_CONFIG = "dxvk_config"
    }
}
