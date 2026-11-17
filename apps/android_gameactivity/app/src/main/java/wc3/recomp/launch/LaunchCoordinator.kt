package wc3.recomp.launch

import android.content.Context
import android.content.Intent
import android.os.Bundle
import wc3.recomp.LaunchSettingsActivity
import wc3.recomp.R

object LaunchCoordinator {
    /**
     * Starts the native host in its own task.  The host activity is
     * singleTask, so an already-running game is brought to the front instead
     * of being recreated with the new settings.
     */
    fun startGame(context: Context, settings: LaunchSettings) {
        // Resolve an empty "auto" folder here so the host gets the exact
        // directory that was validated.  This also lets the host recognize an
        // app-private folder and skip the All files access prompt.
        val launchSettings = resolve(context, settings)
        val intent = Intent(context, settings.mode.hostClass).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            putExtra(LaunchContract.EXTRA_LAUNCH, launchSettings.toBundle())
        }
        context.startActivity(intent)
    }

    @JvmStatic
    fun resolveForHost(context: Context, mode: GameMode): Bundle =
        resolve(context, LaunchSettingsStore(context).load(mode)).toBundle()

    private fun resolve(context: Context, settings: LaunchSettings): LaunchSettings {
        val resolved = settings.copy(
            gameDir = LaunchValidator.resolveGameDir(context, settings).absolutePath
        )
        val experimentalGameplay = context.applicationContext.resources
            .getBoolean(R.bool.experimental_gameplay_options_supported)
        if (experimentalGameplay) return resolved
        return resolved.copy(selection24 = false, parallelExecution = false)
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
