package wc3.recomp.launch

import android.os.Bundle

/**
 * The launch overrides owned by the product settings screen.  An empty game
 * directory means "use the native auto-detection"; zero width/height means
 * "keep the physical surface size".
 */
data class LaunchSettings(
    val mode: GameMode,
    val gameDir: String = "",
    val width: Int = 0,
    val height: Int = 0,
    val sound: Boolean = true,
    /* Empty means the system Vulkan loader.  A non-empty value is a 16-hex
     * ADPkg id under filesDir/drivers (ADR-036). */
    val driverId: String = "",
    /* "1" forces DXVK 1.10.3 and is the product default; "3" forces DXVK
     * 3.1.1.  Empty is the explicit "Auto" choice and keeps the driver-based
     * compatibility gate.  A forced backend bypasses the gate and is not
     * replaced by the other version on failure. */
    val dxvkVersion: String = LaunchContract.DEFAULT_DXVK_VERSION,
    /* Key-value DXVK options in dxvk.conf syntax.  The host writes this text
     * to an app-private file and points DXVK_CONFIG_FILE at it before
     * android_main; both 1.10.3 and 3.1.1 read that contract.  The default is
     * the 60 FPS cap; an explicitly empty string means "no DXVK overrides". */
    val dxvkConfig: String = LaunchContract.DEFAULT_DXVK_CONFIG,
    /* One of the three product schemes: authored double tap, single tap with
     * no double-tap wait, or semantic direct touch.  The enum is the single
     * discriminator; the bundle publishes the legacy overlay boolean and the
     * direct-touch flag from it. */
    val gestureMode: GestureMode = GestureMode.DOUBLE_TAP,
    /* Gesture timers in milliseconds.  The settings UI clamps them to
     * 50..200 in 10 ms steps; the native product helper repeats the clamp. */
    val doubleTapTimeoutMs: Int = LaunchContract.DEFAULT_DOUBLE_TAP_TIMEOUT_MS,
    val longPressTimeoutMs: Int = LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS,
    /* Unit-selection runtime cap: 12 (the original product default), 24 or
     * 40.  The 6x2 panel is used up to 12 units, 8x3 for 13..24 and 10x4 for
     * 25..40.  The corpus always carries the 40-unit structure; the cap is a
     * launch-time choice.  Applied on the next game launch, not to an already
     * running host. */
    val selectionCap: Int = LaunchContract.DEFAULT_SELECTION_CAP,
    /* True suppresses CCursorFrame::Render in the guest.  Touch input still
     * works; only the game-drawn cursor sprite is skipped.  Applied on the
     * next game launch. */
    val hideCursor: Boolean = LaunchContract.DEFAULT_HIDE_CURSOR,
    /* Opacity of the on-screen control overlay in percent.  The settings UI
     * exposes 10..50; the native controls layer clamps the same range and
     * replaces the authored per-element opacity. */
    val overlayOpacityPercent: Int = LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT,
    /* The single core selected in the product settings table.  The game's
     * main thread is pinned to it; no worker pool is published.  Empty means
     * no allowed core was detected and the host leaves affinity untouched.
     * Applied on the next game launch. */
    val workerCpus: List<Int> = emptyList(),
    /* Half-resolution grid presentation.  Off creates no extra SurfaceView
     * and no native renderer.  The darken percent follows the source slider
     * (0..100); the cycle moves the transparent pixel of every 2x2 cell and
     * is resolved from -1 "auto" to a concrete interval before launch. */
    val gridEnabled: Boolean = LaunchContract.DEFAULT_GRID_ENABLED,
    val gridDarkenPercent: Int = LaunchContract.DEFAULT_GRID_DARKEN_PERCENT,
    val gridCycleSeconds: Int = LaunchContract.DEFAULT_GRID_CYCLE_SECONDS,
    /* The final screen swap chain.  FIFO is the product default; the swap-chain
     * wrapper applies these values to the final real-window swap chain,
     * including the native direct path, and falls back to FIFO when a mode is
     * unsupported.  Buffering is the number of extra images above the surface
     * minimum (0..2). */
    val presentMode: String = LaunchContract.DEFAULT_PRESENT_MODE,
    val presentBuffers: Int = LaunchContract.DEFAULT_PRESENT_BUFFERS,
    /* Presentation scaling is not a setting.  The grid needs the exact-2x
     * half-surface path; every other case stays on the compositor. */
) {
    fun toBundle(): Bundle = Bundle().apply {
        /* The core table selects one CPU and never enables the worker pool.
         * Normalise here as well as in the settings screen so every launch
         * path, including a caller that constructs LaunchSettings directly,
         * publishes the same sequential bundle. */
        val requestedCpu = workerCpus
            .filter { it in 0..CpuTopology.MAX_CPU_ID }
            .firstOrNull()
        /* Repeat the cpuset filter here because a caller can construct
         * LaunchSettings directly or the cpuset can change between the
         * settings screen and the launch. */
        val availableCores = if (requestedCpu == null) {
            emptyList()
        } else {
            CpuTopology.availableCores()
        }
        val selectedCore = availableCores.firstOrNull { it.id == requestedCpu }
        /* Pin the game's main thread to the selected core.  A stale selected
         * id that the game process can no longer use falls back to the best
         * currently available core instead of leaving it unpinned. */
        val mainCpu = when {
            requestedCpu == null -> -1
            selectedCore != null -> selectedCore.id
            else -> CpuTopology.pickBest(availableCores)?.id ?: requestedCpu
        }
        putString(LaunchContract.KEY_GAME_DIR, gameDir)
        putInt(LaunchContract.KEY_WIDTH, width)
        putInt(LaunchContract.KEY_HEIGHT, height)
        putBoolean(LaunchContract.KEY_SOUND, sound)
        putString(LaunchContract.KEY_EXTRA_ARGS, "")
        putString(LaunchContract.KEY_DRIVER_ID, driverId)
        putString(LaunchContract.KEY_DXVK_VERSION, dxvkVersion)
        putString(LaunchContract.KEY_DXVK_CONFIG, dxvkConfig)
        putBoolean(
            LaunchContract.KEY_DOUBLE_TAP_GESTURES,
            gestureMode.doubleTapGestures
        )
        putBoolean(
            LaunchContract.KEY_DIRECT_TOUCH_ENABLED,
            gestureMode.directTouch
        )
        putInt(LaunchContract.KEY_DOUBLE_TAP_TIMEOUT_MS, doubleTapTimeoutMs)
        putInt(LaunchContract.KEY_LONG_PRESS_TIMEOUT_MS, longPressTimeoutMs)
        putInt(LaunchContract.KEY_TAP_CLICK_DELAY_MS, LaunchContract.TAP_CLICK_DELAY_MS)
        putInt(
            LaunchContract.KEY_SELECTION_CAP,
            LaunchContract.normalizeSelectionCap(selectionCap)
        )
        putBoolean(LaunchContract.KEY_HIDE_CURSOR, hideCursor)
        putInt(LaunchContract.KEY_OVERLAY_OPACITY_PERCENT, overlayOpacityPercent)
        putBoolean(LaunchContract.KEY_PARALLEL_EXECUTION, false)
        /* A single selected core is the sequential product path: publish no
         * worker list, so a stale pool from an earlier launch cannot survive. */
        putString(LaunchContract.KEY_WORKER_CPUS, "")
        putInt(LaunchContract.KEY_MAIN_CPU, mainCpu)
        putBoolean(LaunchContract.KEY_GRID_ENABLED, gridEnabled)
        putInt(LaunchContract.KEY_GRID_DARKEN_PERCENT, gridDarkenPercent)
        putInt(LaunchContract.KEY_GRID_CYCLE_SECONDS, gridCycleSeconds)
        putString(LaunchContract.KEY_PRESENT_MODE, presentMode)
        putInt(LaunchContract.KEY_PRESENT_BUFFERS, presentBuffers)
        putString(
            LaunchContract.KEY_PRESENT_SCALE,
            if (gridEnabled) {
                LaunchContract.PRESENT_SCALE_HALF
            } else {
                LaunchContract.PRESENT_SCALE_SYSTEM
            }
        )
    }
}
