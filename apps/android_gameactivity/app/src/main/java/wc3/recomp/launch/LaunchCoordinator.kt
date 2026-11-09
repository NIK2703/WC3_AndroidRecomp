package wc3.recomp.launch

import android.content.Context
import android.content.Intent
import wc3.recomp.LaunchSettingsActivity

object LaunchCoordinator {
    /**
     * Starts the native host in its own task.  The host activity is
     * singleTask, so an already-running game is brought to the front instead
     * of being recreated with the new settings.
     */
    fun startGame(context: Context, settings: LaunchSettings) {
        val intent = Intent(context, settings.mode.hostClass).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            putExtra(LaunchContract.EXTRA_LAUNCH, settings.toBundle())
        }
        context.startActivity(intent)
    }

    /**
     * The settings screen has its own task affinity and singleTask launch
     * mode, so re-opening it from either shortcut reuses the same task.
     */
    fun settingsIntent(context: Context, mode: GameMode): Intent =
        Intent(context, LaunchSettingsActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            putExtra(LaunchContract.EXTRA_MODE, mode.id)
        }
}
