package wc3.recomp.launch

import android.app.Activity
import wc3.recomp.R
import wc3.recomp.Wc3GameActivity
import wc3.recomp.Wc3RocActivity

/**
 * The two product launch modes.  The enum owns the host class and the
 * mode-specific labels/icons so the gates and the settings screen do not
 * duplicate the mapping.
 */
enum class GameMode(
    val id: String,
    val hostClass: Class<out Activity>,
    val titleRes: Int,
    val iconRes: Int
) {
    TFT(
        id = "tft",
        hostClass = Wc3GameActivity::class.java,
        titleRes = R.string.launch_mode_tft,
        iconRes = R.mipmap.ic_launcher_tft
    ),
    ROC(
        id = "roc",
        hostClass = Wc3RocActivity::class.java,
        titleRes = R.string.launch_mode_roc,
        iconRes = R.mipmap.ic_launcher_roc
    );

    companion object {
        fun fromId(id: String?): GameMode? = entries.firstOrNull { it.id == id }
    }
}
