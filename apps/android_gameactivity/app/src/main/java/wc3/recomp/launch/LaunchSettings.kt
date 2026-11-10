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
    val extraArgs: String = ""
) {
    fun toBundle(): Bundle = Bundle().apply {
        putString(LaunchContract.KEY_GAME_DIR, gameDir)
        putInt(LaunchContract.KEY_WIDTH, width)
        putInt(LaunchContract.KEY_HEIGHT, height)
        putBoolean(LaunchContract.KEY_SOUND, sound)
        putString(LaunchContract.KEY_EXTRA_ARGS, extraArgs)
    }
}
