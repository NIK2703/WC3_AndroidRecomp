package com.wc3.recomp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import com.google.androidgamesdk.GameActivity;

import java.lang.ref.WeakReference;

/**
 * Android host entry point.  GameActivity owns the SurfaceView and native
 * window; libwc3gameactivity.so provides android_main and the platform/guest
 * startup path.  The game loop and HLE are linked in only when their gates are
 * green, so this class deliberately contains no second copy of that logic.
 */
public final class Wc3GameActivity extends GameActivity {
    private static WeakReference<Wc3GameActivity> sInstance;
    private static volatile String sPendingDiagnostics;

    private TextView mDiagnostics;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = new WeakReference<>(this);
        super.onCreate(savedInstanceState);

        // GameActivity wraps its SurfaceView in a FrameLayout.  Keep that
        // hierarchy intact and put the diagnostic panel above the surface; the
        // native startup path updates it after platform/memory/PE checks.
        ViewGroup root = null;
        if (mSurfaceView != null && mSurfaceView.getParent() instanceof ViewGroup) {
            root = (ViewGroup) mSurfaceView.getParent();
        }
        if (root != null) {
            mDiagnostics = new TextView(this);
            mDiagnostics.setText(sPendingDiagnostics != null
                    ? sPendingDiagnostics
                    : "WC3 recomp diagnostic\nnative host starting...");
            mDiagnostics.setTextColor(Color.WHITE);
            mDiagnostics.setTextSize(14.0f);
            mDiagnostics.setPadding(32, 32, 32, 32);
            mDiagnostics.setBackgroundColor(0xB0000000);
            mDiagnostics.setClickable(false);
            mDiagnostics.setFocusable(false);
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP | Gravity.START);
            root.addView(mDiagnostics, params);
        }
    }

    /**
     * Called from android_main through JNI.  The method is static because the
     * native thread has no stable activity reference and must not outlive it.
     */
    public static void setDiagnostics(final String text) {
        sPendingDiagnostics = text;
        final Wc3GameActivity activity = sInstance != null ? sInstance.get() : null;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() -> {
            if (activity.mDiagnostics != null) {
                activity.mDiagnostics.setText(text);
            }
        });
    }

    @Override
    protected void onDestroy() {
        if (sInstance != null && sInstance.get() == this) {
            sInstance = null;
        }
        super.onDestroy();
    }
}
