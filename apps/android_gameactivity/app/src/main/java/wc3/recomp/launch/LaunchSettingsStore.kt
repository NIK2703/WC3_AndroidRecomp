package wc3.recomp.launch

import android.content.Context
import wc3.recomp.R

/**
 * SharedPreferences is owned by the default process.  The RoC host lives in
 * :roc and therefore never reads this file: the gate passes the same values to
 * both hosts through the launch intent instead.
 */
class LaunchSettingsStore(context: Context) {
    private val appContext = context.applicationContext
    private val prefs = appContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val userDebugBuild =
        appContext.resources.getBoolean(R.bool.userdebug_build)

    fun load(mode: GameMode): LaunchSettings {
        /* The grid masks the exact 2x source; a saved "system" value from an
         * older build must not leave the game at native while the mask is on. */
        val storedGrid = prefs.getBoolean(
            KEY_GRID_ENABLED, LaunchContract.DEFAULT_GRID_ENABLED
        )
        val gridEnabled = !userDebugBuild && storedGrid
        return LaunchSettings(
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
                    it <= LaunchContract.MAX_TAP_CLICK_DELAY_MS
                ) {
                    it
                } else {
                    LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS
                }
            },
            /* An absent preference keeps the original 12-unit cap; the 24-unit
             * profile is opt-in. */
            selection24 = !userDebugBuild && prefs.getBoolean(
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
            },
            /* An absent preference keeps the parallel guest paths disabled; the
             * switch is opt-in. */
            parallelExecution = prefs.getBoolean(
                KEY_PARALLEL_EXECUTION,
                LaunchContract.DEFAULT_PARALLEL_EXECUTION
            ),
            /* The grid layer is opt-in.  The darken percent is clamped to the
             * slider range; an absent or negative cycle is the OLED-aware auto
             * default, resolved once here so the intent bundle carries a concrete
             * interval. */
            gridEnabled = gridEnabled,
            gridDarkenPercent = prefs.getInt(
                KEY_GRID_DARKEN_PERCENT,
                LaunchContract.DEFAULT_GRID_DARKEN_PERCENT
            ).coerceIn(
                LaunchContract.MIN_GRID_DARKEN_PERCENT,
                LaunchContract.MAX_GRID_DARKEN_PERCENT
            ),
            gridCycleSeconds = DisplayCapabilities.gridCycleSeconds(
                appContext,
                prefs.getInt(
                    KEY_GRID_CYCLE_SECONDS,
                    LaunchContract.DEFAULT_GRID_CYCLE_SECONDS
                )
            ),
            /* An absent, unknown or stale mode falls back to FIFO.  The
             * native pass repeats the supported-mode check on the device. */
            presentMode = prefs.getString(
                KEY_PRESENT_MODE, LaunchContract.DEFAULT_PRESENT_MODE
            ).orEmpty().trim().let {
                if (it in LaunchContract.PRESENT_MODES) it
                else LaunchContract.DEFAULT_PRESENT_MODE
            },
            presentBuffers = prefs.getInt(
                KEY_PRESENT_BUFFERS,
                LaunchContract.DEFAULT_PRESENT_BUFFERS
            ).coerceIn(
                LaunchContract.MIN_PRESENT_BUFFERS,
                LaunchContract.MAX_PRESENT_BUFFERS
            )
        )
    }

    /**
     * Called when the guest's own Video options changed resolution while the
     * half-resolution grid was active.  The grid and the exact-2x scaling are
     * gone for this session; persist the grid off and remember the new mode so
     * the next launch does not reapply the old launch override.
     */
    fun disableHalfGrid(mode: GameMode, width: Int, height: Int) {
        val current = load(mode)
        save(
            current.copy(
                gridEnabled = false,
                width = if (width > 0) width else current.width,
                height = if (height > 0) height else current.height
            )
        )
    }

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
            .putBoolean(KEY_PARALLEL_EXECUTION, settings.parallelExecution)
            .putBoolean(KEY_GRID_ENABLED, settings.gridEnabled)
            .putInt(
                KEY_GRID_DARKEN_PERCENT,
                settings.gridDarkenPercent.coerceIn(
                    LaunchContract.MIN_GRID_DARKEN_PERCENT,
                    LaunchContract.MAX_GRID_DARKEN_PERCENT
                )
            )
            .putInt(
                KEY_GRID_CYCLE_SECONDS,
                settings.gridCycleSeconds.coerceIn(
                    LaunchContract.MIN_GRID_CYCLE_SECONDS,
                    LaunchContract.MAX_GRID_CYCLE_SECONDS
                )
            )
            .putString(
                KEY_PRESENT_MODE,
                if (settings.presentMode in LaunchContract.PRESENT_MODES)
                    settings.presentMode
                else
                    LaunchContract.DEFAULT_PRESENT_MODE
            )
            .putInt(
                KEY_PRESENT_BUFFERS,
                settings.presentBuffers.coerceIn(
                    LaunchContract.MIN_PRESENT_BUFFERS,
                    LaunchContract.MAX_PRESENT_BUFFERS
                )
            )
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
        const val KEY_PARALLEL_EXECUTION = "parallel_execution"
        const val KEY_GRID_ENABLED = "grid_enabled"
        const val KEY_GRID_DARKEN_PERCENT = "grid_darken_percent"
        const val KEY_GRID_CYCLE_SECONDS = "grid_cycle_seconds"
        const val KEY_PRESENT_MODE = "present_mode"
        const val KEY_PRESENT_BUFFERS = "present_buffers"
    }
}
