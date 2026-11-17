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
    val extraArgs: String = "",
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
    /* True keeps the authored .icp double-tap actions.  False selects the
     * direct RTS scheme: long press is the right click and a single tap fires
     * on finger-up without waiting for a second tap. */
    val doubleTapGestures: Boolean = true,
    /* Gesture timers in milliseconds.  The settings UI clamps them to
     * 100..200 in 10 ms steps; the native product helper repeats the clamp. */
    val doubleTapTimeoutMs: Int = LaunchContract.DEFAULT_DOUBLE_TAP_TIMEOUT_MS,
    val longPressTimeoutMs: Int = LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS,
    /* Wall-clock delay before the LMB press in the SIMPLE overlay.  The
     * native helper clamps it to 8..40 ms; 10 ms is the product default. */
    val tapClickDelayMs: Int = LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS,
    /* True enables the 24-unit selection profile and the 8x3 unit panel;
     * false (the product default) keeps the original 12-unit cap and the
     * 6x2 panel.  Applied on the next game launch, not to an already running
     * host. */
    val selection24: Boolean = LaunchContract.DEFAULT_SELECTION_24,
    /* True suppresses CCursorFrame::Render in the guest.  Touch input still
     * works; only the game-drawn cursor sprite is skipped.  Applied on the
     * next game launch. */
    val hideCursor: Boolean = LaunchContract.DEFAULT_HIDE_CURSOR,
    /* Opacity of the on-screen control overlay in percent.  The settings UI
     * exposes 10..50; the native controls layer clamps the same range and
     * replaces the authored per-element opacity. */
    val overlayOpacityPercent: Int = LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT,
    /* True keeps the deterministic chunked raycast and vertex loops.  False
     * makes both wrappers call the original sequential guest functions;
     * applied on the next game launch. */
    val parallelExecution: Boolean = LaunchContract.DEFAULT_PARALLEL_EXECUTION,
    /* Half-resolution grid presentation.  Off creates no extra SurfaceView
     * and no native renderer.  The darken percent follows the source slider
     * (0..100); the cycle moves the transparent pixel of every 2x2 cell and
     * is resolved from -1 "auto" to a concrete interval before launch. */
    val gridEnabled: Boolean = LaunchContract.DEFAULT_GRID_ENABLED,
    val gridDarkenPercent: Int = LaunchContract.DEFAULT_GRID_DARKEN_PERCENT,
    val gridCycleSeconds: Int = LaunchContract.DEFAULT_GRID_CYCLE_SECONDS,
    /* Presentation scaling is not a setting.  The grid needs the exact-2x
     * half-surface path; every other case stays on the compositor. */
) {
    fun toBundle(): Bundle = Bundle().apply {
        putString(LaunchContract.KEY_GAME_DIR, gameDir)
        putInt(LaunchContract.KEY_WIDTH, width)
        putInt(LaunchContract.KEY_HEIGHT, height)
        putBoolean(LaunchContract.KEY_SOUND, sound)
        putString(LaunchContract.KEY_EXTRA_ARGS, extraArgs)
        putString(LaunchContract.KEY_DRIVER_ID, driverId)
        putString(LaunchContract.KEY_DXVK_VERSION, dxvkVersion)
        putString(LaunchContract.KEY_DXVK_CONFIG, dxvkConfig)
        putBoolean(LaunchContract.KEY_DOUBLE_TAP_GESTURES, doubleTapGestures)
        putInt(LaunchContract.KEY_DOUBLE_TAP_TIMEOUT_MS, doubleTapTimeoutMs)
        putInt(LaunchContract.KEY_LONG_PRESS_TIMEOUT_MS, longPressTimeoutMs)
        putInt(LaunchContract.KEY_TAP_CLICK_DELAY_MS, tapClickDelayMs)
        putBoolean(LaunchContract.KEY_SELECTION_24, selection24)
        putBoolean(LaunchContract.KEY_HIDE_CURSOR, hideCursor)
        putInt(LaunchContract.KEY_OVERLAY_OPACITY_PERCENT, overlayOpacityPercent)
        putBoolean(LaunchContract.KEY_PARALLEL_EXECUTION, parallelExecution)
        putBoolean(LaunchContract.KEY_GRID_ENABLED, gridEnabled)
        putInt(LaunchContract.KEY_GRID_DARKEN_PERCENT, gridDarkenPercent)
        putInt(LaunchContract.KEY_GRID_CYCLE_SECONDS, gridCycleSeconds)
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
