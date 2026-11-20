package wc3.recomp.launch

import android.os.Bundle

/**
 * The launch overrides owned by the product settings screen.  An empty game
 * directory means "use the native auto-detection"; zero width/height means
 * "keep the physical surface size".
 */
data class LaunchSettings(
    val mode: GameMode,
    val gameDir: String = "",
    val width: Int = 0,
    val height: Int = 0,
    val sound: Boolean = true,
    val extraArgs: String = "",
    /* Empty means the system Vulkan loader.  A non-empty value is a 16-hex
     * ADPkg id under filesDir/drivers (ADR-036). */
    val driverId: String = "",
    /* "1" forces DXVK 1.10.3 and is the product default; "3" forces DXVK
     * 3.1.1.  Empty is the explicit "Auto" choice and keeps the driver-based
     * compatibility gate.  A forced backend bypasses the gate and is not
     * replaced by the other version on failure. */
    val dxvkVersion: String = LaunchContract.DEFAULT_DXVK_VERSION,
    /* Key-value DXVK options in dxvk.conf syntax.  The host writes this text
     * to an app-private file and points DXVK_CONFIG_FILE at it before
     * android_main; both 1.10.3 and 3.1.1 read that contract.  The default is
     * the 60 FPS cap; an explicitly empty string means "no DXVK overrides". */
    val dxvkConfig: String = LaunchContract.DEFAULT_DXVK_CONFIG,
    /* One of the three product schemes: authored double tap, single tap with
     * no double-tap wait, or semantic direct touch.  The enum is the single
     * discriminator; the bundle publishes the legacy overlay boolean and the
     * direct-touch flag from it. */
    val gestureMode: GestureMode = GestureMode.DOUBLE_TAP,
    /* Gesture timers in milliseconds.  The settings UI clamps them to
     * 50..200 in 10 ms steps; the native product helper repeats the clamp. */
    val doubleTapTimeoutMs: Int = LaunchContract.DEFAULT_DOUBLE_TAP_TIMEOUT_MS,
    val longPressTimeoutMs: Int = LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS,
    /* Wall-clock delay before the LMB press in the SIMPLE overlay.  The
     * native helper clamps it to 8..40 ms; 10 ms is the product default. */
    val tapClickDelayMs: Int = LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS,
    /* Unit-selection runtime cap: 12 (the original product default), 24 or
     * 40.  The 6x2 panel is used up to 12 units, 8x3 for 13..24 and 10x4 for
     * 25..40.  The corpus always carries the 40-unit structure; the cap is a
     * launch-time choice.  Applied on the next game launch, not to an already
     * running host. */
    val selectionCap: Int = LaunchContract.DEFAULT_SELECTION_CAP,
    /* True suppresses CCursorFrame::Render in the guest.  Touch input still
     * works; only the game-drawn cursor sprite is skipped.  Applied on the
     * next game launch. */
    val hideCursor: Boolean = LaunchContract.DEFAULT_HIDE_CURSOR,
    /* Opacity of the on-screen control overlay in percent.  The settings UI
     * exposes 10..50; the native controls layer clamps the same range and
     * replaces the authored per-element opacity. */
    val overlayOpacityPercent: Int = LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT,
    /* Derived from workerCpus: two or more selected cores enable the
     * deterministic chunked raycast/vertex/triangle paths, one core keeps
     * every wrapper on the original sequential guest function.  There is no
     * separate switch in the product settings screen.  Applied on the next
     * game launch. */
    val parallelExecution: Boolean = LaunchContract.DEFAULT_PARALLEL_EXECUTION,
    /* CPU ids selected for the parallel worker pool.  The pool creates one
     * worker per id and pins that worker to the same core.  Empty disables
     * the pool; the product settings screen always keeps at least one core
     * selected.  Applied on the next game launch. */
    val workerCpus: List<Int> = emptyList(),
    /* Half-resolution grid presentation.  Off creates no extra SurfaceView
     * and no native renderer.  The darken percent follows the source slider
     * (0..100); the cycle moves the transparent pixel of every 2x2 cell and
     * is resolved from -1 "auto" to a concrete interval before launch. */
    val gridEnabled: Boolean = LaunchContract.DEFAULT_GRID_ENABLED,
    val gridDarkenPercent: Int = LaunchContract.DEFAULT_GRID_DARKEN_PERCENT,
    val gridCycleSeconds: Int = LaunchContract.DEFAULT_GRID_CYCLE_SECONDS,
    /* The final screen swap chain.  FIFO is the product default; the swap-chain
     * wrapper applies these values to the final real-window swap chain,
     * including the native direct path, and falls back to FIFO when a mode is
     * unsupported.  Buffering is the number of extra images above the surface
     * minimum (0..2). */
    val presentMode: String = LaunchContract.DEFAULT_PRESENT_MODE,
    val presentBuffers: Int = LaunchContract.DEFAULT_PRESENT_BUFFERS,
    /* Presentation scaling is not a setting.  The grid needs the exact-2x
     * half-surface path; every other case stays on the compositor. */
) {
    fun toBundle(): Bundle = Bundle().apply {
        /* The core table is the only parallel control: two or more selected
         * cores enable the pool, one core keeps every guest path sequential.
         * Normalise here as well as in the settings screen so every launch
         * path, including a caller that constructs LaunchSettings directly,
         * gets the same rule. */
        val requestedCpus = workerCpus
            .filter { it in 0..CpuTopology.MAX_CPU_ID }
            .distinct()
            .sorted()
        /* The settings store and the core table already drop CPUs outside the
         * process cpuset.  Repeat it here because a caller can construct
         * LaunchSettings directly or the cpuset can change between the
         * settings screen and the launch.  A saved core that is no longer
         * allowed is dropped: with no cores left the pool stays off and the
         * main thread still pins to the best available core. */
        val availableCores: List<CpuCore> = if (requestedCpus.isEmpty()) {
            emptyList()
        } else {
            CpuTopology.availableCores()
        }
        val selectedCores = availableCores.filter { it.id in requestedCpus }
        /* The native pool runs the active prefix of this list and keeps the
         * last slot outside the active set for the caller's last chunk.  Order
         * the pool from the most to the least performant core so the prefix
         * contains the fastest cores; appending the caller's core last then
         * leaves the caller on the best core and the critical path on the
         * slowest *active* core, not on the slowest selected core.  The order
         * is topology data, not a predictive cost model. */
        val useCapacity = selectedCores.all { (it.capacity ?: 0) > 0 }
        val performanceOrder = if (useCapacity) {
            compareByDescending<CpuCore> { it.capacity ?: 0 }
                .thenByDescending { it.maxFrequencyMhz ?: -1 }
                .thenBy { it.id }
        } else {
            compareByDescending<CpuCore> { it.maxFrequencyMhz ?: -1 }
                .thenBy { it.id }
        }
        val orderedCores = selectedCores.sortedWith(performanceOrder)
        /* The game's main thread always runs on the most performant selected
         * core.  Capacity/frequency come from sysfs; if topology is hidden,
         * the highest selected id is the big-core fallback.  A stale selected
         * id that the game process can no longer use falls back to the best
         * currently available core instead of leaving the main thread
         * unpinned. */
        val mainCpu = if (requestedCpus.isEmpty()) {
            -1
        } else {
            CpuTopology.pickBest(selectedCores)?.id
                ?: CpuTopology.pickBest(availableCores)?.id
                ?: requestedCpus.last()
        }
        val launchCores = buildList {
            for (core in orderedCores) {
                if (core.id != mainCpu) add(core)
            }
            if (mainCpu >= 0 && selectedCores.any { it.id == mainCpu }) {
                add(orderedCores.first { it.id == mainCpu })
            }
        }
        val launchCpus = launchCores.map { it.id }
        val effectiveParallel = launchCpus.size >= 2
        putString(LaunchContract.KEY_GAME_DIR, gameDir)
        putInt(LaunchContract.KEY_WIDTH, width)
        putInt(LaunchContract.KEY_HEIGHT, height)
        putBoolean(LaunchContract.KEY_SOUND, sound)
        putString(LaunchContract.KEY_EXTRA_ARGS, extraArgs)
        putString(LaunchContract.KEY_DRIVER_ID, driverId)
        putString(LaunchContract.KEY_DXVK_VERSION, dxvkVersion)
        putString(LaunchContract.KEY_DXVK_CONFIG, dxvkConfig)
        putBoolean(
            LaunchContract.KEY_DOUBLE_TAP_GESTURES,
            gestureMode.doubleTapGestures
        )
        putBoolean(
            LaunchContract.KEY_DIRECT_TOUCH_ENABLED,
            gestureMode.directTouch
        )
        putInt(LaunchContract.KEY_DOUBLE_TAP_TIMEOUT_MS, doubleTapTimeoutMs)
        putInt(LaunchContract.KEY_LONG_PRESS_TIMEOUT_MS, longPressTimeoutMs)
        putInt(LaunchContract.KEY_TAP_CLICK_DELAY_MS, tapClickDelayMs)
        putInt(
            LaunchContract.KEY_SELECTION_CAP,
            LaunchContract.normalizeSelectionCap(selectionCap)
        )
        putBoolean(LaunchContract.KEY_HIDE_CURSOR, hideCursor)
        putInt(LaunchContract.KEY_OVERLAY_OPACITY_PERCENT, overlayOpacityPercent)
        putBoolean(LaunchContract.KEY_PARALLEL_EXECUTION, effectiveParallel)
        /* One core is the sequential product path: publish no worker list,
         * so the native side cannot mistake the caller's core for a pool. */
        putString(
            LaunchContract.KEY_WORKER_CPUS,
            if (effectiveParallel) launchCpus.joinToString(",") else ""
        )
        putInt(LaunchContract.KEY_MAIN_CPU, mainCpu)
        putBoolean(LaunchContract.KEY_GRID_ENABLED, gridEnabled)
        putInt(LaunchContract.KEY_GRID_DARKEN_PERCENT, gridDarkenPercent)
        putInt(LaunchContract.KEY_GRID_CYCLE_SECONDS, gridCycleSeconds)
        putString(LaunchContract.KEY_PRESENT_MODE, presentMode)
        putInt(LaunchContract.KEY_PRESENT_BUFFERS, presentBuffers)
        putString(
            LaunchContract.KEY_PRESENT_SCALE,
            if (gridEnabled) {
                LaunchContract.PRESENT_SCALE_HALF
            } else {
                LaunchContract.PRESENT_SCALE_SYSTEM
            }
        )
    }
}
