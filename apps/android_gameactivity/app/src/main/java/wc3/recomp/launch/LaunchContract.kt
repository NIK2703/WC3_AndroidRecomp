package wc3.recomp.launch

/**
 * Intent protocol shared by the launcher/settings layer and the Java
 * GameActivity host.  These are protocol constants, not user-facing strings.
 */
object LaunchContract {
    const val ACTION_OPEN_SETTINGS = "wc3.recomp.action.OPEN_SETTINGS"

    const val EXTRA_MODE = "wc3.recomp.extra.MODE"
    const val EXTRA_LAUNCH = "wc3.recomp.extra.LAUNCH"

    const val KEY_GAME_DIR = "game_dir"
    const val KEY_WIDTH = "width"
    const val KEY_HEIGHT = "height"
    const val KEY_SOUND = "sound"
    /* Always empty in the product bundle.  Kept so a diagnostic launch can
     * still pass guest command-line arguments through the same key. */
    const val KEY_EXTRA_ARGS = "args"
    const val KEY_DRIVER_ID = "vulkan_driver_id"
    const val KEY_DXVK_VERSION = "dxvk_version"
    const val KEY_DXVK_CONFIG = "dxvk_config"
    const val KEY_DOUBLE_TAP_GESTURES = "double_tap_gestures"
    /* The third variant.  False keeps the authored overlay path; true also
     * requires the native guest sink, otherwise the controls layer falls back
     * to the legacy path for every touch. */
    const val KEY_DIRECT_TOUCH_ENABLED = "direct_touch_enabled"
    const val DEFAULT_DIRECT_TOUCH_ENABLED = false
    const val KEY_DOUBLE_TAP_TIMEOUT_MS = "double_tap_timeout_ms"
    const val KEY_LONG_PRESS_TIMEOUT_MS = "long_press_timeout_ms"
    const val KEY_TAP_CLICK_DELAY_MS = "tap_click_delay_ms"
    /* Unit-selection runtime cap: 12, 24 or 40.  The corpus is always built
     * with the 40-unit structure; this key only selects the cap and the panel
     * grid (6x2, 8x3 or 10x4).  The old boolean key "selection_24" is
     * migrated by LaunchSettingsStore. */
    const val KEY_SELECTION_CAP = "selection_cap"
    const val KEY_HIDE_CURSOR = "hide_cursor"
    const val KEY_OVERLAY_OPACITY_PERCENT = "overlay_opacity_percent"
    /* Always false in the product bundle.  Kept as a bundle key so the native
     * host still publishes WC3_PARALLEL_GAMEPLAY=0 to every diagnostic path. */
    const val KEY_PARALLEL_EXECUTION = "parallel_execution"
    /* Always empty: selecting a core never creates a worker pool. */
    const val KEY_WORKER_CPUS = "worker_cpus"
    /* The single core selected in the settings table.  -1 means no core was
     * selected; if the topology is hidden, the requested id is the fallback
     * pin. */
    const val KEY_MAIN_CPU = "main_cpu"
    const val KEY_GRID_ENABLED = "grid_enabled"
    const val KEY_GRID_DARKEN_PERCENT = "grid_darken_percent"
    const val KEY_GRID_CYCLE_SECONDS = "grid_cycle_seconds"
    const val KEY_PRESENT_SCALE = "present_scale"
    const val KEY_PRESENT_MODE = "present_mode"
    const val KEY_PRESENT_BUFFERS = "present_buffers"

    /* The authored .icp profile uses 120 ms for both gesture timers.  The
     * settings screen exposes only the product range 50..200 ms, step 10. */
    const val DEFAULT_DOUBLE_TAP_TIMEOUT_MS = 120
    const val DEFAULT_LONG_PRESS_TIMEOUT_MS = 120

    /* Wall-clock delay between a pointer move and the LMB press in the
     * simplified overlay.  There is no settings control and no per-launch
     * override: this constant is the single source for every launch bundle. */
    const val TAP_CLICK_DELAY_MS = 40

    /* 12 is the original product cap.  The settings dropdown offers 24 and
     * 40 as additional modes; the corpus itself is built with the 40-unit
     * structure, so switching the cap does not rebuild anything. */
    const val DEFAULT_SELECTION_CAP = 12
    const val SELECTION_CAP_12 = 12
    const val SELECTION_CAP_24 = 24
    const val SELECTION_CAP_40 = 40
    const val MIN_SELECTION_CAP = 12
    const val MAX_SELECTION_CAP = 40

    /** Clamp a persisted or intent-supplied cap to the three supported modes. */
    fun normalizeSelectionCap(value: Int): Int = when (value) {
        SELECTION_CAP_24, SELECTION_CAP_40 -> value
        else -> DEFAULT_SELECTION_CAP
    }

    /* The guest draws its own CCursorFrame sprite.  Off keeps the original
     * behavior; on skips that render without changing touch input. */
    const val DEFAULT_HIDE_CURSOR = false

    /* Opacity of the on-screen control overlay, in percent of the alpha
     * channel.  10 is nearly invisible, 50 is half solid; the value replaces
     * the authored per-element opacity of the .icp profile.  The native
     * controls layer clamps the same range. */
    const val DEFAULT_OVERLAY_OPACITY_PERCENT = 40
    const val MIN_OVERLAY_OPACITY_PERCENT = 10
    const val MAX_OVERLAY_OPACITY_PERCENT = 50

    /* Half-resolution grid presentation: one pixel of every 2x2 screen cell
     * stays untouched and the other three are dimmed by the darken percent.
     * Off by default; the native grid layer is created only when enabled, so
     * a disabled build composites the game surface exactly as before.  The
     * product default dims the three grid pixels to one third (33 percent). */
    const val DEFAULT_GRID_ENABLED = false
    const val DEFAULT_GRID_DARKEN_PERCENT = 33
    const val MIN_GRID_DARKEN_PERCENT = 0
    const val MAX_GRID_DARKEN_PERCENT = 100

    /* Seconds between phase rotations.  0 keeps one static phase.  The
     * special -1 means "auto": 60 s on an OLED panel and off on other
     * displays.  LaunchSettingsStore resolves it before the intent bundle is
     * built and the slider only writes 0..60. */
    const val DEFAULT_GRID_CYCLE_SECONDS = -1
    const val MIN_GRID_CYCLE_SECONDS = 0
    const val MAX_GRID_CYCLE_SECONDS = 60

    /* Presentation scaling is derived, not selected.  "system" keeps the guest
     * at the display's own mode and hands any lower in-game mode to the Android
     * compositor, which stretches the layer.  "half" renders the guest at half
     * the surface and upscales it 2x with nearest sampling - the grid path,
     * where every grid cell must be an exact 2x2 block of the guest image. */
    const val PRESENT_SCALE_SYSTEM = "system"
    const val PRESENT_SCALE_HALF = "half"

    /* The final screen swap chain, not DXVK's guest swap chain.  FIFO is the
     * default and the safe fallback.  The native pass checks the surface's
     * supported modes before creating the swap chain and substitutes FIFO for
     * an unsupported choice.  These are the four modes an ordinary Android
     * SurfaceView can use; shared-present modes are extension-only and are not
     * offered here. */
    const val PRESENT_MODE_FIFO = "fifo"
    const val PRESENT_MODE_FIFO_RELAXED = "fifo_relaxed"
    const val PRESENT_MODE_MAILBOX = "mailbox"
    const val PRESENT_MODE_IMMEDIATE = "immediate"
    val PRESENT_MODES = listOf(
        PRESENT_MODE_FIFO,
        PRESENT_MODE_FIFO_RELAXED,
        PRESENT_MODE_MAILBOX,
        PRESENT_MODE_IMMEDIATE
    )
    const val DEFAULT_PRESENT_MODE = PRESENT_MODE_FIFO

    /* Extra screen swap-chain images above the surface's reported
     * minImageCount.  0 keeps the previous behaviour (the minimum) and is the
     * product default; 1 and 2 add one or two images, clamped by
     * maxImageCount in the native pass.  This is not a request to force the
     * presentation engine to queue that many frames. */
    const val DEFAULT_PRESENT_BUFFERS = 0
    const val MIN_PRESENT_BUFFERS = 0
    const val MAX_PRESENT_BUFFERS = 2

    /* The product never enables parallel guest gameplay from the core table:
     * the chosen core runs the game sequentially (WC3_PARALLEL_GAMEPLAY=0).
     * The constant remains the default for a bundle without the key. */
    const val DEFAULT_PARALLEL_EXECUTION = false

    /* The product default is DXVK 1.10.3 ("1").  An empty version is the
     * explicit "Auto" choice and keeps the driver-based compatibility gate.
     * The low-latency 3.1.1 build ("ll") is opt-in. */
    const val DEFAULT_DXVK_VERSION = "1"
    const val DXVK_VERSION_LOW_LATENCY = "ll"

    /* DXVK defaults to an uncapped D3D9 swapchain.  The product starts with a
     * 60 FPS cap so the first launch is not a battery/thermal stress test; the
     * settings table can remove or override this row. */
    const val DEFAULT_DXVK_CONFIG = "d3d9.maxFrameRate = 60"
}
