package wc3.recomp

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.Editable
import android.text.TextWatcher
import android.util.DisplayMetrics
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import wc3.recomp.launch.GameFolderPicker
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

    private lateinit var folderInput: EditText
    private lateinit var folderStatus: TextView
    private lateinit var resolutionSpinner: Spinner
    private lateinit var soundSwitch: SwitchCompat
    private lateinit var argsInput: EditText
    private lateinit var latencyInput: EditText

    private lateinit var errorBanner: LinearLayout
    private lateinit var errorText: TextView
    private lateinit var grantButton: Button
    private lateinit var playButton: Button

    private var resolutionOptions: List<ResolutionOption> = emptyList()
    private var binding = false
    private var storageRequested = false

    private val folderPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) usePickedFolder(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        mode = GameMode.fromId(intent?.getStringExtra(LaunchContract.EXTRA_MODE))
            ?: GameMode.TFT
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launch_settings)

        store = LaunchSettingsStore(this)
        val settings = store.load(mode)
        resolutionOptions = buildResolutionOptions(settings)
        bindViews()
        bindSettings(settings)
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
        if (requestCode == REQUEST_STORAGE) {
            storageRequested = true
            refreshValidation()
        }
    }

    private fun bindViews() {
        folderInput = findViewById(R.id.folder_input)
        folderStatus = findViewById(R.id.folder_status)
        resolutionSpinner = findViewById(R.id.resolution_spinner)
        soundSwitch = findViewById(R.id.sound_switch)
        argsInput = findViewById(R.id.args_input)
        latencyInput = findViewById(R.id.latency_input)

        errorBanner = findViewById(R.id.error_banner)
        errorText = findViewById(R.id.error_text)
        grantButton = findViewById(R.id.grant_button)
        playButton = findViewById(R.id.play_button)

        resolutionSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            resolutionOptions.map { it.label }
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }
    }

    private fun bindSettings(settings: LaunchSettings) {
        binding = true
        folderInput.setText(settings.gameDir)

        val resolutionIndex = resolutionOptions.indexOfFirst {
            it.width == settings.width && it.height == settings.height
        }
        resolutionSpinner.setSelection(
            if (resolutionIndex >= 0) resolutionIndex else resolutionOptions.lastIndex,
            false
        )

        soundSwitch.isChecked = settings.sound
        argsInput.setText(settings.extraArgs)
        latencyInput.setText(
            if (settings.audioLatencyMs > 0) settings.audioLatencyMs.toString() else ""
        )
        binding = false
    }

    private fun wireListeners() {
        folderInput.afterTextChanged { if (!binding) refreshValidation() }
        argsInput.afterTextChanged { if (!binding) refreshValidation() }
        latencyInput.afterTextChanged { if (!binding) refreshValidation() }

        folderInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        argsInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        latencyInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }

        soundSwitch.setOnCheckedChangeListener { _, _ ->
            if (!binding) {
                refreshValidation()
                saveIfValid()
            }
        }

        resolutionSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                position: Int,
                id: Long
            ) {
                if (!binding) {
                    refreshValidation()
                    saveIfValid()
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }

        findViewById<Button>(R.id.choose_folder_button).setOnClickListener { pickFolder() }
        findViewById<Button>(R.id.find_folder_button).setOnClickListener { findFolder() }
        findViewById<Button>(R.id.reset_button).setOnClickListener { confirmReset() }
        findViewById<Button>(R.id.done_button).setOnClickListener { finishWithSave(false) }
        grantButton.setOnClickListener { requestStoragePermission() }
        playButton.setOnClickListener { finishWithSave(true) }
    }

    private fun buildResolutionOptions(settings: LaunchSettings): List<ResolutionOption> {
        val (screenWidth, screenHeight) = nativeScreenSize()
        val options = mutableListOf<ResolutionOption>()

        var height = MIN_RESOLUTION_HEIGHT
        while (height < screenHeight) {
            val width = widthForHeight(height, screenWidth, screenHeight)
            if (width in LaunchValidator.MIN_WIDTH..LaunchValidator.MAX_WIDTH &&
                height in LaunchValidator.MIN_HEIGHT..LaunchValidator.MAX_HEIGHT
            ) {
                options += ResolutionOption(
                    width = width,
                    height = height,
                    label = getString(R.string.launch_resolution_option, width, height)
                )
            }
            height += RESOLUTION_HEIGHT_STEP
        }

        // Preserve a resolution saved by an older build that is not on the
        // current grid.  The native entry below already covers a saved value
        // that happens to equal the screen size.
        val savedExplicit = settings.width > 0 && settings.height > 0 &&
            (settings.width != screenWidth || settings.height != screenHeight)
        if (savedExplicit &&
            options.none { it.width == settings.width && it.height == settings.height }
        ) {
            options += ResolutionOption(
                width = settings.width,
                height = settings.height,
                label = getString(
                    R.string.launch_resolution_option,
                    settings.width,
                    settings.height
                )
            )
        }

        options += ResolutionOption(
            width = 0,
            height = 0,
            label = getString(
                R.string.launch_resolution_native,
                screenWidth,
                screenHeight
            )
        )
        return options
    }

    @Suppress("DEPRECATION")
    private fun nativeScreenSize(): Pair<Int, Int> {
        val metrics = DisplayMetrics()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.getRealMetrics(metrics)
        } else {
            windowManager.defaultDisplay.getRealMetrics(metrics)
        }

        var width = metrics.widthPixels
        var height = metrics.heightPixels
        if (width <= 0 || height <= 0) {
            val fallback = resources.displayMetrics
            width = fallback.widthPixels
            height = fallback.heightPixels
        }
        if (width <= 0 || height <= 0) return DEFAULT_SCREEN_WIDTH to DEFAULT_SCREEN_HEIGHT
        return maxOf(width, height) to minOf(width, height)
    }

    private fun widthForHeight(height: Int, screenWidth: Int, screenHeight: Int): Int {
        if (screenHeight <= 0) return height
        return ((height.toLong() * screenWidth + screenHeight / 2) / screenHeight).toInt()
    }

    private fun pickFolder() {
        try {
            folderPicker.launch(null)
        } catch (ignored: ActivityNotFoundException) {
            Toast.makeText(this, R.string.launch_folder_picker_missing, Toast.LENGTH_LONG)
                .show()
        }
    }

    private fun usePickedFolder(uri: Uri) {
        val folder = GameFolderPicker.resolveTree(this, uri)
        if (folder == null) {
            Toast.makeText(this, R.string.launch_folder_unsupported, Toast.LENGTH_LONG).show()
            return
        }
        folderInput.setText(folder.absolutePath)
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
        if (!store.save(defaults)) {
            Toast.makeText(this, R.string.launch_error_save, Toast.LENGTH_LONG).show()
        }
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
                folderStatus.text = getString(R.string.launch_status_ok, folder.absolutePath)
                folderStatus.setTextColor(ContextCompat.getColor(this, R.color.launch_ok))
            }
            is LaunchValidator.Result.Error -> {
                val statusRes = when (folderResult.kind) {
                    LaunchValidator.Kind.PERMISSION -> R.string.launch_status_permission
                    LaunchValidator.Kind.WRITABLE -> R.string.launch_status_unavailable
                    else -> R.string.launch_status_missing
                }
                val color = if (folderResult.kind == LaunchValidator.Kind.PERMISSION) {
                    R.color.launch_warning
                } else {
                    R.color.launch_error
                }
                folderStatus.setText(statusRes)
                folderStatus.setTextColor(ContextCompat.getColor(this, color))
            }
        }
    }

    private fun readForm(): LaunchSettings {
        val resolution = resolutionOptions.getOrNull(resolutionSpinner.selectedItemPosition)
            ?: resolutionOptions.lastOrNull()
            ?: ResolutionOption(0, 0, "")

        return LaunchSettings(
            mode = mode,
            gameDir = folderInput.text.toString().trim(),
            width = resolution.width,
            height = resolution.height,
            sound = soundSwitch.isChecked,
            extraArgs = argsInput.text.toString().trim(),
            audioLatencyMs = readNumber(latencyInput)
        )
    }

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
            return
        }

        if (storageRequested &&
            !ActivityCompat.shouldShowRequestPermissionRationale(
                this,
                Manifest.permission.READ_EXTERNAL_STORAGE
            )
        ) {
            // The second denial can be permanent; the system dialog will no
            // longer appear, so send the user to the app's permission page.
            try {
                startActivity(
                    Intent(
                        Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:$packageName")
                    )
                )
            } catch (ignored: Exception) {
                // No settings activity on this device; the status stays visible.
            }
            return
        }

        storageRequested = true
        ActivityCompat.requestPermissions(
            this,
            arrayOf(
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            ),
            REQUEST_STORAGE
        )
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

    private data class ResolutionOption(
        val width: Int,
        val height: Int,
        val label: String
    )

    private companion object {
        const val REQUEST_STORAGE = 4101
        const val MIN_RESOLUTION_HEIGHT = 360
        const val RESOLUTION_HEIGHT_STEP = 180
        const val DEFAULT_SCREEN_WIDTH = 1280
        const val DEFAULT_SCREEN_HEIGHT = 720
    }
}
