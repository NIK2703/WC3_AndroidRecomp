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
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;

import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import wc3.recomp.launch.LaunchContract;

/**
 * Android host entry point.  GameActivity owns the game SurfaceView and native
 * window; this activity adds one transparent SurfaceView above it for the
 * touch-control overlay.
 *
 * <p>The overlay is a separate hardware surface: DXVK still owns the game
 * surface, while the native overlay renderer locks only the control surface.
 * The Java side passes the surface to native code and never parses the .icp
 * profile or injects input itself.</p>
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

    /* True once this process has started android_main.  The launch overrides
     * are process environment variables, so applying a second bundle while the
     * guest is already running would only desynchronize the intent from the
     * environment.  Settings are documented as applying on the next start. */
    private static boolean sNativeStarted;

    private static native void nativeSetGameMode(String mode);

    /* JNI matches by name only; host_shell.c must keep the same parameter
     * order (driver id before driver root, DXVK version and config path, then
     * the gesture flag and timers, and the SIMPLE tap delay last).  These
     * values are strings, so a swap compiles and fails only at runtime. */
    private static native void nativeApplyLaunchSettings(
            String gameDir, int width, int height, boolean sound,
            String extraArgs, String driverId, String driverRoot,
            String hookDir, String dxvkVersion, String dxvkConfigPath,
            boolean doubleTapGestures, int doubleTapTimeoutMs,
            int longPressTimeoutMs, int tapClickDelayMs);

    private native void nativeSetOverlaySurface(Surface surface, int width,
                                                int height);
    private native void nativeClearOverlaySurface(Surface surface);

    /* Current host-side IME translation in physical pixels.  The native input
     * path adds it back before hit-testing, so this getter must return exactly
     * the value that requestImePan() applies. */
    private native float nativeImePan();

    /* Backspace forwarding for the empty-editor case.  Returns false when no
     * guest text field is active, so the key keeps its normal GameActivity
     * handling. */
    private native boolean nativeImeBackspace(boolean down);

    /**
     * Applies the host-side IME pan on the UI thread.  Both full-screen
     * SurfaceViews move together: the game surface and the touch overlay.
     * Reading the value inside the runnable makes the newest pan win even when
     * a guest-thread show races an inset update from the UI thread.
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

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        if (!sNativeStarted) {
            applyLaunchSettings(getIntent());
        }
        super.onCreate(savedInstanceState);
        sNativeStarted = true;
        applyDisplayCutoutMode();
        ensureSharedDataAccess();

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

        installOverlaySurface();
    }

    /**
     * Applies only the environment variables the settings screen owns.  A
     * direct host launch without the launch bundle leaves the native defaults
     * and any diagnostic shell environment untouched.
     */
    private void applyLaunchSettings(final Intent intent) {
        if (intent == null) return;
        final Bundle settings = intent.getBundleExtra(
                LaunchContract.EXTRA_LAUNCH);
        if (settings == null) return;
        final String driverId = settings.getString(LaunchContract.KEY_DRIVER_ID, "");
        final String dxvkVersion = settings.getString(
                LaunchContract.KEY_DXVK_VERSION,
                LaunchContract.DEFAULT_DXVK_VERSION);
        final String dxvkConfig = settings.getString(
                LaunchContract.KEY_DXVK_CONFIG,
                LaunchContract.DEFAULT_DXVK_CONFIG);
        final String driverRoot = new java.io.File(getFilesDir(), "drivers")
                .getAbsolutePath();
        final String hookDir = driverId == null || driverId.isEmpty()
                ? "" : wc3.recomp.launch.VulkanDriverStore.ensureHookDir(this);
        final String dxvkConfigPath = writeDxvkConfig(dxvkConfig);
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
                        LaunchContract.DEFAULT_TAP_CLICK_DELAY_MS));
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
    private void ensureSharedDataAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        if (Environment.isExternalStorageManager()) return;

        // A game directory inside the app sandbox needs no All files access.
        // The settings screen accepts that fallback; do not interrupt the game
        // with a permission screen it does not need.
        final Intent launchIntent = getIntent();
        final Bundle launch = launchIntent == null
                ? null
                : launchIntent.getBundleExtra(LaunchContract.EXTRA_LAUNCH);
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
