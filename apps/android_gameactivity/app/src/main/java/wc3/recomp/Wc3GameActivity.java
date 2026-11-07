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
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;

import com.google.androidgamesdk.GameActivity;

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

    private static native void nativeSetGameMode(String mode);

    private native void nativeSetOverlaySurface(Surface surface, int width,
                                                int height);
    private native void nativeClearOverlaySurface(Surface surface);

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
        super.onCreate(savedInstanceState);
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

    private void installOverlaySurface() {
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
