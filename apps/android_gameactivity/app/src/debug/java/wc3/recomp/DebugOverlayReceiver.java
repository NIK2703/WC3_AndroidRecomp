package wc3.recomp;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Debug-only T2 switch for the overlay SurfaceView A/B experiment.
 *
 * <p>The release resource keeps {@code overlay_experiment_supported} false, so
 * the host ignores the preference even if the file exists.  The receiver only
 * exists in the debug source set.
 */
public final class DebugOverlayReceiver extends BroadcastReceiver {
    public static final String ACTION = "wc3.recomp.action.DEBUG_OVERLAY";
    public static final String PREFERENCES = "wc3_debug";
    public static final String KEY_HIDDEN = "overlay_hidden";

    @Override
    public void onReceive(final Context context, final Intent intent) {
        if (context == null) return;
        final boolean hide = intent == null || intent.getBooleanExtra("hide", true);
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                .edit()
                .putBoolean(KEY_HIDDEN, hide)
                .commit();
        Log.i("wc3overlay", "debug overlay hidden=" + hide
                + "; restart the game to apply");
    }
}
