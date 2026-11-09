package wc3.recomp.launch

import android.app.Activity
import wc3.recomp.Wc3GameActivity
import wc3.recomp.Wc3RocActivity

/**
 * The two product launch modes.  The host class stays here so the gates, the
 * settings screen and the coordinator cannot drift apart.
 */
enum class GameMode(
    val id: String,
    val hostClass: Class<out Activity>
) {
    TFT(id = "tft", hostClass = Wc3GameActivity::class.java),
    ROC(id = "roc", hostClass = Wc3RocActivity::class.java);

    companion object {
        fun fromId(id: String?): GameMode? = entries.firstOrNull { it.id == id }
    }
}
