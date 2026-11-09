package wc3.recomp

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import wc3.recomp.launch.GameMode
import wc3.recomp.launch.LaunchContract
import wc3.recomp.launch.LaunchCoordinator
import wc3.recomp.launch.LaunchSettingsStore
import wc3.recomp.launch.LaunchValidator

/**
 * Invisible launcher gate.  A normal tap starts the native host immediately;
 * the settings screen is reachable only through the long-press shortcut, which
 * arrives here as ACTION_OPEN_SETTINGS.
 */
abstract class Wc3LauncherActivity : AppCompatActivity() {
    protected abstract val mode: GameMode

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        handleIntent()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent()
    }

    private fun handleIntent() {
        // A singleTop gate can receive a second intent while it is finishing;
        // do not start a second host from a stale activity.
        if (isFinishing) return

        if (intent?.action == LaunchContract.ACTION_OPEN_SETTINGS) {
            startActivity(LaunchCoordinator.settingsIntent(this, mode))
            finish()
            return
        }

        val settings = LaunchSettingsStore(this).load(mode)
        when (val result = LaunchValidator.validate(this, settings)) {
            is LaunchValidator.Result.Ok -> LaunchCoordinator.startGame(this, settings)
            is LaunchValidator.Result.Error -> {
                val messageRes = when (result.kind) {
                    LaunchValidator.Kind.PERMISSION -> R.string.launch_toast_permission
                    LaunchValidator.Kind.FOLDER,
                    LaunchValidator.Kind.FILES -> R.string.launch_toast_missing
                    else -> R.string.launch_toast_invalid
                }
                Toast.makeText(this, messageRes, Toast.LENGTH_LONG).show()
            }
        }
        finish()
    }
}
