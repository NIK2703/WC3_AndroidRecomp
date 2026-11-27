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
import android.view.Gravity
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.Button
import android.widget.EditText
import android.widget.GridLayout
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
import androidx.viewpager.widget.PagerAdapter
import androidx.viewpager.widget.ViewPager
import wc3.recomp.launch.CpuCore
import wc3.recomp.launch.CpuTopology
import wc3.recomp.launch.DxvkConfigOptions
import wc3.recomp.launch.DxvkSuggestionAdapter
import wc3.recomp.launch.GameFolderPicker
import wc3.recomp.launch.GameMode
import wc3.recomp.launch.GestureMode
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

    private lateinit var pager: ViewPager
    private lateinit var tabs: List<View>
    private lateinit var gamePage: View
    private lateinit var videoPage: View
    private lateinit var controlsPage: View

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
    private lateinit var overlayOpacitySlider: SeekBar
    private lateinit var overlayOpacityValue: TextView
    private lateinit var driverStore: VulkanDriverStore
    private lateinit var driverSpinner: Spinner
    private lateinit var driverRemoveButton: Button
    private lateinit var driverImportButton: Button
    private lateinit var dxvkSpinner: Spinner
    private lateinit var dxvkConfigRows: LinearLayout
    private lateinit var dxvkConfigAddButton: Button
    private lateinit var dxvkConfigImportButton: Button
    private lateinit var selectionCapSpinner: Spinner
    private val selectionCapValues = listOf(
        LaunchContract.SELECTION_CAP_12,
        LaunchContract.SELECTION_CAP_24,
        LaunchContract.SELECTION_CAP_40
    )
    private lateinit var hideCursorSwitch: SwitchCompat
    private lateinit var parallelCpuGrid: GridLayout
    private var availableCpuCores: List<CpuCore> = emptyList()
    private val cpuCoreCells = mutableListOf<TextView>()
    private lateinit var gridOptionsGroup: LinearLayout
    private lateinit var gridDarkenSlider: SeekBar
    private lateinit var gridDarkenValue: TextView
    private lateinit var gridCycleSlider: SeekBar
    private lateinit var gridCycleValue: TextView
    private lateinit var presentModeSpinner: Spinner
    private lateinit var presentBuffersSpinner: Spinner

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
    private var dxvkOptions: List<ValueOption> = emptyList()
    private var presentModeOptions: List<ValueOption> = emptyList()
    private var presentBufferOptions: List<ValueOption> = emptyList()
    private var drivers: List<VulkanDriverStore.DriverInfo> = emptyList()
    private var binding = false
    private var storageRequested = false
    private var driverBusy = false
    private var dxvkConfigImportBusy = false
    /* The table always ends with one empty row used to type a new variable. */
    private var dxvkScratchRow: View? = null

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

    private val dxvkConfigPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) importDxvkConfig(uri)
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
            ValueOption("", getString(R.string.launch_dxvk_auto)),
            ValueOption(DXVK_VERSION_1_10, getString(R.string.launch_dxvk_110)),
            ValueOption(DXVK_VERSION_3_1_1, getString(R.string.launch_dxvk_311)),
            ValueOption(
                LaunchContract.DXVK_VERSION_LOW_LATENCY,
                getString(R.string.launch_dxvk_lowlatency)
            )
        )
        gestureModeOptions = listOf(
            GestureModeOption(
                mode = GestureMode.DOUBLE_TAP,
                label = getString(R.string.launch_gesture_mode_double),
                hint = getString(R.string.launch_gesture_hint_double)
            ),
            GestureModeOption(
                mode = GestureMode.SINGLE_TAP,
                label = getString(R.string.launch_gesture_mode_single),
                hint = getString(R.string.launch_gesture_hint_single)
            ),
            GestureModeOption(
                mode = GestureMode.DIRECT_TOUCH,
                label = getString(R.string.launch_gesture_mode_direct),
                hint = getString(R.string.launch_gesture_hint_direct)
            )
        )
        presentModeOptions = listOf(
            ValueOption(
                LaunchContract.PRESENT_MODE_FIFO,
                getString(R.string.launch_present_mode_fifo)
            ),
            ValueOption(
                LaunchContract.PRESENT_MODE_FIFO_RELAXED,
                getString(R.string.launch_present_mode_fifo_relaxed)
            ),
            ValueOption(
                LaunchContract.PRESENT_MODE_MAILBOX,
                getString(R.string.launch_present_mode_mailbox)
            ),
            ValueOption(
                LaunchContract.PRESENT_MODE_IMMEDIATE,
                getString(R.string.launch_present_mode_immediate)
            )
        )
        presentBufferOptions = listOf(
            ValueOption(
                LaunchContract.MIN_PRESENT_BUFFERS.toString(),
                getString(R.string.launch_present_buffers_min)
            ),
            ValueOption(
                (LaunchContract.MIN_PRESENT_BUFFERS + 1).toString(),
                getString(R.string.launch_present_buffers_plus_one)
            ),
            ValueOption(
                (LaunchContract.MIN_PRESENT_BUFFERS + 2).toString(),
                getString(R.string.launch_present_buffers_plus_two)
            )
        )
        val settings = store.load(mode)
        availableCpuCores = CpuTopology.availableCores()
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
        val pageAdapter = SettingsPagerAdapter(layoutInflater, PAGE_LAYOUTS)
        pager = findViewById(R.id.settings_pager)
        pager.adapter = pageAdapter
        pager.offscreenPageLimit = PAGE_LAYOUTS.size - 1

        gamePage = pageAdapter.page(GAME_PAGE)
        videoPage = pageAdapter.page(VIDEO_PAGE)
        controlsPage = pageAdapter.page(CONTROLS_PAGE)

        tabs = listOf(
            findViewById(R.id.tab_game),
            findViewById(R.id.tab_video),
            findViewById(R.id.tab_controls)
        )
        tabs.forEachIndexed { index, tab ->
            tab.setOnClickListener { pager.setCurrentItem(index, true) }
        }
        pager.addOnPageChangeListener(object : ViewPager.SimpleOnPageChangeListener() {
            override fun onPageSelected(position: Int) {
                updateTabs(position)
            }
        })
        updateTabs(pager.currentItem)

        folderInput = gamePage.findViewById(R.id.folder_input)
        folderStatus = gamePage.findViewById(R.id.folder_status)
        soundSwitch = gamePage.findViewById(R.id.sound_switch)
        selectionCapSpinner = gamePage.findViewById(R.id.selection_cap_spinner)
        parallelCpuGrid = gamePage.findViewById(R.id.parallel_cpu_grid)

        resolutionSpinner = videoPage.findViewById(R.id.resolution_spinner)
        hideCursorSwitch = gamePage.findViewById(R.id.hide_cursor_switch)
        gridOptionsGroup = videoPage.findViewById(R.id.grid_options_group)
        gridDarkenSlider = videoPage.findViewById(R.id.grid_darken_slider)
        gridDarkenValue = videoPage.findViewById(R.id.grid_darken_value)
        gridCycleSlider = videoPage.findViewById(R.id.grid_cycle_slider)
        gridCycleValue = videoPage.findViewById(R.id.grid_cycle_value)
        presentModeSpinner = videoPage.findViewById(R.id.present_mode_spinner)
        presentBuffersSpinner = videoPage.findViewById(R.id.present_buffers_spinner)
        driverSpinner = videoPage.findViewById(R.id.driver_spinner)
        driverRemoveButton = videoPage.findViewById(R.id.driver_remove_button)
        driverImportButton = videoPage.findViewById(R.id.driver_import_button)
        dxvkSpinner = videoPage.findViewById(R.id.dxvk_spinner)
        dxvkConfigRows = videoPage.findViewById(R.id.dxvk_config_rows)
        dxvkConfigAddButton = videoPage.findViewById(R.id.dxvk_config_add)
        dxvkConfigImportButton = videoPage.findViewById(R.id.dxvk_config_import)

        gestureModeSpinner = controlsPage.findViewById(R.id.gesture_mode_spinner)
        gestureModeHint = controlsPage.findViewById(R.id.gesture_mode_hint)
        doubleTapDelayGroup = controlsPage.findViewById(R.id.double_tap_delay_group)
        doubleTapDelaySlider = controlsPage.findViewById(R.id.double_tap_delay_slider)
        doubleTapDelayValue = controlsPage.findViewById(R.id.double_tap_delay_value)
        longPressDelaySlider = controlsPage.findViewById(R.id.long_press_delay_slider)
        longPressDelayValue = controlsPage.findViewById(R.id.long_press_delay_value)
        overlayOpacitySlider = controlsPage.findViewById(R.id.overlay_opacity_slider)
        overlayOpacityValue = controlsPage.findViewById(R.id.overlay_opacity_value)

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            // libadrenotools needs Android 9; keep the system-only path and
            // do not expose an option that cannot work.
            for (id in intArrayOf(
                R.id.driver_label,
                R.id.driver_picker_frame,
                R.id.driver_buttons
            )) videoPage.findViewById<View>(id).visibility = View.GONE
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

        presentModeSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            presentModeOptions.map { it.label }
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }

        presentBuffersSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            presentBufferOptions.map { it.label }
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }

        selectionCapSpinner.adapter = ArrayAdapter(
            this,
            R.layout.item_resolution,
            listOf(
                getString(R.string.launch_selection_12),
                getString(R.string.launch_selection_24),
                getString(R.string.launch_selection_40)
            )
        ).apply {
            setDropDownViewResource(R.layout.item_resolution_dropdown)
        }
    }

    private fun updateTabs(position: Int) {
        tabs.forEachIndexed { index, tab ->
            tab.isSelected = index == position
        }
    }

    /* A horizontal drag on a slider or a text field belongs to that control,
     * not to the page swipe. The ScrollView parent still handles vertical
     * scrolling. */
    private fun guardFromPager(view: View) {
        view.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN ->
                    pager.requestDisallowInterceptTouchEvent(true)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                    pager.requestDisallowInterceptTouchEvent(false)
            }
            false
        }
    }

    private fun bindSettings(settings: LaunchSettings) {
        binding = true
        folderInput.setText(settings.gameDir)

        val resolutionIndex = if (settings.gridEnabled) {
            resolutionOptions.indexOfFirst { it.grid }
        } else {
            resolutionOptions.indexOfFirst {
                !it.grid &&
                    it.width == settings.width &&
                    it.height == settings.height
            }
        }
        resolutionSpinner.setSelection(
            if (resolutionIndex >= 0) resolutionIndex else resolutionOptions.lastIndex,
            false
        )

        soundSwitch.isChecked = settings.sound
        selectionCapSpinner.setSelection(
            selectionCapValues.indexOf(settings.selectionCap).coerceAtLeast(0),
            false
        )
        hideCursorSwitch.isChecked = settings.hideCursor
        bindCpuCores(settings.workerCpus)
        bindGridDarkenSlider(settings.gridDarkenPercent)
        bindGridCycleSlider(settings.gridCycleSeconds)
        updateGridOptions()
        gestureModeSpinner.setSelection(
            gestureModeOptions.indexOfFirst {
                it.mode == settings.gestureMode
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
        bindOverlayOpacitySlider(settings.overlayOpacityPercent)
        rebuildDriverOptions(settings.driverId)
        updateDriverState()

        val dxvkIndex = dxvkOptions.indexOfFirst { it.value == settings.dxvkVersion }
        val defaultDxvkIndex =
            dxvkOptions.indexOfFirst {
                it.value == LaunchContract.DEFAULT_DXVK_VERSION
            }
        dxvkSpinner.setSelection(
            if (dxvkIndex >= 0) dxvkIndex else defaultDxvkIndex.coerceAtLeast(0),
            false
        )

        val presentModeIndex = presentModeOptions.indexOfFirst {
            it.value == settings.presentMode
        }
        presentModeSpinner.setSelection(
            if (presentModeIndex >= 0) presentModeIndex else 0,
            false
        )
        val presentBuffersIndex = presentBufferOptions.indexOfFirst {
            it.value == settings.presentBuffers.toString()
        }
        presentBuffersSpinner.setSelection(
            if (presentBuffersIndex >= 0) presentBuffersIndex else 0,
            false
        )

        setDxvkConfig(settings.dxvkConfig)
        binding = false
        updateGestureModeHint()
    }

    /* The core table is a single-choice main-thread affinity.  The selected
     * cell pins the guest main thread and there is no worker list.  The
     * default is the most performant allowed CPU, so a fresh install starts
     * sequential and pins the main thread there. */
    private fun bindCpuCores(saved: List<Int>) {
        parallelCpuGrid.removeAllViews()
        cpuCoreCells.clear()

        val availableIds = availableCpuCores.map { it.id }.toSet()
        val savedIds = saved.filter { it in availableIds }.toSet()
        /* One cell is the main-thread core.  An old multi-core preference is
         * collapsed to its most performant member so the serial product never
         * starts with a worker list. */
        val selectedId = if (savedIds.isEmpty()) {
            CpuTopology.pickBest(availableCpuCores)?.id
        } else {
            CpuTopology.pickBest(
                availableCpuCores.filter { it.id in savedIds }
            )?.id ?: savedIds.minOrNull()
        }
        val selected = selectedId?.let { setOf(it) }.orEmpty()

        /* Two rows, not two columns: all cores fill the first row and spill
         * into the second one.  The column count is derived from the core
         * count so the grid stays two rows on every device. */
        val columns = (availableCpuCores.size + 1) / 2
        if (columns > 0) {
            parallelCpuGrid.columnCount = columns
            parallelCpuGrid.rowCount = minOf(2, availableCpuCores.size)
        }

        for ((index, core) in availableCpuCores.withIndex()) {
            val cell = TextView(this).apply {
                tag = core.id
                text = core.id.toString()
                isSelected = core.id in selected
                gravity = Gravity.CENTER
                textSize = 16f
                minHeight = dp(48)
                ContextCompat.getColorStateList(
                    this@LaunchSettingsActivity,
                    R.color.cpu_cell_text
                )?.let { setTextColor(it) }
                setBackgroundResource(R.drawable.bg_cpu_cell)
                contentDescription = this@LaunchSettingsActivity.getString(
                    R.string.launch_cpu_core_description,
                    core.id
                )
                tooltipText = core.label
                isClickable = true
                isFocusable = true
                setOnClickListener { toggleCpuCore(this@apply) }
            }
            val column = index % columns
            val row = index / columns
            /* A column with one core must carry the weight of the two-core
             * columns so every cell keeps the same width on an odd count. */
            val columnCells = if (column + columns < availableCpuCores.size) 2 else 1
            val params = GridLayout.LayoutParams().apply {
                width = 0
                height = dp(48)
                columnSpec = GridLayout.spec(
                    column,
                    if (columnCells == 1) 2f else 1f
                )
                rowSpec = GridLayout.spec(row)
                setMargins(dp(4), dp(4), dp(4), dp(4))
            }
            parallelCpuGrid.addView(cell, params)
            cpuCoreCells.add(cell)
        }
    }

    private fun toggleCpuCore(cell: TextView) {
        /* Single choice: the selected cell is the main-thread affinity.
         * Clicking the current cell keeps it; clicking another moves it. */
        for (other in cpuCoreCells) {
            other.isSelected = other === cell
        }
        refreshValidation()
        saveIfValid()
    }

    private fun selectedCpuCores(): List<Int> =
        cpuCoreCells
            .filter { it.isSelected }
            .mapNotNull { it.tag as? Int }
            .sorted()

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()

    private fun updateGestureModeHint() {
        val option = gestureModeOptions.getOrNull(gestureModeSpinner.selectedItemPosition)
            ?: return
        gestureModeHint.text = option.hint
        doubleTapDelayGroup.visibility =
            if (option.mode == GestureMode.DOUBLE_TAP) View.VISIBLE else View.GONE
    }

    private fun wireListeners() {
        folderInput.afterTextChanged { if (!binding) refreshValidation() }

        folderInput.setOnFocusChangeListener { _, focused -> if (!focused) saveIfValid() }

        soundSwitch.setOnCheckedChangeListener { _, _ ->
            if (!binding) {
                refreshValidation()
                saveIfValid()
            }
        }

        selectionCapSpinner.onItemSelectedListener =
            object : AdapterView.OnItemSelectedListener {
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

        hideCursorSwitch.setOnCheckedChangeListener { _, _ ->
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

        listOf<View>(
            folderInput,
            gridDarkenSlider,
            gridCycleSlider,
            doubleTapDelaySlider,
            longPressDelaySlider,
            overlayOpacitySlider
        ).forEach(::guardFromPager)
        wireDelaySlider(doubleTapDelaySlider, doubleTapDelayValue)
        wireDelaySlider(longPressDelaySlider, longPressDelayValue)
        wireOverlayOpacitySlider()
        wireGridDarkenSlider()
        wireGridCycleSlider()
        wireSettingsSpinner(presentModeSpinner)
        wireSettingsSpinner(presentBuffersSpinner)

        resolutionSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                position: Int,
                id: Long
            ) {
                updateGridOptions()
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
                    updateDriverState()
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
            ensureScratchRow()
            dxvkScratchRow
                ?.findViewById<AutoCompleteTextView>(R.id.dxvk_config_key)
                ?.requestFocus()
        }
        dxvkConfigImportButton.setOnClickListener { pickDxvkConfig() }

        gamePage.findViewById<Button>(R.id.choose_folder_button)
            .setOnClickListener { pickFolder() }
        gamePage.findViewById<Button>(R.id.find_folder_button)
            .setOnClickListener { findFolder() }
        grantButton.setOnClickListener { requestStoragePermission() }
        playButton.setOnClickListener { play() }
    }

    /* Plain value spinners only persist; unlike the gesture mode they do not
     * change the visibility of another row. */
    private fun wireSettingsSpinner(spinner: Spinner) {
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
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

        /* Half-resolution is one more display option, not a separate switch.
         * It carries the exact extent the guest renders at and stays in the
         * ascending order of the list. */
        if (screenWidth >= 2 && screenHeight >= 2) {
            val gridWidth = screenWidth / 2
            val gridHeight = screenHeight / 2
            val gridArea = gridWidth.toLong() * gridHeight
            val grid = ResolutionOption(
                width = 0,
                height = 0,
                label = getString(
                    R.string.launch_resolution_grid_option,
                    gridWidth,
                    gridHeight
                ),
                grid = true
            )
            val index = options.indexOfFirst { option ->
                val width = if (option.width > 0) option.width else screenWidth
                val height = if (option.height > 0) option.height else screenHeight
                width.toLong() * height > gridArea
            }
            if (index < 0) options += grid else options.add(index, grid)
        }
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

    private fun pickDxvkConfig() {
        if (dxvkConfigImportBusy) return
        try {
            dxvkConfigPicker.launch(
                arrayOf("text/plain", "application/octet-stream", "*/*")
            )
        } catch (ignored: ActivityNotFoundException) {
            Toast.makeText(
                this,
                R.string.launch_dxvk_config_import_missing,
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun importDxvkConfig(uri: Uri) {
        if (dxvkConfigImportBusy) return
        dxvkConfigImportBusy = true
        dxvkConfigImportButton.isEnabled = false
        val resolver = contentResolver
        Thread {
            val text = try {
                resolver.openInputStream(uri)?.use { input ->
                    val reader = input.bufferedReader(Charsets.UTF_8)
                    val buffer = CharArray(8192)
                    val content = StringBuilder()
                    var tooLarge = false
                    while (true) {
                        val count = reader.read(buffer)
                        if (count < 0) break
                        if (content.length + count > MAX_DXVK_CONFIG_FILE_CHARS) {
                            tooLarge = true
                            break
                        }
                        content.append(buffer, 0, count)
                    }
                    if (tooLarge) null else content.toString().removePrefix("\uFEFF")
                }
            } catch (ignored: Exception) {
                null
            }

            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                dxvkConfigImportBusy = false
                dxvkConfigImportButton.isEnabled = true
                if (text == null) {
                    Toast.makeText(
                        this,
                        R.string.launch_dxvk_config_import_error,
                        Toast.LENGTH_LONG
                    ).show()
                    return@runOnUiThread
                }

                val entries = parseDxvkConfig(text)
                if (entries.isEmpty()) {
                    Toast.makeText(
                        this,
                        R.string.launch_dxvk_config_import_empty,
                        Toast.LENGTH_LONG
                    ).show()
                    return@runOnUiThread
                }

                val importedCount = applyDxvkConfigEntries(entries)
                refreshValidation()
                saveIfValid()
                Toast.makeText(
                    this,
                    getString(R.string.launch_dxvk_config_import_done, importedCount),
                    Toast.LENGTH_SHORT
                ).show()
            }
        }.start()
    }

    private fun applyDxvkConfigEntries(entries: List<Pair<String, String>>): Int {
        /* The file wins over every matching row, including duplicate rows the
         * user may have created.  Keys that are not in the file keep their
         * current value; new keys are appended in file order. */
        val imported = LinkedHashMap<String, String>()
        entries.forEach { (key, value) -> imported[key] = value }
        val remaining = LinkedHashMap<String, String>()
        remaining.putAll(imported)

        for (index in 0 until dxvkConfigRows.childCount) {
            val row = dxvkConfigRows.getChildAt(index)
            val key = row.findViewById<EditText>(R.id.dxvk_config_key)
                .text.toString().trim()
            val value = imported[key] ?: continue
            row.findViewById<EditText>(R.id.dxvk_config_value).setText(value)
            remaining.remove(key)
        }

        remaining.forEach { (key, value) -> addDxvkConfigRow(key, value) }
        return imported.size
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
        /* The row under the driver buttons is gone, so the progress of the
         * install goes to a toast like the result. */
        Toast.makeText(this, R.string.launch_driver_installing, Toast.LENGTH_LONG)
            .show()
        Thread {
            val result = driverStore.install(uri)
            runOnUiThread {
                driverBusy = false
                if (isFinishing || isDestroyed) return@runOnUiThread
                when (result) {
                    is VulkanDriverStore.InstallResult.Ok -> {
                        drivers = driverStore.list()
                        rebuildDriverOptions(result.info.id)
                        updateDriverState()
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
                        updateDriverState()
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
                    updateDriverState()
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

    /*
     * The selected driver is already visible in the spinner, so this row
     * only tracks whether the delete button is available.
     */
    private fun updateDriverState() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return
        val id = currentDriverId()
        val info = drivers.firstOrNull { it.id == id }
        driverRemoveButton.isEnabled = info != null && !driverBusy
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
        dxvkScratchRow = null
        parseDxvkConfig(config).forEach { (key, value) ->
            addDxvkConfigRow(key, value)
        }
        ensureScratchRow()
    }

    private fun addDxvkConfigRow(
        key: String = "",
        value: String = ""
    ): AutoCompleteTextView {
        val row = createDxvkConfigRow(key, value, scratch = false)
        insertDxvkRow(row)
        return row.findViewById(R.id.dxvk_config_key)
    }

    /* The table keeps one empty row at the end, so a new variable can be typed
     * without pressing a button first. */
    private fun ensureScratchRow() {
        val scratch = dxvkScratchRow
        if (scratch != null && scratch.parent === dxvkConfigRows) return
        val row = createDxvkConfigRow("", "", scratch = true)
        dxvkScratchRow = row
        dxvkConfigRows.addView(row)
    }

    /* New rows go in front of the empty row, so the empty one stays last. */
    private fun insertDxvkRow(row: View) {
        val scratch = dxvkScratchRow
        if (scratch != null && scratch.parent === dxvkConfigRows) {
            dxvkConfigRows.addView(row, dxvkConfigRows.indexOfChild(scratch))
        } else {
            dxvkConfigRows.addView(row)
        }
    }

    private fun createDxvkConfigRow(
        key: String,
        value: String,
        scratch: Boolean
    ): View {
        val row = layoutInflater.inflate(R.layout.item_dxvk_config, dxvkConfigRows, false)
        val keyInput = row.findViewById<AutoCompleteTextView>(R.id.dxvk_config_key)
        val valueInput = row.findViewById<AutoCompleteTextView>(R.id.dxvk_config_value)
        keyInput.filters = arrayOf(
            dxvkKeyFilter,
            InputFilter.LengthFilter(DXVK_KEY_MAX_LENGTH)
        )
        valueInput.filters = arrayOf(
            dxvkValueFilter,
            InputFilter.LengthFilter(DXVK_VALUE_MAX_LENGTH)
        )

        /* The text is set before the adapters are attached: an attached
         * adapter filters on every text change and would open the suggestion
         * list while the table is filled from the saved settings. */
        keyInput.setText(key)
        valueInput.setText(value)

        /* The empty row is a placeholder for a new variable, not an example:
         * the hints would read like values that are already there. */
        if (scratch) {
            keyInput.hint = null
            valueInput.hint = null
        }

        val keyAdapter = DxvkSuggestionAdapter(this, DxvkConfigOptions.keys)
        keyInput.setAdapter(keyAdapter)
        keyInput.threshold = 0
        val valueAdapter = DxvkSuggestionAdapter(
            this,
            DxvkConfigOptions.valuesFor(key.trim())
        )
        valueInput.setAdapter(valueAdapter)
        valueInput.threshold = 0

        val saveOnBlur = View.OnFocusChangeListener { view, focused ->
            if (focused) {
                /* The list of keys or of the allowed values opens on a tap and
                 * is filtered by the beginning of the typed text. */
                (view as AutoCompleteTextView).performCompletion()
            } else if (!binding) {
                saveIfValid()
            }
        }
        keyInput.setOnFocusChangeListener(saveOnBlur)
        valueInput.setOnFocusChangeListener(saveOnBlur)

        /* The suggestion list opens on a tap and is filtered by the beginning
         * of the typed text.  Exactly one list per action: the focus path
         * covers a field that gains the focus, the touch path covers a tap on
         * a field that already has it.  Calling performCompletion() from both
         * paths showed the list twice on the same tap. */
        var focusedAtTouchDown = false
        val showSuggestions = View.OnTouchListener { view, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    pager.requestDisallowInterceptTouchEvent(true)
                    focusedAtTouchDown = view.hasFocus()
                }
                MotionEvent.ACTION_UP -> {
                    pager.requestDisallowInterceptTouchEvent(false)
                    if (focusedAtTouchDown) {
                        (view as AutoCompleteTextView).performCompletion()
                    }
                }
                MotionEvent.ACTION_CANCEL ->
                    pager.requestDisallowInterceptTouchEvent(false)
                else -> Unit
            }
            false
        }
        keyInput.setOnTouchListener(showSuggestions)
        valueInput.setOnTouchListener(showSuggestions)

        keyInput.setOnItemClickListener { _, _, _, _ ->
            updateDxvkValueOptions(row, keyInput.text.toString().trim())
        }

        keyInput.addTextChangedListener(object : TextWatcher {
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
            ) = Unit

            override fun afterTextChanged(text: Editable?) {
                val typed = keyInput.text.toString().trim()
                updateDxvkValueOptions(row, typed)
                maybeAddScratchRow(row, typed, valueInput.text.toString().trim())
            }
        })

        valueInput.addTextChangedListener(object : TextWatcher {
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
            ) = Unit

            override fun afterTextChanged(text: Editable?) {
                maybeAddScratchRow(
                    row,
                    keyInput.text.toString().trim(),
                    valueInput.text.toString().trim()
                )
            }
        })

        val removeButton = row.findViewById<Button>(R.id.dxvk_config_remove)
        removeButton.visibility = if (scratch) View.INVISIBLE else View.VISIBLE
        removeButton.setOnClickListener {
            dxvkConfigRows.removeView(row)
            if (!binding) saveIfValid()
        }
        return row
    }

    /* The empty row at the end of the table becomes a real row once both its
     * key and its value are set; a new empty row then takes its place. */
    private fun maybeAddScratchRow(row: View, key: String, value: String) {
        if (row !== dxvkScratchRow) return
        if (key.isEmpty() || value.isEmpty()) return
        dxvkScratchRow = null
        ensureScratchRow()
    }

    /* Only the options with a finite set of values offer a choice; the others
     * keep a free-form value that the user types. */
    private fun updateDxvkValueOptions(row: View, key: String) {
        val adapter = row.findViewById<AutoCompleteTextView>(R.id.dxvk_config_value)
            .adapter as? DxvkSuggestionAdapter ?: return
        adapter.setItems(DxvkConfigOptions.valuesFor(key))
    }

    private fun readDxvkConfig(): String {
        val lines = mutableListOf<String>()
        for (index in 0 until dxvkConfigRows.childCount) {
            val row = dxvkConfigRows.getChildAt(index)
            val key = row.findViewById<AutoCompleteTextView>(R.id.dxvk_config_key)
                .text.toString().trim()
            if (key.isEmpty() || !key.all(::isDxvkKeyChar)) continue

            /* InputFilter is not applied to text set programmatically from a
             * saved preference, so sanitize once more before serialization.
             * DXVK has no escape for a quote and treats CR as whitespace. */
            val value = row.findViewById<AutoCompleteTextView>(R.id.dxvk_config_value)
                .text.toString().trim()
                .filter { it != '"' && it != '\n' && it != '\r' }
            val encoded = if (value.any { it.isWhitespace() }) "\"$value\"" else value
            lines += "$key = $encoded"
        }
        return lines.joinToString("\n")
    }

    /* Parses the flat key=value form used by the settings table.  Comments and
     * section headers are skipped; the value ends at the first whitespace
     * outside quotes, and quote characters are removed, matching DXVK's value
     * syntax.  Only active (uncommented) assignments are imported: the stock
     * dxvk.conf is a fully commented template. */
    private fun parseDxvkConfig(config: String): List<Pair<String, String>> =
        config.lineSequence().mapNotNull { raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#") || line.startsWith("[")) {
                return@mapNotNull null
            }

            val equals = line.indexOf('=')
            if (equals <= 0) return@mapNotNull null

            val key = line.substring(0, equals).trim()
            if (key.isEmpty() || key.length > DXVK_KEY_MAX_LENGTH ||
                !key.all(::isDxvkKeyChar)
            ) {
                return@mapNotNull null
            }

            key to parseDxvkValue(line.substring(equals + 1))
        }.toList()

    private fun parseDxvkValue(raw: String): String {
        var index = 0
        while (index < raw.length && raw[index].isWhitespace()) index += 1

        val value = StringBuilder()
        var quoted = false
        while (index < raw.length) {
            val character = raw[index]
            if (!quoted && character.isWhitespace()) break
            if (character == '"') {
                quoted = !quoted
            } else {
                value.append(character)
            }
            index += 1
        }
        return value.toString().take(DXVK_VALUE_MAX_LENGTH)
    }

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
        val folderSettings = settings.copy(width = 0, height = 0)
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
        val resolution = currentResolutionOption()
            ?: resolutionOptions.lastOrNull()
            ?: ResolutionOption(0, 0, "")
        val workerCpus = selectedCpuCores()

        return LaunchSettings(
            mode = mode,
            gameDir = folderInput.text.toString().trim(),
            width = resolution.width,
            height = resolution.height,
            sound = soundSwitch.isChecked,
            driverId = currentDriverId(),
            dxvkVersion = currentDxvkVersion(),
            dxvkConfig = readDxvkConfig(),
            gestureMode = gestureModeOptions
                .getOrNull(gestureModeSpinner.selectedItemPosition)?.mode
                ?: GestureMode.DOUBLE_TAP,
            doubleTapTimeoutMs = progressToDelay(doubleTapDelaySlider.progress),
            longPressTimeoutMs = progressToDelay(longPressDelaySlider.progress),
            selectionCap = selectionCapValues[
                selectionCapSpinner.selectedItemPosition.coerceIn(
                    0,
                    selectionCapValues.lastIndex
                )
            ],
            hideCursor = hideCursorSwitch.isChecked,
            overlayOpacityPercent = overlayOpacityFromProgress(
                overlayOpacitySlider.progress
            ),
            parallelExecution = workerCpus.size >= 2,
            workerCpus = workerCpus,
            gridEnabled = resolution.grid,
            gridDarkenPercent =
                LaunchContract.MIN_GRID_DARKEN_PERCENT +
                gridDarkenSlider.progress.coerceIn(
                    0,
                    LaunchContract.MAX_GRID_DARKEN_PERCENT -
                    LaunchContract.MIN_GRID_DARKEN_PERCENT
                ),
            gridCycleSeconds =
                LaunchContract.MIN_GRID_CYCLE_SECONDS +
                gridCycleSlider.progress.coerceIn(
                    0,
                    LaunchContract.MAX_GRID_CYCLE_SECONDS -
                    LaunchContract.MIN_GRID_CYCLE_SECONDS
                ),
            presentMode = presentModeOptions.getOrNull(
                presentModeSpinner.selectedItemPosition
            )?.value ?: LaunchContract.DEFAULT_PRESENT_MODE,
            presentBuffers = presentBufferOptions.getOrNull(
                presentBuffersSpinner.selectedItemPosition
            )?.value?.toIntOrNull()
                ?: LaunchContract.DEFAULT_PRESENT_BUFFERS
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

    /*
     * The opacity slider is an index into the 10..50 percent range: the
     * layout sets max = 40 so one step is one percent.  The native controls
     * layer clamps the same range.
     */
    private fun bindOverlayOpacitySlider(percent: Int) {
        overlayOpacitySlider.max =
            LaunchContract.MAX_OVERLAY_OPACITY_PERCENT -
            LaunchContract.MIN_OVERLAY_OPACITY_PERCENT
        val progress = overlayOpacityToProgress(percent)
        overlayOpacitySlider.progress = progress
        overlayOpacityValue.text = getString(
            R.string.launch_overlay_opacity_value,
            overlayOpacityFromProgress(progress)
        )
    }

    private fun wireOverlayOpacitySlider() {
        overlayOpacitySlider.setOnSeekBarChangeListener(
            object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(
                    seekBar: SeekBar?,
                    progress: Int,
                    fromUser: Boolean
                ) {
                    overlayOpacityValue.text = getString(
                        R.string.launch_overlay_opacity_value,
                        overlayOpacityFromProgress(progress)
                    )
                    if (fromUser && !binding) {
                        refreshValidation()
                        saveIfValid()
                    }
                }

                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit

                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            }
        )
    }

    private fun overlayOpacityToProgress(percent: Int): Int =
        percent.coerceIn(
            LaunchContract.MIN_OVERLAY_OPACITY_PERCENT,
            LaunchContract.MAX_OVERLAY_OPACITY_PERCENT
        ) - LaunchContract.MIN_OVERLAY_OPACITY_PERCENT

    private fun overlayOpacityFromProgress(progress: Int): Int =
        LaunchContract.MIN_OVERLAY_OPACITY_PERCENT + progress.coerceIn(
            0,
            LaunchContract.MAX_OVERLAY_OPACITY_PERCENT -
            LaunchContract.MIN_OVERLAY_OPACITY_PERCENT
        )

    /*
     * The grid darken slider is the source 0..100 percent value; the native
     * layer truncates `percent * 255 / 100`, so 50 percent maps to alpha 127.
     */
    private fun bindGridDarkenSlider(percent: Int) {
        gridDarkenSlider.max = LaunchContract.MAX_GRID_DARKEN_PERCENT -
            LaunchContract.MIN_GRID_DARKEN_PERCENT
        val value = percent.coerceIn(
            LaunchContract.MIN_GRID_DARKEN_PERCENT,
            LaunchContract.MAX_GRID_DARKEN_PERCENT
        )
        gridDarkenSlider.progress =
            value - LaunchContract.MIN_GRID_DARKEN_PERCENT
        gridDarkenValue.text = getString(R.string.launch_grid_darken_value, value)
    }

    private fun wireGridDarkenSlider() {
        gridDarkenSlider.setOnSeekBarChangeListener(
            object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(
                    seekBar: SeekBar?,
                    progress: Int,
                    fromUser: Boolean
                ) {
                    val value = progress.coerceIn(
                        0,
                        LaunchContract.MAX_GRID_DARKEN_PERCENT -
                            LaunchContract.MIN_GRID_DARKEN_PERCENT
                    ) + LaunchContract.MIN_GRID_DARKEN_PERCENT
                    gridDarkenValue.text = getString(
                        R.string.launch_grid_darken_value,
                        value
                    )
                    if (fromUser && !binding) {
                        refreshValidation()
                        saveIfValid()
                    }
                }

                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit

                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            }
        )
    }

    private fun bindGridCycleSlider(seconds: Int) {
        gridCycleSlider.max = LaunchContract.MAX_GRID_CYCLE_SECONDS -
            LaunchContract.MIN_GRID_CYCLE_SECONDS
        val value = seconds.coerceIn(
            LaunchContract.MIN_GRID_CYCLE_SECONDS,
            LaunchContract.MAX_GRID_CYCLE_SECONDS
        )
        gridCycleSlider.progress =
            value - LaunchContract.MIN_GRID_CYCLE_SECONDS
        gridCycleValue.text = gridCycleText(value)
    }

    private fun wireGridCycleSlider() {
        gridCycleSlider.setOnSeekBarChangeListener(
            object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(
                    seekBar: SeekBar?,
                    progress: Int,
                    fromUser: Boolean
                ) {
                    val value = progress.coerceIn(
                        0,
                        LaunchContract.MAX_GRID_CYCLE_SECONDS -
                            LaunchContract.MIN_GRID_CYCLE_SECONDS
                    ) + LaunchContract.MIN_GRID_CYCLE_SECONDS
                    gridCycleValue.text = gridCycleText(value)
                    if (fromUser && !binding) {
                        refreshValidation()
                        saveIfValid()
                    }
                }

                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit

                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            }
        )
    }

    private fun gridCycleText(seconds: Int): String =
        if (seconds <= 0) {
            getString(R.string.launch_grid_cycle_off)
        } else {
            getString(R.string.launch_grid_cycle_value, seconds)
        }

    /* The darken and cycle sliders belong to the half-resolution option and
     * appear only while that resolution is selected. */
    private fun updateGridOptions() {
        val grid = currentResolutionOption()?.grid == true
        gridOptionsGroup.visibility = if (grid) View.VISIBLE else View.GONE
    }

    private fun currentResolutionOption(): ResolutionOption? =
        resolutionOptions.getOrNull(resolutionSpinner.selectedItemPosition)

    /* The slider is an index, not milliseconds: max = 15 maps 50..200 ms in
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
        val label: String,
        val grid: Boolean = false
    )

    private data class GestureModeOption(
        val mode: GestureMode,
        val label: String,
        val hint: String
    )

    private data class DriverOption(
        val id: String,
        val label: String
    )

    private data class ValueOption(
        val value: String,
        val label: String
    )

    /* The three pages are inflated once and reused by the pager, so every
     * control exists before the first layout pass and each page keeps its
     * scroll position while the user swipes. */
    private class SettingsPagerAdapter(
        inflater: LayoutInflater,
        pageLayouts: IntArray
    ) : PagerAdapter() {
        private val pages: List<View> = pageLayouts.map {
            inflater.inflate(it, null, false)
        }

        fun page(index: Int): View = pages[index]

        override fun getCount(): Int = pages.size

        override fun isViewFromObject(view: View, obj: Any): Boolean = view === obj

        override fun instantiateItem(container: ViewGroup, position: Int): Any {
            val page = pages[position]
            (page.parent as? ViewGroup)?.removeView(page)
            container.addView(page)
            return page
        }

        override fun destroyItem(container: ViewGroup, position: Int, obj: Any) {
            container.removeView(obj as View)
        }
    }

    private companion object {
        const val GAME_PAGE = 0
        const val VIDEO_PAGE = 1
        const val CONTROLS_PAGE = 2
        val PAGE_LAYOUTS = intArrayOf(
            R.layout.page_launch_game,
            R.layout.page_launch_video,
            R.layout.page_launch_controls
        )

        const val REQUEST_STORAGE = 4101
        const val MIN_RESOLUTION_HEIGHT = 360
        const val RESOLUTION_HEIGHT_STEP = 180
        const val DEFAULT_SCREEN_WIDTH = 1280
        const val DEFAULT_SCREEN_HEIGHT = 720
        const val MIN_GESTURE_DELAY_MS = 50
        const val MAX_GESTURE_DELAY_MS = 200
        const val GESTURE_DELAY_STEP_MS = 10
        const val GESTURE_DELAY_STEPS =
            (MAX_GESTURE_DELAY_MS - MIN_GESTURE_DELAY_MS) / GESTURE_DELAY_STEP_MS
        const val DXVK_KEY_MAX_LENGTH = 80
        const val DXVK_VALUE_MAX_LENGTH = 200
        const val MAX_DXVK_CONFIG_FILE_CHARS = 256 * 1024
        const val DXVK_VERSION_1_10 = "1"
        const val DXVK_VERSION_3_1_1 = "3"
    }
}
