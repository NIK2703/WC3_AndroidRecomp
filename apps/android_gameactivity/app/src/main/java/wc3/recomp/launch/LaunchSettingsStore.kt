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
            ?: LaunchContract.DEFAULT_DXVK_CONFIG,
        /* An absent preference is the existing double-tap scheme. */
        doubleTapGestures = prefs.getBoolean(KEY_DOUBLE_TAP_GESTURES, true),
        doubleTapTimeoutMs = prefs.getInt(
            KEY_DOUBLE_TAP_TIMEOUT_MS,
            LaunchContract.DEFAULT_DOUBLE_TAP_TIMEOUT_MS
        ),
        longPressTimeoutMs = prefs.getInt(
            KEY_LONG_PRESS_TIMEOUT_MS,
            LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS
        ),
        tapClickDelayMs = prefs.getInt(
            KEY_TAP_CLICK_DELAY_MS,
            LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS
        ).let {
            if (it >= LaunchContract.MIN_TAP_CLICK_DELAY_MS &&
                it <= LaunchContract.MAX_TAP_CLICK_DELAY_MS) {
                it
            } else {
                LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS
            }
        },
        /* An absent preference keeps the original 12-unit cap; the 24-unit
         * profile is opt-in. */
        selection24 = prefs.getBoolean(
            KEY_SELECTION_24, LaunchContract.DEFAULT_SELECTION_24
        ),
        hideCursor = prefs.getBoolean(
            KEY_HIDE_CURSOR, LaunchContract.DEFAULT_HIDE_CURSOR
        ),
        /* An absent or out-of-range preference falls back to the product
         * default; the slider range is 10..50 percent. */
        overlayOpacityPercent = prefs.getInt(
            KEY_OVERLAY_OPACITY_PERCENT,
            LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT
        ).let {
            if (it >= LaunchContract.MIN_OVERLAY_OPACITY_PERCENT &&
                it <= LaunchContract.MAX_OVERLAY_OPACITY_PERCENT
            ) {
                it
            } else {
                LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT
            }
        }
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
            .putBoolean(KEY_DOUBLE_TAP_GESTURES, settings.doubleTapGestures)
            .putInt(KEY_DOUBLE_TAP_TIMEOUT_MS, settings.doubleTapTimeoutMs)
            .putInt(KEY_LONG_PRESS_TIMEOUT_MS, settings.longPressTimeoutMs)
            .putInt(
                KEY_TAP_CLICK_DELAY_MS,
                settings.tapClickDelayMs.coerceIn(
                    LaunchContract.MIN_TAP_CLICK_DELAY_MS,
                    LaunchContract.MAX_TAP_CLICK_DELAY_MS
                )
            )
            .putBoolean(KEY_SELECTION_24, settings.selection24)
            .putBoolean(KEY_HIDE_CURSOR, settings.hideCursor)
            .putInt(
                KEY_OVERLAY_OPACITY_PERCENT,
                settings.overlayOpacityPercent.coerceIn(
                    LaunchContract.MIN_OVERLAY_OPACITY_PERCENT,
                    LaunchContract.MAX_OVERLAY_OPACITY_PERCENT
                )
            )
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
        const val KEY_DOUBLE_TAP_GESTURES = "double_tap_gestures"
        const val KEY_DOUBLE_TAP_TIMEOUT_MS = "double_tap_timeout_ms"
        const val KEY_LONG_PRESS_TIMEOUT_MS = "long_press_timeout_ms"
        const val KEY_TAP_CLICK_DELAY_MS = "tap_click_delay_ms"
        const val KEY_SELECTION_24 = "selection_24"
        const val KEY_HIDE_CURSOR = "hide_cursor"
        const val KEY_OVERLAY_OPACITY_PERCENT = "overlay_opacity_percent"
    }
}
