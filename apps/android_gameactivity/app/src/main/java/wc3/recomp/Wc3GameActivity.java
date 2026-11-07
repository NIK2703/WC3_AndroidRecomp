package wc3.recomp;

import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;

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
public final class Wc3GameActivity extends GameActivity {
    private SurfaceView overlayView;

    private native void nativeSetOverlaySurface(Surface surface, int width,
                                                int height);
    private native void nativeClearOverlaySurface(Surface surface);

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
        installOverlaySurface();
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
