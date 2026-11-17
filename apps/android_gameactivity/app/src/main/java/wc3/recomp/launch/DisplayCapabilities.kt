package wc3.recomp.launch

import android.content.Context
import android.view.WindowManager

/**
 * Display facts the launch settings need before the native host starts.
 *
 * The grid cycle defaults to 60 s on OLED panels so the bright pixel of every
 * 2x2 cell keeps moving and does not leave a fixed pattern.  This mirrors the
 * source feature; a configured non-negative value always wins.  Only the
 * settings screen and the one-shot launch path call this, never a render loop.
 */
object DisplayCapabilities {
    const val OLED_GRID_CYCLE_SECONDS = 60

    @JvmStatic
    fun gridCycleSeconds(context: Context, configuredSeconds: Int): Int {
        if (configuredSeconds >= 0) {
            return configuredSeconds.coerceIn(
                LaunchContract.MIN_GRID_CYCLE_SECONDS,
                LaunchContract.MAX_GRID_CYCLE_SECONDS
            )
        }
        return if (isOled(context)) OLED_GRID_CYCLE_SECONDS else 0
    }

    @JvmStatic
    fun isOled(context: Context): Boolean {
        val display = try {
            (context.getSystemService(Context.WINDOW_SERVICE) as? WindowManager)
                ?.defaultDisplay
        } catch (ignored: Exception) {
            null
        } ?: return false

        try {
            val caps = display.hdrCapabilities
            if (caps != null && caps.supportedHdrTypes.isNotEmpty()) return true
        } catch (ignored: Exception) {
            // Some vendor displays refuse the HDR query; fall back to the name.
        }

        val name = try {
            display.name
        } catch (ignored: Exception) {
            null
        } ?: return false
        return name.contains("OLED") || name.contains("AMOLED") ||
            name.contains("POLED")
    }
}
