package wc3.recomp;

import com.google.androidgamesdk.GameActivity;

/**
 * Android host entry point.  GameActivity owns the SurfaceView and native
 * window; libwc3gameactivity.so provides android_main and the platform/guest
 * startup path.  The game loop and HLE are linked in only when their gates are
 * green, so this class deliberately contains no second copy of that logic.
 */
public final class Wc3GameActivity extends GameActivity {
    /**
     * Called from android_main through JNI after platform/memory/PE checks.
     * The text is already written to the native run log; rendering a second
     * in-game panel would cover the game surface, so this hook is intentionally
     * silent in the product activity.
     */
    public static void setDiagnostics(final String text) {
    }
}
