package wc3.recomp

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.Editable
import android.text.InputFilter
import android.text.TextWatcher
import android.util.DisplayMetrics
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.SeekBar
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
import wc3.recomp.launch.VulkanDriverStore

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
    private lateinit var gestureModeSpinner: Spinner
    private lateinit var gestureModeHint: TextView
    private lateinit var doubleTapDelayGroup: LinearLayout
    private lateinit var doubleTapDelaySlider: SeekBar
    private lateinit var doubleTapDelayValue: TextView
    private lateinit var longPressDelaySlider: SeekBar
    private lateinit var longPressDelayValue: TextView
    private lateinit var argsInput: EditText
    private lateinit var driverStore: VulkanDriverStore
    private lateinit var driverSpinner: Spinner
    private lateinit var driverStatus: TextView
    private lateinit var driverRemoveButton: Button
    private lateinit var driverImportButton: Button
    private lateinit var dxvkSpinner: Spinner
    private lateinit var dxvkConfigRows: LinearLayout
    private lateinit var dxvkConfigAddButton: Button

    /* DXVK's config parser accepts only these key characters and has no escape
     * for a double quote inside a value.  Filtering at input time keeps the
     * generated key-value file unambiguous. */
    private val dxvkKeyFilter = InputFilter { source, start, end, _, _, _ ->
        val input = source.subSequence(start, end)
        val filtered = input.filter(::isDxvkKeyChar)
        if (filtered.length == input.length) null else filtered
    }
    private val dxvkValueFilter = InputFilter { source, start, end, _, _, _ ->
        val input = source.subSequence(start, end)
        val filtered = input.filter { it != '"' && it != '\n' && it != '\r' }
        if (filtered.length == input.length) null else filtered
    }

    private lateinit var errorBanner: LinearLayout
    private lateinit var errorText: TextView
    private lateinit var grantButton: Button
    private lateinit var playButton: Button

    private var resolutionOptions: List<ResolutionOption> = emptyList()
    private var gestureModeOptions: List<GestureModeOption> = emptyList()
    private var driverOptions: List<DriverOption> = emptyList()
    private var dxvkOptions: List<DxvkOption> = emptyList()
    private var drivers: List<VulkanDriverStore.DriverInfo> = emptyList()
    private var binding = false
    private var storageRequested = false
    private var driverBusy = false

    private val folderPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) usePickedFolder(uri)
    }

    private val driverPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) importDriver(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        mode = GameMode.fromId(intent?.getStringExtra(LaunchContract.EXTRA_MODE))
            ?: GameMode.TFT
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launch_settings)

        store = LaunchSettingsStore(this)
        driverStore = VulkanDriverStore(this)
        drivers = driverStore.list()
        dxvkOptions = listOf(
            DxvkOption("", getString(R.string.launch_dxvk_auto)),
            DxvkOption(DXVK_VERSION_1_10, getString(R.string.launch_dxvk_110)),
            DxvkOption(DXVK_VERSION_3_1_1, getString(R.string.launch_dxvk_311))
        )
        gestureModeOptions = listOf(
            GestureModeOption(
                doubleTap = true,
                label = getString(R.string.launch_gesture_mode_double),
                hint = getString(R.string.launch_gesture_hint_double)
            ),
            GestureModeOption(
                doubleTap = false,
                label = getString(R.string.launch_gesture_mode_single),
                hint = getString(R.string.launch_gesture_hint_single)
            )
        )
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
        gestureModeSpinner = findViewById(R.id.gesture_mode_spinner)
        gestureModeHint = findViewById(R.id.gesture_mode_hint)
        doubleTapDelayGroup = findViewById(R.id.double_tap_delay_group)
        doubleTapDelaySlider = findViewById(R.id.double_tap_delay_slider)
        doubleTapDelayValue = findViewById(R.id.double_tap_delay_value)
        longPressDelaySlider = findViewById(R.id.long_press_delay_slider)
        longPressDelayValue = findViewById(R.id.long_press_delay_value)
        argsInput = findViewById(R.id.args_input)
        driverSpinner = findViewById(R.id.driver_spinner)
        driverStatus = findViewById(R.id.driver_status)
        driverRemoveButton = findViewById(R.id.driver_remove_button)
        driverImportButton = findViewById(R.id.driver_import_button)
        dxvkSpinner = findViewById(R.id.dxvk_spinner)
        dxvkConfigRows = findViewById(R.id.dxvk_config_rows)
        dxvkConfigAddButton = findViewById(R.id.dxvk_config_add)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            // libadrenotools needs Android 9; keep the system-only path and
            // do not expose an option that cannot work.
            for (id in intArrayOf(
                R.id.driver_label,
                R.id.driver_picker_frame,
                R.id.driver_buttons,
                R.id.driver_status
            )) findViewById<View>(id).visibility = View.GONE
        }

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

        gestureModeSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_gesture_mode,
            gestureModeOptions.map { it.label }
        ).apply {
            setDropDownViewResource(R.layout.item_gesture_mode_dropdown)
        }

        driverSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            emptyList<String>()
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }

        dxvkSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            dxvkOptions.map { it.label }
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
        gestureModeSpinner.setSelection(
            gestureModeOptions.indexOfFirst {
                it.doubleTap == settings.doubleTapGestures
            }.coerceAtLeast(0),
            false
        )
        bindDelaySlider(
            doubleTapDelaySlider,
            doubleTapDelayValue,
            settings.doubleTapTimeoutMs
        )
        bindDelaySlider(
            longPressDelaySlider,
            longPressDelayValue,
            settings.longPressTimeoutMs
        )
        argsInput.setText(settings.extraArgs)
        rebuildDriverOptions(settings.driverId)
        updateDriverStatus()

        val dxvkIndex = dxvkOptions.indexOfFirst { it.value == settings.dxvkVersion }
        val defaultDxvkIndex =
            dxvkOptions.indexOfFirst {
                it.value == LaunchContract.DEFAULT_DXVK_VERSION
            }
        dxvkSpinner.setSelection(
            if (dxvkIndex >= 0) dxvkIndex else defaultDxvkIndex.coerceAtLeast(0),
            false
        )
        setDxvkConfig(settings.dxvkConfig)
        binding = false
        updateGestureModeHint()
    }

    private fun updateGestureModeHint() {
        val option = gestureModeOptions.getOrNull(gestureModeSpinner.selectedItemPosition)
            ?: return
        gestureModeHint.text = option.hint
        doubleTapDelayGroup.visibility =
            if (option.doubleTap) View.VISIBLE else View.GONE
    }

    private fun wireListeners() {
        folderInput.afterTextChanged { if (!binding) refreshValidation() }
        argsInput.afterTextChanged { if (!binding) refreshValidation() }

        folderInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }
        argsInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }

        soundSwitch.setOnCheckedChangeListener { _, _ ->
            if (!binding) {
                refreshValidation()
                saveIfValid()
            }
        }

        gestureModeSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                position: Int,
                id: Long
            ) {
                if (!binding) {
                    updateGestureModeHint()
                    refreshValidation()
                    saveIfValid()
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }

        wireDelaySlider(doubleTapDelaySlider, doubleTapDelayValue)
        wireDelaySlider(longPressDelaySlider, longPressDelayValue)

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

        driverSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                position: Int,
                id: Long
            ) {
                if (!binding) {
                    updateDriverStatus()
                    refreshValidation()
                    saveIfValid()
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }

        dxvkSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                position: Int,
                id: Long
            ) {
                if (!binding) saveIfValid()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }

        driverImportButton.setOnClickListener { pickDriverPackage() }
        driverRemoveButton.setOnClickListener { removeSelectedDriver() }
        dxvkConfigAddButton.setOnClickListener {
            addDxvkConfigRow().requestFocus()
        }

        findViewById<Button>(R.id.choose_folder_button).setOnClickListener { pickFolder() }
        findViewById<Button>(R.id.find_folder_button).setOnClickListener { findFolder() }
        grantButton.setOnClickListener { requestStoragePermission() }
        playButton.setOnClickListener { play() }
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
                R.string.launch_resolution_option,
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

    private fun pickDriverPackage() {
        if (driverBusy) return
        try {
            driverPicker.launch(
                arrayOf("application/zip", "application/octet-stream", "*/*")
            )
        } catch (ignored: ActivityNotFoundException) {
            Toast.makeText(
                this,
                R.string.launch_driver_picker_missing,
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun importDriver(uri: Uri) {
        if (driverBusy) return
        driverBusy = true
        setDriverBusy(true)
        driverStatus.text = getString(R.string.launch_driver_installing)
        Thread {
            val result = driverStore.install(uri)
            runOnUiThread {
                driverBusy = false
                if (isFinishing || isDestroyed) return@runOnUiThread
                when (result) {
                    is VulkanDriverStore.InstallResult.Ok -> {
                        drivers = driverStore.list()
                        rebuildDriverOptions(result.info.id)
                        updateDriverStatus()
                        setDriverBusy(false)
                        refreshValidation()
                        saveIfValid()
                        Toast.makeText(
                            this,
                            getString(
                                R.string.launch_driver_installed_toast,
                                result.info.displayName
                            ),
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                    is VulkanDriverStore.InstallResult.Error -> {
                        updateDriverStatus()
                        setDriverBusy(false)
                        refreshValidation()
                        Toast.makeText(
                            this,
                            driverErrorMessage(result),
                            Toast.LENGTH_LONG
                        ).show()
                    }
                }
            }
        }.start()
    }

    private fun removeSelectedDriver() {
        val option = driverOptions.getOrNull(driverSpinner.selectedItemPosition)
            ?: return
        if (option.id.isEmpty() || driverBusy) return
        val info = drivers.firstOrNull { it.id == option.id }
        AlertDialog.Builder(this)
            .setMessage(
                getString(
                    R.string.launch_driver_remove_confirm,
                    info?.displayName ?: option.label
                )
            )
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val wasSelected = option.id == currentDriverId()
                if (driverStore.remove(option.id)) {
                    drivers = driverStore.list()
                    rebuildDriverOptions(if (wasSelected) "" else currentDriverId())
                    updateDriverStatus()
                    refreshValidation()
                    saveIfValid()
                    Toast.makeText(
                        this,
                        R.string.launch_driver_removed,
                        Toast.LENGTH_SHORT
                    ).show()
                } else {
                    Toast.makeText(
                        this,
                        R.string.launch_driver_error_io,
                        Toast.LENGTH_LONG
                    ).show()
                }
            }
            .show()
    }

    private fun rebuildDriverOptions(selectId: String) {
        /* Called both from bindSettings (already inside a binding guard) and
         * from import/remove (outside it).  Preserve the caller's state so the
         * saved DXVK selection cannot be written back as "auto" while the
         * form is still being populated. */
        val wasBinding = binding
        binding = true
        val available = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            drivers
        } else {
            emptyList()
        }
        driverOptions = listOf(
            DriverOption("", getString(R.string.launch_driver_system))
        ) + available.map { info ->
            val version = info.driverVersion.ifBlank { "Vulkan" }
            DriverOption(
                info.id,
                getString(R.string.launch_driver_installed, info.displayName, version)
            )
        }
        driverSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            driverOptions.map { it.label }
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }
        val index = driverOptions.indexOfFirst { it.id == selectId }
        driverSpinner.setSelection(if (index >= 0) index else 0, false)
        binding = wasBinding
    }

    private fun updateDriverStatus() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return
        val id = currentDriverId()
        val info = drivers.firstOrNull { it.id == id }
        if (info == null) {
            driverStatus.text = ""
            driverRemoveButton.isEnabled = false
            return
        }
        val version = info.driverVersion.ifBlank { "Vulkan" }
        val megabytes = (info.size / (1024L * 1024L)).coerceAtLeast(0L)
        driverStatus.text = getString(
            R.string.launch_driver_details,
            info.displayName,
            version,
            megabytes
        )
        driverRemoveButton.isEnabled = !driverBusy
    }

    private fun currentDriverId(): String =
        driverOptions.getOrNull(driverSpinner.selectedItemPosition)?.id ?: ""

    private fun currentDxvkVersion(): String =
        dxvkOptions.getOrNull(dxvkSpinner.selectedItemPosition)?.value
            ?: LaunchContract.DEFAULT_DXVK_VERSION

    private fun setDriverBusy(busy: Boolean) {
        driverSpinner.isEnabled = !busy
        driverImportButton.isEnabled = !busy
        driverRemoveButton.isEnabled = !busy && currentDriverId().isNotEmpty()
        dxvkSpinner.isEnabled = !busy
        if (busy) playButton.isEnabled = false
    }

    private fun setDxvkConfig(config: String) {
        dxvkConfigRows.removeAllViews()
        parseDxvkConfig(config).forEach { (key, value) ->
            addDxvkConfigRow(key, value)
        }
    }

    private fun addDxvkConfigRow(key: String = "", value: String = ""): EditText {
        val row = layoutInflater.inflate(R.layout.item_dxvk_config, dxvkConfigRows, false)
        val keyInput = row.findViewById<EditText>(R.id.dxvk_config_key)
        val valueInput = row.findViewById<EditText>(R.id.dxvk_config_value)
        keyInput.filters = arrayOf(dxvkKeyFilter, InputFilter.LengthFilter(80))
        valueInput.filters = arrayOf(dxvkValueFilter, InputFilter.LengthFilter(200))
        keyInput.setText(key)
        valueInput.setText(value)

        val saveOnBlur = View.OnFocusChangeListener { _, focused ->
            if (!focused && !binding) saveIfValid()
        }
        keyInput.setOnFocusChangeListener(saveOnBlur)
        valueInput.setOnFocusChangeListener(saveOnBlur)

        row.findViewById<Button>(R.id.dxvk_config_remove).setOnClickListener {
            dxvkConfigRows.removeView(row)
            if (!binding) saveIfValid()
        }
        dxvkConfigRows.addView(row)
        return keyInput
    }

    private fun readDxvkConfig(): String {
        val lines = mutableListOf<String>()
        for (index in 0 until dxvkConfigRows.childCount) {
            val row = dxvkConfigRows.getChildAt(index)
            val key = row.findViewById<EditText>(R.id.dxvk_config_key)
                .text.toString().trim()
            if (key.isEmpty() || !key.all(::isDxvkKeyChar)) continue

            /* InputFilter is not applied to text set programmatically from a
             * saved preference, so sanitize once more before serialization.
             * DXVK has no escape for a quote and treats CR as whitespace. */
            val value = row.findViewById<EditText>(R.id.dxvk_config_value)
                .text.toString().trim()
                .filter { it != '"' && it != '\n' && it != '\r' }
            val encoded = if (value.any { it.isWhitespace() }) "\"$value\"" else value
            lines += "$key = $encoded"
        }
        return lines.joinToString("\n")
    }

    private fun parseDxvkConfig(config: String): List<Pair<String, String>> =
        config.lineSequence().mapNotNull { raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) return@mapNotNull null

            val equals = line.indexOf('=')
            if (equals <= 0) return@mapNotNull null

            val key = line.substring(0, equals).trim()
            if (key.isEmpty() || !key.all(::isDxvkKeyChar)) return@mapNotNull null

            var value = line.substring(equals + 1).trim()
            if (value.length >= 2 && value.startsWith("\"") && value.endsWith("\"")) {
                value = value.substring(1, value.length - 1)
            }
            key to value
        }.toList()

    private fun isDxvkKeyChar(character: Char): Boolean =
        character in 'a'..'z' ||
            character in 'A'..'Z' ||
            character in '0'..'9' ||
            character == '.' ||
            character == '_'

    private fun driverErrorMessage(
        result: VulkanDriverStore.InstallResult.Error
    ): String {
        val resource = when (result.kind) {
            VulkanDriverStore.ErrorKind.OPEN -> R.string.launch_driver_error_open
            VulkanDriverStore.ErrorKind.ZIP -> R.string.launch_driver_error_zip
            VulkanDriverStore.ErrorKind.META -> R.string.launch_driver_error_meta
            VulkanDriverStore.ErrorKind.LIBRARY -> R.string.launch_driver_error_library
            VulkanDriverStore.ErrorKind.ELF -> R.string.launch_driver_error_elf
            VulkanDriverStore.ErrorKind.IO -> R.string.launch_driver_error_io
            VulkanDriverStore.ErrorKind.API -> R.string.launch_driver_error_api
            VulkanDriverStore.ErrorKind.DUPLICATE -> R.string.launch_driver_error_duplicate
        }
        return getString(resource)
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

    private fun play() {
        val settings = readForm()
        if (settings.driverId.isNotEmpty() &&
            drivers.none { it.id == settings.driverId }
        ) {
            Toast.makeText(
                this,
                R.string.launch_driver_error_library,
                Toast.LENGTH_LONG
            ).show()
            refreshValidation()
            return
        }
        val result = LaunchValidator.validate(this, settings)
        if (result !is LaunchValidator.Result.Ok) {
            refreshValidation()
            return
        }

        if (!store.save(settings)) {
            Toast.makeText(this, R.string.launch_error_save, Toast.LENGTH_LONG).show()
            return
        }

        LaunchCoordinator.startGame(this, settings)
        finishAndRemoveTask()
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
            extraArgs = ""
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
            driverId = currentDriverId(),
            dxvkVersion = currentDxvkVersion(),
            dxvkConfig = readDxvkConfig(),
            doubleTapGestures = gestureModeOptions
                .getOrNull(gestureModeSpinner.selectedItemPosition)?.doubleTap ?: true,
            doubleTapTimeoutMs = progressToDelay(doubleTapDelaySlider.progress),
            longPressTimeoutMs = progressToDelay(longPressDelaySlider.progress)
        )
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

    private fun bindDelaySlider(slider: SeekBar, valueView: TextView, ms: Int) {
        val progress = delayToProgress(ms)
        slider.progress = progress
        valueView.text = getString(
            R.string.launch_gesture_delay_value,
            progressToDelay(progress)
        )
    }

    private fun wireDelaySlider(slider: SeekBar, valueView: TextView) {
        slider.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(
                seekBar: SeekBar?,
                progress: Int,
                fromUser: Boolean
            ) {
                valueView.text = getString(
                    R.string.launch_gesture_delay_value,
                    progressToDelay(progress)
                )
                if (fromUser && !binding) {
                    refreshValidation()
                    saveIfValid()
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit

            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
    }

    /* The slider is an index, not milliseconds: max = 10 maps 100..200 ms in
     * exact 10 ms steps on every Android version. */
    private fun delayToProgress(ms: Int): Int {
        val clamped = ms.coerceIn(MIN_GESTURE_DELAY_MS, MAX_GESTURE_DELAY_MS)
        return ((clamped - MIN_GESTURE_DELAY_MS + GESTURE_DELAY_STEP_MS / 2) /
            GESTURE_DELAY_STEP_MS).coerceIn(0, GESTURE_DELAY_STEPS)
    }

    private fun progressToDelay(progress: Int): Int =
        MIN_GESTURE_DELAY_MS +
            progress.coerceIn(0, GESTURE_DELAY_STEPS) * GESTURE_DELAY_STEP_MS

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

    private data class GestureModeOption(
        val doubleTap: Boolean,
        val label: String,
        val hint: String
    )

    private data class DriverOption(
        val id: String,
        val label: String
    )

    private data class DxvkOption(
        val value: String,
        val label: String
    )

    private companion object {
        const val REQUEST_STORAGE = 4101
        const val MIN_RESOLUTION_HEIGHT = 360
        const val RESOLUTION_HEIGHT_STEP = 180
        const val DEFAULT_SCREEN_WIDTH = 1280
        const val DEFAULT_SCREEN_HEIGHT = 720
        const val MIN_GESTURE_DELAY_MS = 100
        const val MAX_GESTURE_DELAY_MS = 200
        const val GESTURE_DELAY_STEP_MS = 10
        const val GESTURE_DELAY_STEPS =
            (MAX_GESTURE_DELAY_MS - MIN_GESTURE_DELAY_MS) / GESTURE_DELAY_STEP_MS
        const val DXVK_VERSION_1_10 = "1"
        const val DXVK_VERSION_3_1_1 = "3"
    }
}
