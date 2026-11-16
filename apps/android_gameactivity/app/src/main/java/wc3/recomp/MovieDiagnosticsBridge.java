package wc3.recomp;

import android.app.Activity;
import android.view.SurfaceView;

import androidx.media3.exoplayer.ExoPlayer;

/**
 * Release-safe entry point for the debug-only movie trace.
 *
 * <p>{@link MoviePlayer} must compile for the release variant, where
 * {@code MovieDiagnostics} does not exist.  This bridge therefore keeps the
 * reflection string and the no-op fallback in main, while every log statement
 * and every Media3 listener stays in {@code src/debug/java}.  The release
 * resource keeps {@code movie_diagnostics_supported} false, so release never
 * probes for the class.
 */
final class MovieDiagnosticsBridge {
    private static final String CLASS = "wc3.recomp.MovieDiagnostics";

    private MovieDiagnosticsBridge() {
    }

    static Object attach(Activity activity, ExoPlayer player, String path,
                         AviFile avi, SurfaceView surfaceView) {
        if (!activity.getResources().getBoolean(
                R.bool.movie_diagnostics_supported)) {
            return null;
        }
        try {
            Class<?> diagnostics = Class.forName(CLASS);
            return diagnostics.getMethod("attach", ExoPlayer.class,
                            String.class, AviFile.class, SurfaceView.class)
                    .invoke(null, player, path, avi, surfaceView);
        } catch (Throwable ignored) {
            return null;
        }
    }

    static void detach(Object token) {
        if (token == null) return;
        try {
            Class<?> diagnostics = Class.forName(CLASS);
            diagnostics.getMethod("detach", Object.class)
                    .invoke(null, token);
        } catch (Throwable ignored) {
        }
    }
}
