package wc3.recomp;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import wc3.recomp.launch.DisplayCapabilities;
import wc3.recomp.launch.GameMode;
import wc3.recomp.launch.LaunchContract;
import wc3.recomp.launch.LaunchCoordinator;
import wc3.recomp.launch.LaunchSettingsStore;

/**
 * Android host entry point.  GameActivity owns the game SurfaceView and native
 * window; this activity adds transparent SurfaceViews above it: one for the
 * optional half-resolution grid and one for the touch-control overlay.
 *
 * <p>Both overlays are separate hardware surfaces: DXVK still owns the game
 * surface, the native grid layer fills only its own buffer, and the native
 * overlay renderer locks only the control surface.  The Java side passes the
 * surfaces to native code and never parses the .icp profile or injects input
 * itself.</p>
 */
public class Wc3GameActivity extends GameActivity {
    static {
        // Load before GameActivity.onCreate starts android_main.  The
        // mode-specific activities call nativeSetGameMode() in their own
        // onCreate, before super.onCreate(), so the native boot path sees the
        // selected game before it builds the guest command line.
        System.loadLibrary("wc3gameactivity");
    }

    private SurfaceView overlayView;
    private SurfaceView gridView;
    private boolean gridRuntimeDisabled;
    private MoviePlayer moviePlayer;

    /* True once this process has started android_main.  The launch overrides
     * are process environment variables, so applying a second bundle while the
     * guest is already running would only desynchronize the intent from the
     * environment.  Settings are documented as applying on the next start. */
    private static boolean sNativeStarted;

    private static native void nativeSetGameMode(String mode);

    /* JNI matches by name only; host_shell.c must keep the same parameter
     * order (driver id before driver root, DXVK version and config path, then
     * the gesture flag and timers, the SIMPLE tap delay, the selection cap
     * (12/24/40), the hideCursor toggle, the derived parallelExecution flag, the
     * overlay opacity percent, the grid toggle, darken percent and cycle
     * seconds, the presentation scale string, the screen present mode string
     * and buffering count, the comma-separated worker CPU list, and finally
     * the main-thread CPU id).  Several values are strings, so a swap compiles
     * and fails only at runtime. */
    private static native void nativeApplyLaunchSettings(
            String gameDir, int width, int height, boolean sound,
            String extraArgs, String driverId, String driverRoot,
            String hookDir, String dxvkVersion, String dxvkConfigPath,
            boolean doubleTapGestures, int doubleTapTimeoutMs,
            int longPressTimeoutMs, int tapClickDelayMs,
            int selectionCap, boolean hideCursor, boolean parallelExecution,
            int overlayOpacityPercent, boolean gridEnabled,
            int gridDarkenPercent, int gridCycleSeconds, String presentScale,
            String presentMode, int presentBuffers, String workerCpus,
            int mainCpu);

    private native void nativeSetOverlaySurface(Surface surface, int width,
                                                int height);
    private native void nativeClearOverlaySurface(Surface surface);

    /*
     * Half-resolution grid layer.  The activity only owns the SurfaceView
     * lifecycle; the native module rasterises the pattern once per phase and
     * SurfaceFlinger composites the buffer, so no Java code runs while the
     * game presents frames.
     */
    private native void nativeSetGridSurface(Surface surface, int width,
                                             int height);
    private native void nativeClearGridSurface(Surface surface);
    private native void nativeSetGridActive(boolean active);

    private static native void nativeMovieFinished(int status);

    /* Current host-side IME translation in physical pixels.  The native input
     * path adds it back before hit-testing, so this getter must return exactly
     * the value that requestImePan() applies. */
    private native float nativeImePan();

    /* Backspace forwarding for the empty-editor case.  Returns false when no
     * guest text field is active, so the key keeps its normal GameActivity
     * handling. */
    private native boolean nativeImeBackspace(boolean down);

    /**
     * Applies the host-side IME pan on the UI thread.  All full-screen
     * SurfaceViews move together: the game surface, the grid layer and the
     * touch overlay.  Reading the value inside the runnable makes the newest
     * pan win even when a guest-thread show races an inset update from the UI
     * thread.
     */
    public void requestImePan() {
        runOnUiThread(() -> {
            final float pan = nativeImePan();
            final float offset = pan > 0.0f ? -pan : 0.0f;
            if (mSurfaceView != null) {
                mSurfaceView.setTranslationY(offset);
            }
            if (overlayView != null) {
                overlayView.setTranslationY(offset);
            }
            if (gridView != null) {
                gridView.setTranslationY(offset);
            }
        });
    }

    /*
     * The GameActivity input connection consumes Backspace while its editor
     * has text and reports the edit through the text-state callback.  When the
     * editor is empty the key reaches the activity instead; native code then
     * forwards it to the focused guest field.  The matching key-up is consumed
     * so it is not delivered as an independent game key.
     */
    @Override
    public boolean onKeyDown(final int keyCode, final KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_DEL && nativeImeBackspace(true)) {
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(final int keyCode, final KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_DEL && nativeImeBackspace(false)) {
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    protected static void setGameMode(final String mode) {
        nativeSetGameMode(mode);
    }

    /**
     * Called from android_main through JNI after platform/memory/PE checks.
     * The text is already written to the native run log; rendering a second
     * in-game panel would cover the game surface, so this hook is intentionally
     * silent in the product activity.
     */
    public static void setDiagnostics(final String text) {
    }

    protected GameMode hostGameMode() {
        return GameMode.TFT;
    }

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        final Bundle launch = resolveLaunchBundle(getIntent());
        if (!sNativeStarted) {
            applyLaunchSettings(launch);
        }
        super.onCreate(savedInstanceState);
        sNativeStarted = true;
        applyDisplayCutoutMode();
        applyImmersiveMode();
        ensureSharedDataAccess(launch);

        // Visible-password commits every key instead of leaving a composing
        // word in the IME strip.  The guest owns the real field and receives
        // only forward/backspace edits through the native text bridge.
        setImeEditorInfoFields(
                InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
                EditorInfo.IME_ACTION_DONE,
                EditorInfo.IME_ACTION_DONE
                        | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                        | EditorInfo.IME_FLAG_NO_FULLSCREEN);

        installGridSurface(launch);
        installOverlaySurface();
        installMoviePlayer();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersiveMode();
    }

    private void applyImmersiveMode() {
        final Window window = getWindow();
        WindowCompat.setDecorFitsSystemWindows(window, false);
        final WindowInsetsControllerCompat controller =
                WindowCompat.getInsetsController(window, window.getDecorView());
        controller.hide(WindowInsetsCompat.Type.systemBars());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /**
     * The grid cycle is meaningful only while the grid is visible.  A native
     * Surface producer that keeps posting to a background window can fill the
     * BufferQueue; stopping the worker here keeps the UI-thread join bounded.
     */
    @Override
    protected void onStart() {
        super.onStart();
        if (gridView != null && !gridRuntimeDisabled) nativeSetGridActive(true);
    }

    @Override
    protected void onStop() {
        if (gridView != null) nativeSetGridActive(false);
        super.onStop();
    }

    /**
     * Called through JNI from the guest thread when the game's own Video options
     * changed the resolution while the half-resolution grid was active.  Native
     * has already retired the half present target and switched the new mode to
     * the normal compositor path, so this hides the mask and persists the
     * option off for the next launch.  The actual view and preference work runs
     * on the UI thread.
     */
    public void onGridRuntimeDisabled(int width, int height) {
        runOnUiThread(() -> {
            gridRuntimeDisabled = true;
            if (gridView != null) {
                nativeSetGridActive(false);
                gridView.setVisibility(View.GONE);
            }
            new LaunchSettingsStore(this).disableHalfGrid(
                    hostGameMode(), width, height);
        });
    }

    /**
     * Called through JNI from the ole32/DirectShow facade.  The method is
     * public because JNI lookup uses the activity class; it is not part of the
     * guest ABI.
     */
    public boolean playMovie(final String path) {
        final MoviePlayer player = moviePlayer;
        if (player == null || path == null || path.isEmpty()) return false;
        player.play(path);
        return true;
    }

    public void stopMovie() {
        final MoviePlayer player = moviePlayer;
        if (player != null) player.stop();
    }

    private void installMoviePlayer() {
        moviePlayer = new MoviePlayer(this, new MoviePlayer.Host() {
            @Override
            public void setOverlayVisible(final boolean visible) {
                if (overlayView == null) return;
                overlayView.setVisibility(visible
                        ? View.VISIBLE : View.INVISIBLE);
            }

            @Override
            public void onFinished(final int status) {
                nativeMovieFinished(status);
            }
        });
    }

    private Bundle resolveLaunchBundle(final Intent intent) {
        if (intent != null) {
            final Bundle explicit = intent.getBundleExtra(
                    LaunchContract.EXTRA_LAUNCH);
            if (explicit != null) return explicit;
        }
        return LaunchCoordinator.resolveForHost(this, hostGameMode());
    }

    /**
     * Applies only the environment variables the settings screen owns.  The
     * explicit launch bundle wins; a direct host launch gets the saved values
     * from LaunchCoordinator.resolveForHost, so the CPU selection is applied
     * on that path too.  A null bundle is the only case that leaves the native
     * defaults and any diagnostic shell environment untouched.
     */
    private void applyLaunchSettings(final Bundle settings) {
        if (settings == null) return;
        final String driverId = settings.getString(LaunchContract.KEY_DRIVER_ID, "");
        final String dxvkVersion = settings.getString(
                LaunchContract.KEY_DXVK_VERSION,
                LaunchContract.DEFAULT_DXVK_VERSION);
        final String dxvkConfig = settings.getString(
                LaunchContract.KEY_DXVK_CONFIG,
                LaunchContract.DEFAULT_DXVK_CONFIG);
        /* A negative preference is the "auto" OLED default; resolve it once
         * here so the native grid layer only sees a concrete interval. */
        final int gridCycleSeconds = DisplayCapabilities.gridCycleSeconds(
                this,
                settings.getInt(
                        LaunchContract.KEY_GRID_CYCLE_SECONDS,
                        LaunchContract.DEFAULT_GRID_CYCLE_SECONDS));
        final String driverRoot = new java.io.File(getFilesDir(), "drivers")
                .getAbsolutePath();
        final String hookDir = driverId == null || driverId.isEmpty()
                ? "" : wc3.recomp.launch.VulkanDriverStore.ensureHookDir(this);
        final String dxvkConfigPath = writeDxvkConfig(dxvkConfig);
        final boolean gridEnabled = settings.getBoolean(
                LaunchContract.KEY_GRID_ENABLED,
                LaunchContract.DEFAULT_GRID_ENABLED);
        /* Scaling is derived, never chosen: the grid masks the exact 2x source,
         * so it implies the half-surface nearest path; without the grid the
         * compositor gets the guest mode.  Deriving it here keeps every launch
         * path, including old shortcuts, on the same rule. */
        final String presentScale = gridEnabled
                ? LaunchContract.PRESENT_SCALE_HALF
                : LaunchContract.PRESENT_SCALE_SYSTEM;
        String presentMode = settings.getString(
                LaunchContract.KEY_PRESENT_MODE,
                LaunchContract.DEFAULT_PRESENT_MODE);
        if (presentMode == null || presentMode.isEmpty()) {
            presentMode = LaunchContract.DEFAULT_PRESENT_MODE;
        }
        final int presentBuffers = Math.max(
                LaunchContract.MIN_PRESENT_BUFFERS,
                Math.min(
                        LaunchContract.MAX_PRESENT_BUFFERS,
                        settings.getInt(
                                LaunchContract.KEY_PRESENT_BUFFERS,
                                LaunchContract.DEFAULT_PRESENT_BUFFERS)));
        nativeApplyLaunchSettings(
                settings.getString(LaunchContract.KEY_GAME_DIR),
                settings.getInt(LaunchContract.KEY_WIDTH, 0),
                settings.getInt(LaunchContract.KEY_HEIGHT, 0),
                settings.getBoolean(LaunchContract.KEY_SOUND, true),
                settings.getString(LaunchContract.KEY_EXTRA_ARGS),
                driverId, driverRoot, hookDir, dxvkVersion, dxvkConfigPath,
                settings.getBoolean(
                        LaunchContract.KEY_DOUBLE_TAP_GESTURES, true),
                settings.getInt(
                        LaunchContract.KEY_DOUBLE_TAP_TIMEOUT_MS,
                        LaunchContract.DEFAULT_DOUBLE_TAP_TIMEOUT_MS),
                settings.getInt(
                        LaunchContract.KEY_LONG_PRESS_TIMEOUT_MS,
                        LaunchContract.DEFAULT_LONG_PRESS_TIMEOUT_MS),
                settings.getInt(
                        LaunchContract.KEY_TAP_CLICK_DELAY_MS,
                        LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS),
                settings.getInt(
                        LaunchContract.KEY_SELECTION_CAP,
                        LaunchContract.DEFAULT_SELECTION_CAP),
                settings.getBoolean(
                        LaunchContract.KEY_HIDE_CURSOR,
                        LaunchContract.DEFAULT_HIDE_CURSOR),
                settings.getBoolean(
                        LaunchContract.KEY_PARALLEL_EXECUTION,
                        LaunchContract.DEFAULT_PARALLEL_EXECUTION),
                settings.getInt(
                        LaunchContract.KEY_OVERLAY_OPACITY_PERCENT,
                        LaunchContract.DEFAULT_OVERLAY_OPACITY_PERCENT),
                gridEnabled,
                settings.getInt(
                        LaunchContract.KEY_GRID_DARKEN_PERCENT,
                        LaunchContract.DEFAULT_GRID_DARKEN_PERCENT),
                gridCycleSeconds,
                presentScale,
                presentMode,
                presentBuffers,
                settings.getString(LaunchContract.KEY_WORKER_CPUS, ""),
                settings.getInt(LaunchContract.KEY_MAIN_CPU, -1));
    }

    /**
     * DXVK 1.10.3 only reads {@code DXVK_CONFIG_FILE}; 3.1.1 additionally
     * accepts the shorter {@code DXVK_CONFIG} environment form.  Writing the
     * same file gives both backends one contract and avoids the semicolon
     * escaping rules of the environment form.
     *
     * <p>The empty string is written as an empty file on purpose: it prevents
     * DXVK from falling back to an unrelated {@code dxvk.conf} in the current
     * working directory.  The temporary file and rename keep a failed write
     * from leaving half a config for the loader.</p>
     */
    private String writeDxvkConfig(final String config) {
        final File target = new File(getFilesDir(), "dxvk.conf");
        final File temporary = new File(getFilesDir(), "dxvk.conf.tmp");
        final String text = config == null ? "" : config;

        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        } catch (IOException ignored) {
            temporary.delete();
            return "";
        }

        if (target.exists() && !target.delete()) {
            temporary.delete();
            return "";
        }
        if (!temporary.renameTo(target)) {
            temporary.delete();
            return "";
        }
        return target.getAbsolutePath();
    }

    @Override
    protected void onDestroy() {
        if (moviePlayer != null) moviePlayer.stop();
        /* The source cancels the grid cycle callback in onDestroy.  The native
         * side owns the cycle worker, so stop it here as well; the later
         * surfaceDestroyed callback then only clears the surface and never
         * leaves a worker running against a torn-down window. */
        if (gridView != null) nativeClearGridSurface(null);
        if (isFinishing()) {
            sNativeStarted = false;
        }
        super.onDestroy();
    }

    /**
     * A landscape game should use the whole panel, including the notch strip.
     * Without short-edge cutout mode Android shrinks the window by the cutout
     * inset, which leaves a black band beside the game surface.  The window
     * theme sets the same mode before the first layout; this call also covers
     * GameActivity versions that replace the window attributes in onCreate.
     */
    private void applyDisplayCutoutMode() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return;
        final WindowManager.LayoutParams attrs = getWindow().getAttributes();
        attrs.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams
                        .LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(attrs);
    }

    /**
     * The native host reads the licensed game files with POSIX calls from the
     * shared {@code Warcraft 3} directory, so scoped-storage media permissions
     * are not enough on API 30+.  The user grants All files access once; the
     * native side falls back to the package directory until then.
     */
    private void ensureSharedDataAccess(final Bundle launch) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        if (Environment.isExternalStorageManager()) return;

        // A game directory inside the app sandbox needs no All files access.
        // The settings screen accepts that fallback; do not interrupt the game
        // with a permission screen it does not need.
        if (launch != null && isAppPrivatePath(
                launch.getString(LaunchContract.KEY_GAME_DIR))) {
            return;
        }

        final Intent appIntent = new Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        try {
            startActivity(appIntent);
        } catch (Exception ignored) {
            startActivity(new Intent(
                    Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private boolean isAppPrivatePath(final String path) {
        if (path == null || path.isEmpty()) return false;
        final String candidate;
        try {
            candidate = new File(path).getCanonicalPath();
        } catch (Exception ignored) {
            return false;
        }
        final File[] roots = {
                getDataDir(),
                getFilesDir(),
                getCacheDir(),
                getExternalFilesDir(null),
                getExternalCacheDir()
        };
        for (final File root : roots) {
            if (root == null) continue;
            final String rootPath;
            try {
                rootPath = root.getCanonicalPath();
            } catch (Exception ignored) {
                continue;
            }
            if (candidate.equals(rootPath)
                    || candidate.startsWith(rootPath + File.separator)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Debug-only T2 A/B.  The release resource is false, so this method is a
     * no-op in the product; DebugOverlayReceiver only exists in the debug APK.
     */
    private boolean isOverlayExperimentHidden() {
        if (!getResources().getBoolean(R.bool.overlay_experiment_supported))
            return false;
        return getSharedPreferences("wc3_debug", MODE_PRIVATE)
                .getBoolean("overlay_hidden", false);
    }

    /**
     * Adds the half-resolution grid below the touch overlay.  The layer has no
     * Java draw path: the native grid module rasterises the 2x2 pattern into
     * this SurfaceView buffer once per phase, and only the OLED cycle posts a
     * new buffer.  With the switch off the view is never created, so the
     * compositor has one layer less.
     */
    private void installGridSurface(final Bundle launch) {
        if (launch == null) return;
        if (!launch.getBoolean(LaunchContract.KEY_GRID_ENABLED,
                LaunchContract.DEFAULT_GRID_ENABLED)) return;
        final int darken = launch.getInt(
                LaunchContract.KEY_GRID_DARKEN_PERCENT,
                LaunchContract.DEFAULT_GRID_DARKEN_PERCENT);
        if (darken <= 0) return;

        final SurfaceView view = new SurfaceView(this);
        view.setZOrderMediaOverlay(true);
        view.setFocusable(false);
        view.setFocusableInTouchMode(false);
        view.setClickable(false);
        view.setBackgroundColor(Color.TRANSPARENT);
        view.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(final SurfaceHolder holder) {
                nativeSetGridSurface(holder.getSurface(), 0, 0);
            }

            @Override
            public void surfaceChanged(final SurfaceHolder holder, final int format,
                                       final int width, final int height) {
                nativeSetGridSurface(holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(final SurfaceHolder holder) {
                nativeClearGridSurface(holder.getSurface());
            }
        });
        addContentView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        gridView = view;
    }

    private void installOverlaySurface() {
        if (isOverlayExperimentHidden()) return;
        final SurfaceView view = new SurfaceView(this);
        view.setZOrderMediaOverlay(true);
        view.setFocusable(false);
        view.setFocusableInTouchMode(false);
        view.setClickable(false);
        view.setBackgroundColor(Color.TRANSPARENT);
        view.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(final SurfaceHolder holder) {
                nativeSetOverlaySurface(holder.getSurface(), 0, 0);
            }

            @Override
            public void surfaceChanged(final SurfaceHolder holder, final int format,
                                       final int width, final int height) {
                nativeSetOverlaySurface(holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(final SurfaceHolder holder) {
                nativeClearOverlaySurface(holder.getSurface());
            }
        });
        addContentView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        overlayView = view;
    }
}
