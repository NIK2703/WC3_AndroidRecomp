package wc3.recomp

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import wc3.recomp.launch.GameMode
import wc3.recomp.launch.LaunchContract
import wc3.recomp.launch.LaunchCoordinator
import wc3.recomp.launch.LaunchSettings
import wc3.recomp.launch.LaunchSettingsStore
import wc3.recomp.launch.LaunchValidator

/**
 * The only visible launch-settings screen.  It is not a launcher entry: both
 * game icons start their host immediately and expose this screen through the
 * static long-press shortcut.
 */
class LaunchSettingsActivity : AppCompatActivity() {
    private lateinit var mode: GameMode
    private lateinit var store: LaunchSettingsStore

    private lateinit var modeIcon: ImageView
    private lateinit var modeTitle: TextView
    private lateinit var modeState: TextView

    private lateinit var folderInput: EditText
    private lateinit var folderStatus: TextView
    private lateinit var widthInput: EditText
    private lateinit var heightInput: EditText
    private lateinit var nativeChip: TextView
    private lateinit var chip720: TextView
    private lateinit var chip900: TextView
    private lateinit var chip1080: TextView
    private lateinit var soundSwitch: SwitchCompat
    private lateinit var argsInput: EditText
    private lateinit var latencyInput: EditText

    private lateinit var errorBanner: LinearLayout
    private lateinit var errorText: TextView
    private lateinit var grantButton: Button
    private lateinit var resetButton: Button
    private lateinit var doneButton: Button
    private lateinit var playButton: Button

    private var binding = false

    override fun onCreate(savedInstanceState: Bundle?) {
        mode = GameMode.fromId(intent?.getStringExtra(LaunchContract.EXTRA_MODE))
            ?: GameMode.TFT
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launch_settings)
        applyDisplayCutoutMode()

        store = LaunchSettingsStore(this)
        bindViews()
        bindMode()
        bindSettings(store.load(mode))
        wireListeners()
        refreshValidation()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val nextMode = GameMode.fromId(intent.getStringExtra(LaunchContract.EXTRA_MODE))
            ?: return
        if (nextMode != mode) {
            mode = nextMode
            bindMode()
            refreshValidation()
        }
    }

    override fun onResume() {
        super.onResume()
        if (::store.isInitialized) refreshValidation()
    }

    override fun onPause() {
        if (::store.isInitialized) saveIfValid()
        super.onPause()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_STORAGE) refreshValidation()
    }

    private fun bindViews() {
        modeIcon = findViewById(R.id.mode_icon)
        modeTitle = findViewById(R.id.mode_title)
        modeState = findViewById(R.id.mode_state)

        folderInput = findViewById(R.id.folder_input)
        folderStatus = findViewById(R.id.folder_status)
        widthInput = findViewById(R.id.width_input)
        heightInput = findViewById(R.id.height_input)
        nativeChip = findViewById(R.id.preset_native)
        chip720 = findViewById(R.id.preset_720)
        chip900 = findViewById(R.id.preset_900)
        chip1080 = findViewById(R.id.preset_1080)
        soundSwitch = findViewById(R.id.sound_switch)
        argsInput = findViewById(R.id.args_input)
        latencyInput = findViewById(R.id.latency_input)

        errorBanner = findViewById(R.id.error_banner)
        errorText = findViewById(R.id.error_text)
        grantButton = findViewById(R.id.grant_button)
        resetButton = findViewById(R.id.reset_button)
        doneButton = findViewById(R.id.done_button)
        playButton = findViewById(R.id.play_button)
    }

    private fun bindMode() {
        modeIcon.setImageResource(mode.iconRes)
        modeIcon.contentDescription = getString(
            R.string.cd_mode_icon,
            getString(mode.titleRes)
        )
        modeTitle.setText(mode.titleRes)
    }

    private fun bindSettings(settings: LaunchSettings) {
        binding = true
        folderInput.setText(settings.gameDir)
        widthInput.setText(if (settings.width > 0) settings.width.toString() else "")
        heightInput.setText(if (settings.height > 0) settings.height.toString() else "")
        soundSwitch.isChecked = settings.sound
        argsInput.setText(settings.extraArgs)
        latencyInput.setText(
            if (settings.audioLatencyMs > 0) settings.audioLatencyMs.toString() else ""
        )
        binding = false
    }

    private fun wireListeners() {
        folderInput.afterTextChanged { if (!binding) refreshValidation() }
        widthInput.afterTextChanged { if (!binding) refreshValidation() }
        heightInput.afterTextChanged { if (!binding) refreshValidation() }
        argsInput.afterTextChanged { if (!binding) refreshValidation() }
        latencyInput.afterTextChanged { if (!binding) refreshValidation() }

        folderInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        widthInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        heightInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        argsInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        latencyInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }

        soundSwitch.setOnCheckedChangeListener { _, _ ->
            if (!binding) {
                refreshValidation()
                saveIfValid()
            }
        }

        nativeChip.setOnClickListener { setResolution(0, 0) }
        chip720.setOnClickListener { setResolution(1280, 720) }
        chip900.setOnClickListener { setResolution(1600, 900) }
        chip1080.setOnClickListener { setResolution(1920, 1080) }

        findViewById<Button>(R.id.find_folder_button).setOnClickListener { findFolder() }
        grantButton.setOnClickListener { requestStoragePermission() }
        resetButton.setOnClickListener { confirmReset() }
        doneButton.setOnClickListener { finishWithSave(false) }
        playButton.setOnClickListener { finishWithSave(true) }
    }

    private fun setResolution(width: Int, height: Int) {
        widthInput.setText(if (width > 0) width.toString() else "")
        heightInput.setText(if (height > 0) height.toString() else "")
        refreshValidation()
        saveIfValid()
    }

    private fun findFolder() {
        val folder = LaunchValidator.findGameDir(this)
        if (folder != null) {
            folderInput.setText(folder.absolutePath)
            Toast.makeText(
                this,
                getString(R.string.launch_folder_found, folder.absolutePath),
                Toast.LENGTH_SHORT
            ).show()
            refreshValidation()
            saveIfValid()
            return
        }

        val probe = LaunchValidator.resolveGameDir(this, readForm())
        if (!LaunchValidator.hasStoragePermission(this, probe)) {
            requestStoragePermission()
        } else {
            Toast.makeText(this, R.string.launch_folder_not_found, Toast.LENGTH_LONG).show()
        }
    }

    private fun confirmReset() {
        AlertDialog.Builder(this)
            .setTitle(R.string.launch_reset_title)
            .setMessage(R.string.launch_reset_message)
            .setPositiveButton(R.string.launch_reset_confirm) { _, _ -> resetForm() }
            .setNegativeButton(R.string.launch_cancel, null)
            .show()
    }

    private fun resetForm() {
        val defaults = LaunchSettings(mode)
        bindSettings(defaults)
        store.save(defaults)
        refreshValidation()
    }

    private fun finishWithSave(launch: Boolean) {
        val settings = readForm()

        // Done persists even an invalid draft; the gate re-validates before it
        // starts the game.  Play is the only path that must be valid now.
        if (launch) {
            val result = LaunchValidator.validate(this, settings)
            if (result !is LaunchValidator.Result.Ok) {
                refreshValidation()
                return
            }
        }

        if (!store.save(settings)) {
            Toast.makeText(this, R.string.launch_error_save, Toast.LENGTH_LONG).show()
            return
        }

        if (launch) {
            LaunchCoordinator.startGame(this, settings)
            finishAndRemoveTask()
        } else {
            finish()
        }
    }

    private fun saveIfValid() {
        val settings = readForm()
        if (LaunchValidator.validate(this, settings) is LaunchValidator.Result.Ok) {
            store.save(settings)
        }
    }

    private fun refreshValidation() {
        val settings = readForm()
        updateChipSelection(settings.width, settings.height)
        updateFolderStatus(settings)

        when (val result = LaunchValidator.validate(this, settings)) {
            is LaunchValidator.Result.Ok -> {
                errorBanner.visibility = View.GONE
                grantButton.visibility = View.GONE
                playButton.isEnabled = true
            }
            is LaunchValidator.Result.Error -> {
                errorBanner.visibility = View.VISIBLE
                errorText.text = result.message
                grantButton.visibility =
                    if (result.kind == LaunchValidator.Kind.PERMISSION) View.VISIBLE
                    else View.GONE
                playButton.isEnabled = false
            }
        }
    }

    private fun updateChipSelection(width: Int, height: Int) {
        nativeChip.isSelected = width == 0 && height == 0
        chip720.isSelected = width == 1280 && height == 720
        chip900.isSelected = width == 1600 && height == 900
        chip1080.isSelected = width == 1920 && height == 1080
    }

    private fun updateFolderStatus(settings: LaunchSettings) {
        val folderSettings = settings.copy(
            width = 0,
            height = 0,
            extraArgs = "",
            audioLatencyMs = 0
        )
        when (val folderResult = LaunchValidator.validate(this, folderSettings)) {
            is LaunchValidator.Result.Ok -> {
                val folder = LaunchValidator.resolveGameDir(this, settings)
                setStatus(
                    getString(R.string.launch_status_ok, folder.absolutePath),
                    getString(R.string.launch_status_ready),
                    R.color.launch_ok
                )
            }
            is LaunchValidator.Result.Error -> {
                val statusRes = if (folderResult.kind == LaunchValidator.Kind.PERMISSION) {
                    R.string.launch_status_permission
                } else {
                    R.string.launch_status_missing
                }
                val text = getString(statusRes)
                val color = if (folderResult.kind == LaunchValidator.Kind.PERMISSION) {
                    R.color.launch_warning
                } else {
                    R.color.launch_error
                }
                setStatus(text, text, color)
            }
        }
    }

    private fun setStatus(folderText: String, modeText: String, colorRes: Int) {
        val color = ContextCompat.getColor(this, colorRes)
        folderStatus.text = folderText
        folderStatus.setTextColor(color)
        modeState.text = modeText
        modeState.setTextColor(color)
    }

    private fun readForm(): LaunchSettings = LaunchSettings(
        mode = mode,
        gameDir = folderInput.text.toString().trim(),
        width = readNumber(widthInput),
        height = readNumber(heightInput),
        sound = soundSwitch.isChecked,
        extraArgs = argsInput.text.toString().trim(),
        audioLatencyMs = readNumber(latencyInput)
    )

    private fun readNumber(input: EditText): Int {
        val text = input.text.toString().trim()
        if (text.isEmpty()) return 0
        return text.toIntOrNull() ?: -1
    }

    private fun requestStoragePermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val appIntent = Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:$packageName")
            )
            try {
                startActivity(appIntent)
            } catch (ignored: Exception) {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE
                ),
                REQUEST_STORAGE
            )
        }
    }

    private fun applyDisplayCutoutMode() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return
        val attributes = window.attributes
        attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        window.attributes = attributes
    }

    private fun EditText.afterTextChanged(block: () -> Unit) {
        addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(
                text: CharSequence?,
                start: Int,
                count: Int,
                after: Int
            ) = Unit

            override fun onTextChanged(
                text: CharSequence?,
                start: Int,
                before: Int,
                count: Int
            ) = block()

            override fun afterTextChanged(text: Editable?) = Unit
        })
    }

    private companion object {
        const val REQUEST_STORAGE = 4101
    }
}
