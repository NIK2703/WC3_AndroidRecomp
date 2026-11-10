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
        extraArgs = prefs.getString(KEY_EXTRA_ARGS, "").orEmpty().trim()
    )

    fun save(settings: LaunchSettings): Boolean =
        prefs.edit()
            .putString(KEY_GAME_DIR, settings.gameDir)
            .putInt(KEY_WIDTH, settings.width)
            .putInt(KEY_HEIGHT, settings.height)
            .putBoolean(KEY_SOUND, settings.sound)
            .putString(KEY_EXTRA_ARGS, settings.extraArgs)
            .commit()

    private companion object {
        const val PREFS_NAME = "wc3_launch"
        const val KEY_GAME_DIR = "game_dir"
        const val KEY_WIDTH = "width"
        const val KEY_HEIGHT = "height"
        const val KEY_SOUND = "sound"
        const val KEY_EXTRA_ARGS = "args"
    }
}
