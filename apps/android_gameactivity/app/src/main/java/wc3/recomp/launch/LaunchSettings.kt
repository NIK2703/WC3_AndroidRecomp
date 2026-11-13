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
    val longPressTimeoutMs: Int = LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS
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
    }
}
